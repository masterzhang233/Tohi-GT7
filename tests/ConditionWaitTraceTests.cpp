#include "kernel/conditionWaitTrace.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#include <thread>

// Tests the actual diagnostic helper, using a bounded Record sink. This is not an
// integration test of the emulator's native mutexes, pthread wrappers, timed-wait
// APIs, or source-trace ring/reporter. The caller supplies IDs to this helper, so
// distinct fixture IDs stand in for the native IDs supplied by pthread.cpp.
namespace {
namespace Trace = Common::SourceTrace;
namespace WaitTrace = Libs::LibKernel::ConditionWaitTrace;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr uint32_t WaiterTid = 101, NotifierTid = 202;
alignas(8) uint64_t fixture_owner = 0;
thread_local uint32_t recording_tid = NotifierTid;

struct Recorded {
	std::array<Trace::Event, 32> events {};
	size_t count = 0;
	bool overflow = false;
};
std::mutex records_mutex;
Recorded records;

[[noreturn]] void Fail(const char* message) {
	std::fprintf(stderr, "FAIL: condition wait trace: %s\n", message);
	std::fflush(nullptr);
	// A synchronization regression must fail rather than hang during thread joins.
	std::_Exit(1);
}

void Check(bool condition, const char* message) {
	if (!condition) Fail(message);
}

void RecordForFixture(Trace::Event event) noexcept {
	if (event.tid == 0) event.tid = recording_tid;
	std::lock_guard lock(records_mutex);
	if (records.count == records.events.size()) {
		records.overflow = true;
		return;
	}
	records.events[records.count++] = event;
}

Recorded Snapshot() {
	std::lock_guard lock(records_mutex);
	Check(!records.overflow, "bounded fixture event sink overflowed");
	return records;
}

void Reset(bool enabled, uint32_t producer_tid) {
	std::lock_guard lock(records_mutex);
	records = {};
	Trace::t_context = {};
	Trace::Detail::g_enabled.store(enabled, std::memory_order_relaxed);
	Trace::Detail::g_observed_threads.store(uint64_t {producer_tid} << 32u, std::memory_order_relaxed);
}

bool Empty(const WaitTrace::State& state) {
	return state.owner.load(std::memory_order_acquire) == 0 && state.id == 0 &&
	       state.begin_ns == 0 && state.waiter_tid == 0 && state.api == 0;
}

template <typename Predicate> void WaitFor(Predicate&& ready, const char* message) {
	const auto deadline = Clock::now() + 5s;
	while (!ready()) {
		if (Clock::now() >= deadline) Fail(message);
		std::this_thread::sleep_for(1ms);
	}
}

void CheckWait(const Trace::Event& event, uint64_t id, uint32_t flags,
               uint64_t poll_returns, int native_result) {
	Check(event.kind == Trace::Kind::CondWait && event.flags == flags,
	      "wait kind/API/abort flags retained");
	Check(event.request_id == id && event.submission_id == 0 && event.tid == WaiterTid,
	      "wait retains opaque identity and waiter, without a submission association");
	Check(event.begin_ns != 0 && event.begin_ns <= event.work_end_ns &&
	      event.work_end_ns <= event.end_ns, "wait timestamps are ordered");
	Check(event.arg0 == poll_returns && event.arg1 == static_cast<uint32_t>(native_result),
	      "wait retains poll count and native result");
}

void TestInactiveGates() {
	struct Gate { bool enabled; uint32_t producer, waiter; };
	for (const auto gate: {Gate {false, WaiterTid, WaiterTid},
	                      Gate {true, WaiterTid, NotifierTid},
	                      Gate {true, 0, WaiterTid}, Gate {true, 0, 0}}) {
		Reset(gate.enabled, gate.producer);
		WaitTrace::State state;
		Check(!WaitTrace::Begin(state, &fixture_owner, gate.waiter, 1),
		      "disabled/nonproducer wait is inactive");
		Check(Empty(state), "disabled/nonproducer wait must not retain diagnostic state");
		WaitTrace::PublishWake(WaitTrace::MakeWake(state, &fixture_owner, 17, 1));
		WaitTrace::Complete(WaitTrace::CaptureReady(state, &fixture_owner), 3, 0);
		Check(Empty(state) && Snapshot().count == 0,
		      "disabled/nonproducer wait must not publish wake or wait events");
	}
	Reset(true, WaiterTid);
	WaitTrace::State state;
	alignas(8) char invalid_owner[2] {};
	Check(!WaitTrace::Begin(state, nullptr, WaiterTid, 1) &&
	      !WaitTrace::Begin(state, &invalid_owner[1], WaiterTid, 1) && Empty(state),
	      "null and unaligned condition tags cannot become active owners");
}

void TestCopiedLifetimeAndResults() {
	Reset(true, WaiterTid);
	WaitTrace::State state;
	Check(WaitTrace::Begin(state, &fixture_owner, WaiterTid, 1), "first wait is active");
	const uint64_t first_id = state.id;
	Check(first_id != 0, "eligible producer has a wait identity");
	const auto first = WaitTrace::CaptureReady(state, &fixture_owner);
	Check(Empty(state) && Snapshot().count == 0,
	      "ready capture clears state without publishing before reacquisition");
	WaitTrace::PublishWake(WaitTrace::MakeWake(state, &fixture_owner, 8, 1));
	Check(Snapshot().count == 0, "cleared wait cannot receive a stale notification record");

	// Reusing waiter storage must not mutate a locally captured, incomplete event.
	Check(WaitTrace::Begin(state, &fixture_owner, WaiterTid, 1), "reused state accepts fresh wait");
	const uint64_t second_id = state.id;
	Check(second_id > first_id, "consecutive waits have fresh increasing opaque IDs");
	WaitTrace::Complete(first, (uint64_t {1} << 32) + 7, EINVAL);
	Check(state.id == second_id, "completing copied event leaves next wait intact");
	const auto aborted = WaitTrace::CaptureReady(state, &fixture_owner, true);
	WaitTrace::Complete(aborted, 0, EPERM);
	Check(Empty(state), "aborted wait clears state");
	// Completing an empty capture is harmless and must not invent another wait.
	WaitTrace::Complete(WaitTrace::CaptureReady(state, &fixture_owner), 0, 0);
	const auto captured = Snapshot();
	Check(captured.count == 2, "error and aborted helpers each publish exactly one wait");
	CheckWait(captured.events[0], first_id, 1, (uint64_t {1} << 32) + 7, EINVAL);
	CheckWait(captured.events[1], second_id, 1u | 256u, 0, EPERM);
	Check(captured.events[0].begin_ns == first.begin_ns &&
	      captured.events[0].work_end_ns == first.work_end_ns,
	      "completion preserves the captured condition-wait interval");
}

void TestWakeKinds() {
	Reset(true, WaiterTid);
	uint64_t previous_id = 0;
	for (uint32_t kind = 1; kind <= 3; ++kind) {
		WaitTrace::State state;
		Check(WaitTrace::Begin(state, &fixture_owner, WaiterTid, 1), "wake-kind wait is active");
		const uint64_t id = state.id;
		const uint64_t begin_ns = state.begin_ns;
		Check(id > previous_id, "each signal-kind fixture has its own wait identity");
		previous_id = id;
		Trace::t_context.submission_id = 900 + kind;
		const auto wake = WaitTrace::MakeWake(state, &fixture_owner, 70 + kind, kind);
		Trace::t_context = {}; // Captured notifier context must survive publication delay.
		Check(Snapshot().count == 2 * (kind - 1), "constructing wake does not publish it");
		WaitTrace::PublishWake(wake);
		WaitTrace::Complete(WaitTrace::CaptureReady(state, &fixture_owner), 1, 0);
		const auto captured = Snapshot();
		const auto& event = captured.events[2 * (kind - 1)];
		Check(event.kind == Trace::Kind::CondWake && event.flags == kind &&
		      event.request_id == id && event.submission_id == 900 + kind,
		      "signal, targeted signal and broadcast retain distinct kinds and wait IDs");
		Check(event.tid == NotifierTid && event.arg0 == WaiterTid && event.arg1 == 70 + kind,
		      "wake identifies notifier, waiter and sequence");
		Check(event.begin_ns >= begin_ns && event.begin_ns != 0 &&
		      event.begin_ns == event.work_end_ns && event.begin_ns == event.end_ns,
		      "notification is represented by one timestamp");
		CheckWait(captured.events[2 * (kind - 1) + 1], id, 1, 1, 0);
		Check(event.begin_ns <= captured.events[2 * (kind - 1) + 1].work_end_ns,
		      "wake precedes its wait's ready capture");
	}
	Check(Snapshot().count == 6, "wake-kind fixture records three distinct pairs");
}

void TestConditionAssociation() {
	Reset(true, WaiterTid);
	alignas(8) uint64_t owner_a = 0, owner_b = 0;
	WaitTrace::State state;
	Check(WaitTrace::Begin(state, &owner_a, WaiterTid, 1), "associated wait is active");
	const uint64_t id = state.id, begin_ns = state.begin_ns;
	WaitTrace::PublishWake(WaitTrace::MakeWake(state, &owner_b, 1, 1));
	WaitTrace::Complete(WaitTrace::CaptureReady(state, &owner_b), 0, 0);
	Check(Snapshot().count == 0 && state.id == id && state.begin_ns == begin_ns,
	      "different condition cannot wake, capture or clear this wait");
	WaitTrace::PublishWake(WaitTrace::MakeWake(state, &owner_a, 2, 1));
	WaitTrace::Complete(WaitTrace::CaptureReady(state, &owner_a), 1, 0);
	const auto captured = Snapshot();
	Check(captured.count == 2 && captured.events[0].request_id == id,
	      "matching condition retains its wake association");
	CheckWait(captured.events[1], id, 1, 1, 0);
}

void TestBorrowedIdentityExclusion() {
	Reset(true, WaiterTid);
	WaitTrace::State never_started;
	WaitTrace::MarkBorrowed(never_started);
	Check(!WaitTrace::Begin(never_started, &fixture_owner, WaiterTid, 1),
	      "borrowed identity cannot start a fresh attributed wait");
	WaitTrace::State active;
	Check(WaitTrace::Begin(active, &fixture_owner, WaiterTid, 1), "wait active before borrowing");
	const auto id = active.id;
	// A signal helper can borrow this guest identity without taking its condition mutex.
	std::thread helper([&] { WaitTrace::MarkBorrowed(active); });
	helper.join();
	Check(active.id == id, "borrowed marking does not modify condition-owned POD");
	WaitTrace::PublishWake(WaitTrace::MakeWake(active, &fixture_owner, 8, 1));
	WaitTrace::Complete(WaitTrace::CaptureReady(active, &fixture_owner), 2, 0);
	Check(Empty(active) && active.borrowed_identity.load(std::memory_order_acquire),
	      "completion clears occurrence but retains borrowed identity exclusion");
	const auto captured = Snapshot();
	Check(captured.count == 1, "borrowed occurrence cannot attribute a notifier");
	CheckWait(captured.events[0], id, 1u | 512u, 2, 0);
	alignas(8) uint64_t next_owner = 0;
	Check(!WaitTrace::Begin(active, &fixture_owner, WaiterTid, 1) &&
	      !WaitTrace::Begin(active, &next_owner, WaiterTid, 1),
	      "helper spanning later waits cannot regain attribution on reused identity");
	Check(Snapshot().count == 1, "excluded later waits emit no invented events");
}

void TestNestedSuppressionAndReuse() {
	// Same/different condition nesting, the timed-wrapper hook, and a producer
	// change must all preserve the active outer state and mark it ambiguous.
	for (unsigned scenario = 0; scenario != 4; ++scenario) {
		Reset(true, WaiterTid);
		alignas(8) uint64_t owner_a = 0, owner_b = 0;
		WaitTrace::State state;
		WaitTrace::NoteNested(state); // No active outer wait: no lasting suppression.
		Check(WaitTrace::Begin(state, &owner_a, WaiterTid, 1), "outer wait is active");
		const uint64_t id = state.id, begin_ns = state.begin_ns;
		if (scenario == 2) {
			WaitTrace::NoteNested(state);
		} else {
			if (scenario == 3) {
				Trace::Detail::g_observed_threads.store(uint64_t {NotifierTid} << 32u, std::memory_order_relaxed);
			}
			const void* nested_owner = scenario == 0 ? &owner_a : &owner_b;
			Check(!WaitTrace::Begin(state, nested_owner, WaiterTid, 1),
			      "nested wait cannot replace an active outer identity");
		}
		Check(state.id == id && state.begin_ns == begin_ns &&
		      state.waiter_tid == WaiterTid && state.api == 1,
		      "nested entry does not modify outer wait payload");
		// Rejected nested waits skip CaptureReady/Complete. Even after returning
		// from the nested scope, no wake is confidently attributed to this outer.
		WaitTrace::PublishWake(WaitTrace::MakeWake(state, &owner_a, 1, 1));
		WaitTrace::PublishWake(WaitTrace::MakeWake(state, &owner_b, 2, 2));
		Check(Snapshot().count == 0, "nested entry suppresses all remaining outer wake attribution");
		WaitTrace::Complete(WaitTrace::CaptureReady(state, &owner_a), 4, 0);
		Check(Empty(state), "outer completion clears state after nested ambiguity");
		CheckWait(Snapshot().events[0], id, 1u | 512u, 4, 0);

		Trace::Detail::g_observed_threads.store(uint64_t {WaiterTid} << 32u, std::memory_order_relaxed);
		Check(WaitTrace::Begin(state, &owner_a, WaiterTid, 1),
		      "completed ambiguous outer permits a new independent wait");
		const uint64_t fresh_id = state.id;
		Check(fresh_id > id, "new wait after nesting has a fresh identity");
		WaitTrace::PublishWake(WaitTrace::MakeWake(state, &owner_a, 3, 1));
		WaitTrace::Complete(WaitTrace::CaptureReady(state, &owner_a), 1, 0);
		const auto captured = Snapshot();
		Check(captured.count == 3 && captured.events[1].kind == Trace::Kind::CondWake &&
		      captured.events[1].request_id == fresh_id,
		      "nested ambiguity does not leak into the next wait's wake");
		CheckWait(captured.events[2], fresh_id, 1, 1, 0);
	}
}

void TestThreadedNestedWait() {
	Reset(true, WaiterTid);
	alignas(8) uint64_t owner_a = 0, owner_b = 0;
	std::mutex outer_mutex, nested_mutex;
	std::condition_variable outer_condition, nested_condition;
	WaitTrace::State state;
	uint64_t outer_sequence = 0;
	bool nested_ready = false;
	std::atomic<bool> nested_registered {false}, nested_active {false};
	uint64_t id = 0, begin_ns = 0;
	{
		std::lock_guard lock(outer_mutex);
		Check(WaitTrace::Begin(state, &owner_a, WaiterTid, 1), "threaded outer wait is active");
		id = state.id;
		begin_ns = state.begin_ns;
	}
	// Model a guest APC callback with A's lock released. Its nested B wait uses
	// the same per-thread diagnostic state under a different condition lock.
	std::thread notifier([&] {
		recording_tid = NotifierTid;
		WaitFor([&] { return nested_registered.load(std::memory_order_acquire); },
		        "nested waiter did not register");
		{
			// Registration occurs with B locked. Acquiring it here ensures the
			// nested waiter has entered the real cv wait and released that lock.
			std::lock_guard lock(nested_mutex);
			Check(!nested_ready, "nested condition has not yet been signaled");
		}
		{
			std::lock_guard lock(outer_mutex);
			Check(nested_active.load(std::memory_order_acquire),
			      "outer notification occurs while nested wait is active");
			const auto wake = WaitTrace::MakeWake(state, &owner_a, ++outer_sequence, 1);
			Check(wake.request_id == 0, "outer notification during nested wait is suppressed");
			outer_condition.notify_one();
			WaitTrace::PublishWake(wake);
		}
		{
			std::lock_guard lock(nested_mutex);
			const auto wake = WaitTrace::MakeWake(state, &owner_b, 1, 1);
			Check(wake.request_id == 0, "nested condition cannot borrow outer identity");
			nested_ready = true;
			nested_condition.notify_one();
			WaitTrace::PublishWake(wake);
		}
	});
	{
		std::unique_lock lock(nested_mutex);
		Check(!WaitTrace::Begin(state, &owner_b, WaiterTid, 1),
		      "nested B wait is excluded from outer trace state");
		nested_active.store(true, std::memory_order_release);
		nested_registered.store(true, std::memory_order_release);
		Check(nested_condition.wait_for(lock, 5s, [&] { return nested_ready; }),
		      "nested condition did not receive its real notification");
		nested_active.store(false, std::memory_order_release);
		// Begin returned false: this nested path must not capture/clear state.
	}
	notifier.join();
	Trace::Event wait;
	{
		std::lock_guard lock(outer_mutex);
		Check(outer_sequence == 1 && state.id == id && state.begin_ns == begin_ns &&
		      state.waiter_tid == WaiterTid && state.api == 1,
		      "concurrent outer notification and nested B wait preserve outer payload");
		WaitTrace::PublishWake(WaitTrace::MakeWake(state, &owner_a, 2, 1));
		Check(Snapshot().count == 0, "wake suppression persists after nested B returns");
		wait = WaitTrace::CaptureReady(state, &owner_a);
		Check(Empty(state), "only outer completion clears the nested-affected state");
	}
	WaitTrace::Complete(wait, 1, 0);
	const auto captured = Snapshot();
	Check(captured.count == 1, "nested handoff records only the uncertain outer wait");
	CheckWait(captured.events[0], id, 1u | 512u, 1, 0);
	std::puts("Threaded nested helper: outer identity retained, wake events suppressed, uncertain=1");
}

void TestThreadedSignalAndReacquire() {
	Reset(true, WaiterTid);
	std::mutex guest_mutex, condition_mutex;
	std::condition_variable condition;
	alignas(8) uint64_t owner = 0;
	WaitTrace::State state;
	uint64_t sequence = 0;
	std::atomic<bool> registered {false}, captured_ready {false}, completed {false};
	std::atomic<uint64_t> poll_returns {0};

	std::thread waiter([&] {
		recording_tid = WaiterTid;
		std::unique_lock guest_lock(guest_mutex);
		std::unique_lock condition_lock(condition_mutex);
		const uint64_t initial_sequence = sequence;
		Check(WaitTrace::Begin(state, &owner, WaiterTid, 1), "threaded wait is active");
		registered.store(true, std::memory_order_release);
		guest_lock.unlock();
		uint64_t polls = 0;
		while (sequence == initial_sequence) {
			condition.wait_for(condition_lock, 1ms);
			poll_returns.store(++polls, std::memory_order_release);
		}
		const auto event = WaitTrace::CaptureReady(state, &owner);
		condition_lock.unlock();
		captured_ready.store(true, std::memory_order_release);
		guest_lock.lock();
		WaitTrace::Complete(event, polls, 0);
		completed.store(true, std::memory_order_release);
	});

	WaitFor([&] { return registered.load(std::memory_order_acquire) &&
	                     poll_returns.load(std::memory_order_acquire) >= 2; },
	        "waiter did not register and return from condition polling");
	Check(Snapshot().count == 0, "condition polling alone does not invent wake/wait completion");
	std::unique_lock guest_lock(guest_mutex);
	uint64_t wait_id = 0;
	{
		std::lock_guard condition_lock(condition_mutex);
		wait_id = state.id;
		const auto wake = WaitTrace::MakeWake(state, &owner, ++sequence, 1);
		condition.notify_one();
		WaitTrace::PublishWake(wake);
	}
	WaitFor([&] { return captured_ready.load(std::memory_order_acquire); },
	        "notified waiter did not capture its ready phase");
	{
		std::lock_guard condition_lock(condition_mutex);
		Check(Empty(state), "wait identity is cleared while guest mutex remains held");
		WaitTrace::PublishWake(WaitTrace::MakeWake(state, &owner, sequence + 1, 1));
	}
	Check(!completed.load(std::memory_order_acquire) && Snapshot().count == 1,
	      "wait completion is not published before guest mutex reacquisition");
	std::this_thread::sleep_for(10ms);
	const uint64_t release_begin_ns = Trace::NowNs();
	guest_lock.unlock();
	WaitFor([&] { return completed.load(std::memory_order_acquire); },
	        "waiter did not complete after guest mutex release");
	waiter.join();

	const auto captured = Snapshot();
	Check(captured.count == 2, "threaded handoff records one real wake and one completed wait");
	const auto& wake = captured.events[0];
	const auto& wait = captured.events[1];
	CheckWait(wait, wait_id, 1, poll_returns.load(std::memory_order_relaxed), 0);
	Check(wake.kind == Trace::Kind::CondWake && wake.flags == 1 &&
	      wake.request_id == wait_id && wake.tid == NotifierTid &&
	      wake.arg0 == WaiterTid && wake.arg1 == 1,
	      "cross-thread wake joins the completed wait by opaque identity");
	Check(wait.begin_ns <= wake.begin_ns && wake.begin_ns <= wait.work_end_ns &&
	      wait.work_end_ns < release_begin_ns && release_begin_ns <= wait.end_ns,
	      "notification, readiness and delayed reacquisition remain separately ordered");
	std::printf("Threaded helper: polls=%llu reacquire_ms=%.3f events=2\n",
	            static_cast<unsigned long long>(wait.arg0),
	            static_cast<double>(wait.end_ns - wait.work_end_ns) / 1'000'000.0);
}
} // namespace

namespace Common::SourceTrace {
void Record(Event event) noexcept { RecordForFixture(event); }
} // namespace Common::SourceTrace

int main() {
	Check(!Trace::Enabled() && Trace::ProducerThreadId() == 0, "tracing defaults off");
	std::atomic<bool> done {false};
	std::thread watchdog([&] {
		const auto deadline = Clock::now() + 15s;
		while (!done.load(std::memory_order_acquire)) {
			if (Clock::now() >= deadline) Fail("fixture watchdog expired");
			std::this_thread::sleep_for(10ms);
		}
	});
	TestInactiveGates();
	TestCopiedLifetimeAndResults();
	TestWakeKinds();
	TestConditionAssociation();
	TestBorrowedIdentityExclusion();
	TestNestedSuppressionAndReuse();
	TestThreadedNestedWait();
	TestThreadedSignalAndReacquire();
	Reset(false, 0);
	done.store(true, std::memory_order_release);
	watchdog.join();
	std::puts("PASS: condition wait trace helper (native pthread/timed APIs not exercised)");
	return 0;
}
