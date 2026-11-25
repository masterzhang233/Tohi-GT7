#include "kernel/conditionWaitTrace.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#include <thread>
#include <vector>

// CPU-only tests of production diagnostic helpers. The bounded sink below replaces
// Record's platform caller-ID lookup and reporter I/O; native pthread waits, sampler
// suspension, environment parsing and game behavior are not exercised here.
namespace {
namespace Trace = Common::SourceTrace;
namespace WaitTrace = Libs::LibKernel::ConditionWaitTrace;
using namespace std::chrono_literals;

[[noreturn]] void Fail(const char* message) {
	std::fprintf(stderr, "FAIL: source chain: %s\n", message);
	std::fflush(nullptr);
	std::_Exit(1);
}
void Check(bool condition, const char* message) {
	if (!condition) Fail(message);
}

Trace::Event Flip(uint32_t tid) {
	Trace::Event event {};
	event.kind = Trace::Kind::FlipSubmit;
	event.tid = tid;
	return event;
}
Trace::Event Wake(uint32_t notifier, uint32_t waiter, uint64_t id = 7) {
	Trace::Event event {};
	event.kind = Trace::Kind::CondWake;
	event.tid = notifier;
	event.flags = 1;
	event.request_id = id;
	event.arg0 = waiter;
	event.begin_ns = event.work_end_ns = event.end_ns = Trace::NowNs();
	return event;
}

std::mutex sink_mutex;
std::array<Trace::Event, 32> sink {};
size_t sink_count = 0;
thread_local uint32_t sink_tid = 303;

bool Empty(const WaitTrace::State& state) {
	return state.owner.load(std::memory_order_acquire) == 0 && state.id == 0 &&
	       state.begin_ns == 0 && state.waiter_tid == 0 && state.api == 0;
}

void TestRegistryCapacityAndConcurrency() {
	Trace::Detail::ConditionThreadRegistry<32> registry;
	Check(!registry.Admit(0) && !registry.Contains(0) && registry.Rejected() == 0,
	      "zero is never an admitted identity or a capacity rejection");
	std::atomic<bool> go {false};
	std::atomic<unsigned> unexpected_rejections {0};
	std::vector<std::thread> workers;
	for (unsigned worker = 0; worker != 16; ++worker) workers.emplace_back([&, worker] {
		while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
		// All contenders first try the same empty slot, then overlap all 32 IDs.
		if (!registry.Admit(1)) unexpected_rejections.fetch_add(1);
		for (unsigned i = 0; i != 2'000; ++i) {
			if (!registry.Admit(1 + (i + worker) % 32)) unexpected_rejections.fetch_add(1);
		}
	});
	go.store(true, std::memory_order_release);
	for (auto& worker: workers) worker.join();
	Check(unexpected_rejections.load() == 0 && registry.Admitted() == 32 && registry.Rejected() == 0,
	      "concurrent duplicates occupy exactly one slot per identity");
	std::array<bool, 33> seen {};
	for (size_t i = 0; i != 32; ++i) {
		const auto tid = registry.At(i);
		Check(tid >= 1 && tid <= 32 && !seen[tid], "full registry contains only distinct admitted IDs");
		seen[tid] = true;
	}
	Check(registry.At(32) == 0, "out-of-range dictionary lookup returns no identity");
	for (uint32_t tid = 1; tid <= 32; ++tid) {
		Check(registry.Contains(tid) && registry.Admit(tid), "full registry retains existing membership");
	}
	Check(!registry.Admit(33) && !registry.Admit(33) && registry.Rejected() == 2 &&
	      registry.Admitted() == 32 && !registry.Contains(33),
	      "capacity rejects attempts without evicting or counting distinct rejected threads");
}

void TestBucketIsolation() {
	static_assert(Trace::Detail::ConditionThreadLimit == 32);
	static_assert(Trace::Detail::BaseRecordBudget == 120'000);
	static_assert(Trace::Detail::ExtraConditionRecordBudget == 360'000);
	Trace::Detail::RecordBudget<2> base_budget;
	Trace::Detail::RecordBudget<4> extra_budget;
	Trace::Detail::Ring<2> base_ring, extra_ring;
	const auto primary_wake = Wake(202, 101);
	const auto upstream_wake = Wake(303, 202);
	Trace::Event primary_wait {}, upstream_wait {};
	primary_wait.kind = upstream_wait.kind = Trace::Kind::CondWait;
	primary_wait.tid = 101; upstream_wait.tid = 202;
	Trace::Detail::g_chain_enabled.store(false);
	Check(!Trace::Detail::UseExtraConditionBucket(upstream_wake, 101) &&
	      !Trace::Detail::UseExtraConditionBucket(upstream_wait, 101),
	      "chain-off retains original bucket routing");
	Trace::Detail::g_chain_enabled.store(true);
	Check(!Trace::Detail::UseExtraConditionBucket(Flip(101), 101) &&
	      !Trace::Detail::UseExtraConditionBucket(primary_wait, 101) &&
	      !Trace::Detail::UseExtraConditionBucket(primary_wake, 101) &&
	      Trace::Detail::UseExtraConditionBucket(upstream_wait, 101) &&
	      Trace::Detail::UseExtraConditionBucket(upstream_wake, 101),
	      "current-producer waits/wakes and noncondition events retain base priority");
	const auto push = [&](const Trace::Event& event) {
		if (Trace::Detail::UseExtraConditionBucket(event, 101)) {
			if (extra_budget.TryAdmit()) (void)extra_ring.TryPush(event);
		} else if (base_budget.TryAdmit()) {
			(void)base_ring.TryPush(event);
		}
	};
	for (unsigned i = 0; i != 5; ++i) push(upstream_wake);
	Check(extra_budget.Attempted() == 5 && extra_budget.Skipped() == 1 && !extra_budget.Open() &&
	      extra_ring.Captured() == 2 && extra_ring.Dropped() == 2,
	      "extra ring contention/full drops and budget skips remain separate");
	Check(base_budget.Attempted() == 0 && base_ring.Captured() == 0 && base_ring.Dropped() == 0,
	      "extra saturation consumes no base slots or attempts");
	push(primary_wake); push(primary_wait);
	Check(base_budget.Attempted() == 2 && base_budget.Skipped() == 0 &&
	      base_ring.Captured() == 2 && base_ring.Dropped() == 0,
	      "base events remain recordable after extra bucket exhaustion");
	unsigned base_wakes = 0, base_waits = 0, extra_wakes = 0;
	base_ring.Drain([&](const Trace::Event& event) {
		base_wakes += event.kind == Trace::Kind::CondWake && event.arg0 == 101;
		base_waits += event.kind == Trace::Kind::CondWait && event.tid == 101;
	});
	extra_ring.Drain([&](const Trace::Event& event) {
		extra_wakes += event.kind == Trace::Kind::CondWake && event.arg0 == 202;
	});
	Check(base_wakes == 1 && base_waits == 1 && extra_wakes == 2,
	      "separate bucket drains preserve event kinds and waiter identities");
	Check(Trace::Detail::UseExtraConditionBucket(primary_wait, 404) &&
	      Trace::Detail::UseExtraConditionBucket(primary_wake, 404) &&
	      !Trace::Detail::UseExtraConditionBucket(upstream_wait, 202) &&
	      !Trace::Detail::UseExtraConditionBucket(upstream_wake, 202),
	      "record-time producer handoff changes priority without relabeling payloads");
	Trace::Detail::RecordBudget<1> exhausted_base, available_extra;
	Check(exhausted_base.TryAdmit() && !exhausted_base.TryAdmit() && available_extra.TryAdmit(),
	      "base exhaustion likewise cannot consume the separate extra budget");
	Trace::Detail::g_chain_enabled.store(false);
}

void TestHistoricalEligibility() {
	Check(!Trace::Enabled() && !Trace::ChainEnabled() && !Trace::ConditionThreadEligible(101),
	      "source and chain coverage default off");
	Trace::Detail::g_enabled.store(true);
	Trace::Detail::ObserveThreadEvent(Flip(101));
	Trace::Detail::ObserveThreadEvent(Wake(202, 101));
	Check(Trace::ProducerThreadId() == 101 && Trace::NotifierThreadId() == 202 &&
	      Trace::Detail::g_condition_threads.Admitted() == 0 &&
	      Trace::ConditionThreadEligible(101) && !Trace::ConditionThreadEligible(202),
	      "chain-off preserves direct notifier selection without learning coverage");
	Trace::Detail::g_chain_enabled.store(true);
	Trace::Detail::g_enabled.store(false);
	Check(!Trace::ChainEnabled() && !Trace::ExtraConditionAdmissionOpen() &&
	      !Trace::ConditionThreadEligible(101), "source-off disables chain and current-producer eligibility");
	Trace::Detail::g_enabled.store(true);
	Trace::Detail::ObserveThreadEvent(Flip(101));
	Trace::Detail::ObserveThreadEvent(Wake(202, 101));
	for (unsigned invalid = 0; invalid != 9; ++invalid) {
		auto event = Wake(400 + invalid, 202);
		switch (invalid) {
			case 0: event.tid = 0; break;
			case 1: event.request_id = 0; break;
			case 2: event.flags = 0; break;
			case 3: event.flags = 4; break;
			case 4: event.arg0 = 0; break;
			case 5: event.arg0 = uint64_t {1} << 32; break;
			case 6: event.begin_ns = event.work_end_ns = event.end_ns = 0; break;
			case 7: ++event.work_end_ns; break;
			case 8: event.kind = Trace::Kind::CondWait; break;
		}
		Trace::Detail::ObserveThreadEvent(event);
	}
	Trace::Detail::ObserveThreadEvent(Wake(499, 999));
	Check(Trace::Detail::g_condition_threads.Admitted() == 2,
	      "malformed wakes and an unknown waiter cannot expand coverage");
	for (uint32_t kind = 1; kind <= 3; ++kind) {
		auto event = Wake(302 + kind, 202); event.flags = kind;
		Trace::Detail::ObserveThreadEvent(event);
		Check(Trace::Detail::g_condition_threads.Contains(302 + kind),
		      "signal, targeted signal and broadcast each learn an upstream notifier");
	}
	Trace::Detail::ObserveThreadEvent(Wake(101, 303));
	Trace::Detail::ObserveThreadEvent(Wake(303, 303));
	Check(Trace::Detail::g_condition_threads.Admitted() == 5 &&
	      Trace::ConditionThreadEligible(202) && Trace::ConditionThreadEligible(303) &&
	      Trace::ProducerThreadId() == 101 && Trace::NotifierThreadId() == 202,
	      "upstream wakes, cycles and self-wakes add bounded coverage without retargeting primary sampler");
	Trace::Detail::ObserveThreadEvent(Flip(404));
	Trace::Detail::ObserveThreadEvent(Wake(505, 202));
	Check(Trace::ConditionThreadEligible(101) && Trace::ConditionThreadEligible(505) &&
	      Trace::ProducerThreadId() == 404 && Trace::NotifierThreadId() == 0,
	      "producer handoff preserves historical coverage but clears primary notifier selection");
	Trace::Detail::g_chain_enabled.store(false);
	alignas(8) uint64_t owner = 0;
	WaitTrace::State off;
	Check(!WaitTrace::Begin(off, &owner, 202, 1) && Empty(off) &&
	      Trace::Detail::g_condition_threads.Contains(202),
	      "chain-off suppresses a historically known nonproducer without erasing membership");
	Trace::Detail::g_chain_enabled.store(true);
}

void TestEligibleWaitLifetimeAndGuards() {
	alignas(8) uint64_t owner_a = 0, owner_b = 0;
	WaitTrace::State active;
	Check(WaitTrace::Begin(active, &owner_a, 202, 1), "registered nonproducer begins a real helper occurrence");
	const auto id = active.id;
	Trace::Detail::ObserveThreadEvent(Flip(606));
	WaitTrace::PublishWake(WaitTrace::MakeWake(active, &owner_a, 8, 1));
	const auto ready = WaitTrace::CaptureReady(active, &owner_a);
	Check(Empty(active), "eligible nonproducer state clears before copied completion");
	Check(WaitTrace::Begin(active, &owner_b, 202, 1) && active.id > id,
	      "historical coverage survives producer switch and creates a new occurrence");
	const auto next_id = active.id;
	WaitTrace::Complete(ready, 9, 0);
	Check(active.id == next_id && sink_count == 2 && sink[0].request_id == id &&
	      sink[0].arg0 == 202 && sink[1].request_id == id && sink[1].tid == 202 &&
	      sink[1].begin_ns <= sink[1].work_end_ns && sink[1].work_end_ns <= sink[1].end_ns &&
	      Trace::ProducerThreadId() == 606 && Trace::NotifierThreadId() == 0,
	      "in-flight copy and upstream wake preserve identities without selecting a primary notifier");
	WaitTrace::Complete(WaitTrace::CaptureReady(active, &owner_b), 0, 0);

	for (bool same_owner: {false, true}) {
		WaitTrace::State nested;
		Check(WaitTrace::Begin(nested, &owner_a, 202, 1), "eligible outer begins before nesting");
		const auto outer_id = nested.id;
		std::thread helper([&] {
			Check(!WaitTrace::Begin(nested, same_owner ? &owner_a : &owner_b, 202, 1),
			      "same/different condition nested entry cannot replace historical wait");
		});
		helper.join();
		Check(nested.id == outer_id && WaitTrace::MakeWake(nested, &owner_a, 9, 1).request_id == 0 &&
		      WaitTrace::MakeWake(nested, &owner_b, 10, 2).request_id == 0,
		      "nested historical wait keeps outer identity and suppresses all wake attribution");
		const auto event = WaitTrace::CaptureReady(nested, &owner_a);
		Check(event.request_id == outer_id && event.flags == (1u | 512u) && Empty(nested),
		      "nested uncertainty survives capture for a registered nonproducer");
		WaitTrace::Complete(event, 1, 0);
	}
	WaitTrace::State borrowed;
	Check(WaitTrace::Begin(borrowed, &owner_a, 202, 1), "historical wait begins before identity borrowing");
	std::thread borrower([&] { WaitTrace::MarkBorrowed(borrowed); });
	borrower.join();
	Check(WaitTrace::MakeWake(borrowed, &owner_a, 11, 1).request_id == 0,
	      "historical membership cannot bypass borrowed-identity wake exclusion");
	const auto event = WaitTrace::CaptureReady(borrowed, &owner_a);
	Check(event.flags == (1u | 512u) && Empty(borrowed) &&
	      !WaitTrace::Begin(borrowed, &owner_b, 202, 1),
	      "borrowed historical identity remains excluded after completion");
	WaitTrace::Complete(event, 0, 0);
}

void TestExhaustionAndInFlightCompletion() {
	// Do this last: production state intentionally has no reset/removal API.
	for (uint32_t tid = 1'000; Trace::Detail::g_condition_threads.Admitted() < 32; ++tid) {
		Check(Trace::Detail::g_condition_threads.Admit(tid), "fill remaining historical coverage slots");
	}
	Trace::Detail::ObserveThreadEvent(Flip(90'000));
	Check(!Trace::Detail::g_condition_threads.Contains(90'000) &&
	      Trace::ConditionThreadEligible(90'000), "current producer remains eligible when registry is full");
	alignas(8) uint64_t owner = 0;
	WaitTrace::State in_flight;
	Check(WaitTrace::Begin(in_flight, &owner, 202, 1), "historical wait begins before extra budget closes");
	const auto id = in_flight.id;
	for (uint64_t i = 0; i != Trace::Detail::ExtraConditionRecordBudget; ++i) {
		Check(Trace::Detail::g_extra_condition_budget.TryAdmit(), "each available extra budget attempt is admitted");
	}
	const auto blocked = Trace::Detail::g_chain_budget_blocked.load();
	WaitTrace::State rejected, producer;
	Check(!Trace::ExtraConditionAdmissionOpen() && !WaitTrace::Begin(rejected, &owner, 202, 1) &&
	      Empty(rejected) && Trace::Detail::g_chain_budget_blocked.load() == blocked + 1,
	      "exhaustion blocks a new historical wait and counts that admission rejection");
	Check(WaitTrace::Begin(producer, &owner, 90'000, 1),
	      "current producer helper remains active despite full registry and extra budget");
	const auto wake = WaitTrace::MakeWake(in_flight, &owner, 12, 1);
	const auto ready = WaitTrace::CaptureReady(in_flight, &owner);
	Check(wake.request_id == id && ready.request_id == id && ready.flags == 1 && Empty(in_flight),
	      "budget closure does not corrupt or prevent cleanup of an in-flight occurrence");
	Check(!Trace::Detail::g_extra_condition_budget.TryAdmit() &&
	      !Trace::Detail::g_extra_condition_budget.TryAdmit() &&
	      Trace::Detail::g_extra_condition_budget.Skipped() == 2,
	      "late in-flight wake/wait attempts become explicit budget skips");
	// The sink deliberately omits bucket admission: validate copied completion and
	// cleanup separately from the actual admission helper checked immediately above.
	WaitTrace::Complete(ready, 3, 0);
	WaitTrace::Complete(WaitTrace::CaptureReady(producer, &owner), 0, 0);
	Check(Trace::Enabled() && Trace::ChainEnabled() && Trace::ConditionThreadEligible(90'000),
	      "budget exhaustion leaves source tracing and current-producer coverage enabled");
}
} // namespace

namespace Common::SourceTrace {
void Record(Event event) noexcept {
	if (!Enabled()) return;
	if (event.tid == 0) event.tid = sink_tid;
	Detail::ObserveThreadEvent(event);
	std::lock_guard lock(sink_mutex);
	Check(sink_count != sink.size(), "bounded fixture sink overflowed");
	sink[sink_count++] = event;
}
} // namespace Common::SourceTrace

int main() {
	std::atomic<bool> done {false};
	std::thread watchdog([&] {
		const auto deadline = std::chrono::steady_clock::now() + 15s;
		while (!done.load(std::memory_order_acquire)) {
			if (std::chrono::steady_clock::now() >= deadline) Fail("fixture watchdog expired");
			std::this_thread::sleep_for(10ms);
		}
	});
	TestRegistryCapacityAndConcurrency();
	TestBucketIsolation();
	TestHistoricalEligibility();
	TestEligibleWaitLifetimeAndGuards();
	TestExhaustionAndInFlightCompletion();
	done.store(true, std::memory_order_release);
	watchdog.join();
	std::printf("PASS: source chain helpers; members=%llu capacity_rejected=%llu extra_attempted=%llu "
	            "extra_skipped=%llu blocked_begins=%llu captured_fixture_events=%zu\n",
	            static_cast<unsigned long long>(Trace::Detail::g_condition_threads.Admitted()),
	            static_cast<unsigned long long>(Trace::Detail::g_condition_threads.Rejected()),
	            static_cast<unsigned long long>(Trace::Detail::g_extra_condition_budget.Attempted()),
	            static_cast<unsigned long long>(Trace::Detail::g_extra_condition_budget.Skipped()),
	            static_cast<unsigned long long>(Trace::Detail::g_chain_budget_blocked.load()), sink_count);
	return 0;
}
