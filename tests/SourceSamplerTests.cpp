#include "common/perfStats.h"
#include "common/sourceTrace.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string_view>
#include <thread>

// CPU-only integration fixture for the real sampler/reporter. The caller supplies
// all environment settings; this executable never starts a renderer or game.
//
// source_sampler_tests --producer-fixture [--switch-producer]
//   KYTY_PERF=1 KYTY_GT7_SOURCE_TRACE=1 KYTY_SAMPLE_SOURCE=1
//   KYTY_SAMPLE=<a deliberately unmatched name> KYTY_SAMPLE_HZ=50 KYTY_SAMPLE_REPORT=1
//
// Also run with source sampling disabled (and, separately, source tracing
// disabled). An external log check must verify sample activation/absence, actual
// target TIDs and non-exhausted budgets: worker progress alone does not prove that
// any sample was captured. No-argument mode checks the process startup defaults.
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto WorkDuration = 3200ms;
constexpr auto FixtureDeadline = 15s;

struct WorkerState {
	std::atomic<uint32_t> tid {0}, observed_producer {0};
	std::atomic<uint64_t> busy_batches {0}, sleep_returns {0}, checksum {0};
	std::atomic<bool> started {false}, work_complete {false}, finished {false};
};

#if defined(_MSC_VER)
#define FIXTURE_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define FIXTURE_NOINLINE __attribute__((noinline))
#else
#define FIXTURE_NOINLINE
#endif

FIXTURE_NOINLINE void SamplerFixtureBusy(WorkerState& state, uint64_t& value) {
	const auto until = Clock::now() + 15ms;
	do {
		for (unsigned i = 0; i < 4096; ++i) {
			value ^= value << 13;
			value ^= value >> 7;
			value ^= value << 17;
		}
		state.busy_batches.fetch_add(1, std::memory_order_relaxed);
	} while (Clock::now() < until);
	// Keep the computation observable in optimized/NDEBUG builds.
	state.checksum.store(value, std::memory_order_relaxed);
}

FIXTURE_NOINLINE void SamplerFixtureSleep(WorkerState& state) {
	std::this_thread::sleep_for(15ms);
	state.sleep_returns.fetch_add(1, std::memory_order_relaxed);
}

void Producer(WorkerState& state, uint64_t generation, WorkerState* successor) {
	// Deliberately do not call PerfStats::NameThread: the source observation must
	// discover a thread absent from the named-thread registry.
	state.tid.store(Common::PerfStats::CurrentOsThreadId(), std::memory_order_relaxed);
	Common::SourceTrace::Event event {};
	event.kind = Common::SourceTrace::Kind::FlipSubmit;
	event.begin_ns = event.work_end_ns = event.end_ns = Common::SourceTrace::NowNs();
	event.request_id = generation;
	event.arg0 = 1;
	Common::SourceTrace::Record(event);
	state.observed_producer.store(Common::SourceTrace::ProducerThreadId(), std::memory_order_relaxed);
	state.started.store(true, std::memory_order_release);

	uint64_t value = 0x0123456789abcdefull ^ generation;
	const auto until = Clock::now() + WorkDuration;
	do {
		SamplerFixtureBusy(state, value);
		SamplerFixtureSleep(state);
	} while (Clock::now() < until);
	state.work_complete.store(true, std::memory_order_release);

	// The first producer remains alive until the second has published its TID.
	// Their identities are therefore distinct without relying on OS TID reuse.
	if (successor != nullptr) {
		while (!successor->started.load(std::memory_order_acquire)) {
			std::this_thread::sleep_for(1ms);
		}
	}
	state.finished.store(true, std::memory_order_release);
}

[[noreturn]] void Timeout(const char* stage) {
	std::fprintf(stderr, "FAIL: Source sampler fixture timeout at %s (worker may not have resumed)\n", stage);
	std::fflush(nullptr);
	// A failed suspend/resume regression must fail rather than hang while joining
	// an unresponsive worker. The fixture owns this process and its threads.
	std::_Exit(2);
}

void Await(const std::atomic<bool>& flag, Clock::time_point deadline, const char* stage) {
	while (!flag.load(std::memory_order_acquire)) {
		if (Clock::now() >= deadline) Timeout(stage);
		std::this_thread::sleep_for(5ms);
	}
}

bool IsNamed(uint32_t tid) {
	for (const auto& entry: Common::PerfStats::NamedThreads()) {
		if (entry.first == tid) return true;
	}
	return false;
}
} // namespace

