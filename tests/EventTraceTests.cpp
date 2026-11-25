#include "common/eventTrace.h"
#include "kernel/conditionWaitTrace.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

// Header/helper tests only. Bounded sinks replace Record; these tests do not
// claim to exercise perfStats transport, scheduler integration or guest waits.
namespace S = Common::SourceTrace;
namespace E = Common::EventTrace;
namespace W = Libs::LibKernel::ConditionWaitTrace;
namespace {
std::mutex sink_mutex;
std::vector<E::Event> emitted;
std::vector<S::Event> source_emitted;
void Check(bool ok, const char* what) {
	if (!ok) { std::fprintf(stderr, "FAIL: event trace: %s\n", what); std::fflush(nullptr); std::_Exit(1); }
}
void TestGatesAndOneShotArm() {
	Check(!S::Enabled() && !E::Enabled() && !S::Armed(), "diagnostics default off");
	Check(S::ArmTimestamp() == 1, "legacy enabled source mode starts immediately");
	Check(E::CaptureCallback(1, 2).id == 0, "disabled capture has no callback identity");
	Check(!S::TryArm(100), "disabled source cannot arm");
	S::Detail::g_enabled.store(true);
	Check(S::Armed() && !E::Enabled(), "source enable does not implicitly enable event tracing");
	S::Detail::g_arm_ns.store(0); // fixture startup, before any live scope
	E::Detail::g_enabled.store(true);
	Check(!S::Armed() && !E::Enabled() && E::CaptureCallback(1, 2).id == 0,
	      "configured but unarmed diagnostics stay inert");
	const auto prearm_callback = E::CaptureCallback(1, 2);
	Check(!S::TryArm(0) && !S::TryArm(1), "reserved arm sentinel values rejected");
	S::Detail::g_observed_threads.store(uint64_t {41} << 32u);
	Check(S::Detail::g_condition_threads.Admit(42), "fixture historical coverage registered");
	Check(!E::ThreadEligible(41) && !E::ThreadEligible(42), "coverage does not bypass arming");
	std::atomic<unsigned> winners {0};
	std::array<uint64_t, 16> attempts {};
	std::vector<std::thread> armers;
	for (size_t i = 0; i < attempts.size(); ++i) {
		armers.emplace_back([&, i] {
			attempts[i] = S::NowNs();
			if (S::TryArm(attempts[i])) ++winners;
		});
	}
	for (auto& thread: armers) thread.join();
	const auto arm_ns = S::ArmTimestamp();
	bool from_attempt = false;
	for (auto value: attempts) from_attempt |= arm_ns == value;
	Check(winners == 1 && from_attempt && S::Armed() && E::Enabled(),
	      "concurrent arming has one winner and publishes its actual timestamp");
	E::PriorityTickSelection prearm_selection;
	prearm_selection.ObserveQueued(prearm_callback, 1, 2);
	Check(!prearm_selection.Take(2), "arming never recaptures an already empty callback context");
	Check(E::ThreadEligible(41) && E::ThreadEligible(42) && !E::ThreadEligible(43) &&
	      !E::ThreadEligible(0), "event coverage is producer or historically observed participant");
	alignas(8) uint64_t owner = 0;
	W::State live_wait;
	Check(W::Begin(live_wait, &owner, 41, 1), "actual condition helper starts after arm");
	const auto id = live_wait.id, begin = live_wait.begin_ns;
	S::Detail::RecordBudget<2> existing_budget;
	Check(existing_budget.TryAdmit(), "fixture budget consumed before repeated arm");
	Check(!S::TryArm(S::NowNs()) && S::ArmTimestamp() == arm_ns &&
	      live_wait.id == id && live_wait.begin_ns == begin && existing_budget.Attempted() == 1,
	      "rearming cannot reset live wait identity, timestamp or independent quota");
	auto completion = W::CaptureReady(live_wait, &owner);
	W::Complete(completion, 3, 0);
	Check(source_emitted.size() == 1 && source_emitted[0].request_id == id &&
	      source_emitted[0].begin_ns == begin, "live completion retains its original invocation");
}
void TestCapturedOriginAndNamespaces() {
	S::t_context = {71, 81, 3, 4};
	const auto a = E::CaptureCallback(901, 77, -3, 15, 0);
	const auto b = E::CaptureCallback(901, 77, -3, 15, 0);
	const auto c = E::CaptureCallback(902, 77, -3, 15, 0);
	Check(a.id != 0 && a.id != b.id && a.id != c.id && b.id != c.id,
	      "callbacks sharing a tick or using different schedulers have distinct identities");
	const auto schedule_ns = a.scheduled_ns;
	std::thread callback([a] {
		S::t_context = {999, 888, 77, 66}; // callback-thread TLS must never replace origin
		E::EmitCallback(E::Kind::CallbackBegin, a, 0, S::NowNs());
		E::EmitCallback(E::Kind::CallbackEnd, a, S::NowNs(), S::NowNs());
		E::EmitCallback(E::Kind::CallbackBegin, {}); // pre-arm/direct unknown stays zero
	});
	callback.join();
	Check(emitted.size() == 2, "zero explicit context never invents callback ancestry");
	for (const auto& event: emitted) {
		Check(event.id == a.id && event.scheduler_id == 901 && event.tick == 77 &&
		      event.submission_id == 71 && event.request_id == 81 && event.index == 3 && event.mode == 4 &&
		      event.ident == static_cast<uint64_t>(int64_t {-3}) && event.data == 15 &&
		      event.extra == schedule_ns, "explicit by-value callback context survives thread/TLS changes");
	}
	const auto preexisting = E::NewIdentity();
	Check(!S::TryArm(S::NowNs()) && E::NewIdentity() > preexisting,
	      "repeated arm never resets identity sequence");
	E::Detail::g_enabled.store(false);
	E::EmitCallback(E::Kind::CallbackBegin, a);
	Check(emitted.size() == 2 && E::CaptureCallback(901, 77).id == 0,
	      "event-disabled gate prevents callback emission and capture");
	E::Detail::g_enabled.store(true);
}
void TestPriorityTickSelection() {
	E::PriorityTickSelection selection;
	Check(!selection.Take(10), "a tick without a queued captured callback is not selected");
	const auto generic = E::CaptureCallback(91, 10);
	const auto interrupt = E::CaptureCallback(91, 10, 0, 0, 0);
	Check(!selection.Take(10), "capture alone does not select until the callback is queued");
	selection.ObserveQueued(generic, 91, 10);
	selection.ObserveQueued(interrupt, 91, 10);
	const bool first_job = selection.Take(10);
	Check(first_job && !selection.Take(10), "multiple callbacks select their tick once and Submit resets it");
	selection.ObserveQueued({}, 91, 11);
	const bool second_job = selection.Take(11);
	const auto next = E::CaptureCallback(91, 12);
	selection.ObserveQueued(next, 91, 12);
	const bool third_job = selection.Take(12);
	Check(first_job && !second_job && third_job,
	      "by-value queued jobs retain independent selection as the producer advances");
	selection.ObserveQueued(generic, 92, 10);
	Check(!selection.Take(10), "a same-number tick on another scheduler cannot select this job");
	selection.ObserveQueued(generic, 91, 11);
	Check(!selection.Take(11), "an old captured tick is not relabelled to the new queue tick");
	selection.ObserveQueued(generic, 91, 10);
	Check(!selection.Take(11) && !selection.Take(10), "a mismatching Submit clears stale selection");
	E::PriorityTickSelection other;
	selection.ObserveQueued(interrupt, 91, 10);
	Check(!other.Take(10) && selection.Take(10), "scheduler instances do not share selection state");
	selection.ObserveQueued(generic, 91, 10);
	Check(selection.Take(10), "generic priority callbacks retain their complete backlog chain");
	const auto before = E::Detail::g_unselected_ticks.load();
	E::NoteUnselectedTick(0);
	E::NoteUnselectedTick(S::ArmTimestamp() - 1);
	Check(E::Detail::g_unselected_ticks.load() == before,
	      "unselected pre-arm handoffs do not enter active selection counters");
	E::NoteUnselectedTick(S::NowNs());
	Check(E::Detail::g_unselected_ticks.load() == before + 1,
	      "one active omitted tick is explicitly counted separately from record admission");
	E::Detail::g_enabled.store(false);
	E::NoteUnselectedTick(S::NowNs());
	Check(E::Detail::g_unselected_ticks.load() == before + 1,
	      "default-off unselected ticks do not update diagnostics");
	E::Detail::g_enabled.store(true);
}
E::Event Payload(uint64_t id) {
	E::Event event {};
	event.kind = E::Kind::Delivery; event.id = id; event.parent_id = id ^ 0xabcde;
	event.filter = -14; event.result = -9; event.count = 1;
	event.tid = static_cast<uint32_t>(id >> 32u); event.queue_id = id + 19;
	event.registration_id = id + 23; event.scheduler_id = id + 29;
	event.tick = id + 31; event.submission_id = id + 37; event.request_id = id + 41;
	event.time_ns = id + 43; event.begin_ns = id + 47; event.data = ~id;
	event.ident = id + 53; event.extra = id + 59;
	return event;
}
void TestTypedConcurrentRing() {
	constexpr size_t Producers = 8, PerProducer = 2000;
	S::Detail::Ring<127, E::Event> ring;
	std::array<std::array<bool, PerProducer>, Producers> seen {};
	std::atomic<unsigned> done {0};
	std::atomic<bool> go {false};
	std::vector<std::thread> producers;
	for (size_t p = 0; p != Producers; ++p) {
		producers.emplace_back([&, p] {
			while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
			for (size_t n = 0; n != PerProducer; ++n) {
				const uint64_t id = (uint64_t {p + 1} << 32u) | (n + 1);
				(void)ring.TryPush(Payload(id));
			}
			done.fetch_add(1, std::memory_order_release);
		});
	}
	uint64_t consumed = 0;
	const auto consume = [&](const E::Event& e) {
		const auto p = (e.id >> 32u) - 1, n = (e.id & 0xffffffffu) - 1;
		Check(p < Producers && n < PerProducer && !seen[p][n], "concurrent payload identity valid and unique");
		seen[p][n] = true;
		const auto expected = Payload(e.id);
		Check(e.kind == expected.kind && e.filter == -14 && e.result == -9 && e.count == 1 &&
		      e.parent_id == expected.parent_id && e.tid == expected.tid &&
		      e.queue_id == expected.queue_id && e.registration_id == expected.registration_id &&
		      e.scheduler_id == expected.scheduler_id && e.tick == expected.tick &&
		      e.submission_id == expected.submission_id && e.request_id == expected.request_id &&
		      e.time_ns == expected.time_ns && e.begin_ns == expected.begin_ns &&
		      e.data == expected.data && e.ident == expected.ident && e.extra == expected.extra,
		      "typed ring never tears the larger event payload during concurrent reuse");
		++consumed;
	};
	go.store(true, std::memory_order_release);
	while (done.load(std::memory_order_acquire) != Producers) {
		ring.Drain(consume); std::this_thread::yield();
	}
	for (auto& thread: producers) thread.join();
	ring.Drain(consume);
	Check(consumed == ring.Captured() && consumed != 0 &&
	      ring.Captured() + ring.Dropped() == Producers * PerProducer,
	      "captured and dropped accounting covers every concurrent attempt");
	S::Detail::Ring<2, E::Event> full;
	Check(full.TryPush(Payload(1)) && full.TryPush(Payload(2)) && !full.TryPush(Payload(3)),
	      "full diagnostic ring drops rather than overwriting live records");
	uint64_t sum = 0;
	Check(full.Drain([&](const auto& e) { sum += e.id; }) == 2 && sum == 3 && full.Dropped() == 1,
	      "overflow preserves both accepted records");
	Check(full.TryPush(Payload(4)), "drained diagnostic ring can be reused");
}
}
namespace Common::EventTrace {
void Record(Event event) noexcept { std::lock_guard lock(sink_mutex); emitted.push_back(event); }
}
namespace Common::SourceTrace {
void Record(Event event) noexcept { std::lock_guard lock(sink_mutex); source_emitted.push_back(event); }
}
int main() {
	TestGatesAndOneShotArm(); TestCapturedOriginAndNamespaces(); TestPriorityTickSelection(); TestTypedConcurrentRing();
	std::puts("PASS: event trace helpers (one-shot arm, captured origin, priority tick selection, concurrent typed ring)");
}