int main(int argc, char** argv) {
	int failures = 0;
	const auto check = [&](bool ok, const char* message) {
		if (!ok) {
			std::fprintf(stderr, "FAIL: %s\n", message);
			++failures;
		}
	};
	check(!Common::SourceTrace::Enabled(), "source tracing defaults off before startup");
	check(Common::SourceTrace::ProducerThreadId() == 0, "no producer is guessed before a source event");
	if (argc == 1) {
		std::printf("Source sampler defaults: status=%s failures=%d\n", failures ? "FAIL" : "PASS", failures);
		return failures ? 1 : 0;
	}
	const bool switch_producer = argc == 3 && std::string_view(argv[2]) == "--switch-producer";
	if (std::string_view(argv[1]) != "--producer-fixture" || (argc != 2 && !switch_producer)) {
		std::fprintf(stderr, "Usage: source_sampler_tests [--producer-fixture [--switch-producer]]\n");
		return 2;
	}

	Common::PerfStats::Start();
	check(Common::PerfStats::Active(), "producer fixture requires an enabled PerfStats reporter");
	const bool trace_enabled = Common::SourceTrace::Enabled();
	const auto deadline = Clock::now() + FixtureDeadline;
	// Also cover a regression that suspends a worker after it published finished
	// but before std::thread::join returns. Normal fixture exit terminates this
	// process-lifetime watchdog before its deadline.
	std::thread([deadline] {
		std::this_thread::sleep_until(deadline);
		Timeout("overall fixture deadline");
	}).detach();
	WorkerState first, second;
	std::thread producer(Producer, std::ref(first), 1, switch_producer ? &second : nullptr);
	Await(first.started, deadline, "first producer startup");
	const auto first_tid = first.tid.load(std::memory_order_relaxed);
	check(first_tid != 0, "first producer has a real OS thread ID");
	check(!IsNamed(first_tid), "first producer is absent from the named-thread registry");
	check(first.observed_producer.load(std::memory_order_relaxed) == (trace_enabled ? first_tid : 0),
	      "FlipSubmit records the actual producer only while source tracing is enabled");
	std::printf("Source sampler fixture worker: generation=1 tid=%u trace_enabled=%u named=0\n",
	            first_tid, unsigned(trace_enabled));
	std::fflush(stdout);

	// Observe progress after the sampler has had time to start and suspend/capture.
	std::this_thread::sleep_for(600ms);
	const auto first_progress = first.busy_batches.load(std::memory_order_relaxed);
	check(first_progress != 0 && first.sleep_returns.load(std::memory_order_relaxed) != 0,
	      "first worker makes both CPU and sleeping progress during sampling");
	Await(first.work_complete, deadline, "first producer work");
	check(first.busy_batches.load(std::memory_order_relaxed) > first_progress,
	      "first worker continues CPU work after initial captures");

	if (switch_producer) {
		std::thread replacement(Producer, std::ref(second), 2, nullptr);
		Await(second.started, deadline, "replacement producer startup");
		const auto second_tid = second.tid.load(std::memory_order_relaxed);
		check(second_tid != 0 && second_tid != first_tid, "producer handoff uses distinct live OS threads");
		check(!IsNamed(second_tid), "replacement producer is absent from the named-thread registry");
		check(second.observed_producer.load(std::memory_order_relaxed) == (trace_enabled ? second_tid : 0),
		      "latest FlipSubmit changes the observed producer only while tracing is enabled");
		std::printf("Source sampler fixture worker: generation=2 tid=%u trace_enabled=%u named=0\n",
		            second_tid, unsigned(trace_enabled));
		std::fflush(stdout);
		Await(first.finished, deadline, "first producer handoff/exit");
		producer.join();
		std::this_thread::sleep_for(600ms);
		const auto second_progress = second.busy_batches.load(std::memory_order_relaxed);
		check(second_progress != 0 && second.sleep_returns.load(std::memory_order_relaxed) != 0,
		      "replacement worker progresses after the old producer exits");
		Await(second.finished, deadline, "replacement producer work/exit");
		check(second.busy_batches.load(std::memory_order_relaxed) > second_progress,
		      "replacement worker keeps progressing during dynamic target sampling");
		replacement.join();
	} else {
		Await(first.finished, deadline, "first producer exit");
		producer.join();
	}

	// Let target refresh encounter the exited producer without emitting another
	// source event. With the documented one-second report setting, this leaves a
	// full report interval for buffered samples and the final source event drain.
	std::this_thread::sleep_for(1200ms);
	check(Common::SourceTrace::ProducerThreadId() ==
	          (trace_enabled ? (switch_producer ? second.tid.load() : first_tid) : 0),
	      "producer observation remains the last source TID, or zero when disabled");
	std::printf("Source sampler fixture: status=%s failures=%d workers=%u trace_enabled=%u "
	            "first_tid=%u second_tid=%u first_busy=%llu first_sleep_returns=%llu "
	            "second_busy=%llu second_sleep_returns=%llu\n",
	            failures ? "FAIL" : "PASS", failures, switch_producer ? 2u : 1u,
	            unsigned(trace_enabled), first_tid, second.tid.load(),
	            static_cast<unsigned long long>(first.busy_batches.load()),
	            static_cast<unsigned long long>(first.sleep_returns.load()),
	            static_cast<unsigned long long>(second.busy_batches.load()),
	            static_cast<unsigned long long>(second.sleep_returns.load()));
	std::fflush(nullptr);
	// PerfStats intentionally has process-lifetime detached threads and no Stop
	// API. Avoid racing their logging against C++ static destruction at test exit.
	std::_Exit(failures ? 1 : 0);
}
