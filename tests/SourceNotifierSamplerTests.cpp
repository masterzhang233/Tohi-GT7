#include "common/perfStats.h"
#include "common/sourceTrace.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <string_view>
#include <thread>

// CPU-only fixture: real Record, reporter and sampler, real live OS threads,
// synthetic source events. It does not exercise guest pthread integration.
// The separate log test verifies actual captured targets and timing; worker
// progress by itself cannot prove that the sampler selected the right thread.
namespace {
namespace Trace = Common::SourceTrace;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

[[noreturn]] void Fail(const char* message) {
	std::fprintf(stderr, "FAIL: notifier fixture: %s\n", message);
	std::fflush(nullptr);
	std::_Exit(1);
}
void Check(bool ok, const char* message) { if (!ok) Fail(message); }
template <typename Predicate> void Await(Predicate&& ready, const char* message) {
	const auto deadline = Clock::now() + 3s;
	while (!ready()) {
		if (Clock::now() >= deadline) Fail(message);
		std::this_thread::sleep_for(1ms);
	}
}

void Publish(Trace::Kind kind, uint32_t waiter, uint64_t id) {
	Trace::Event event {};
	event.kind = kind;
	event.flags = kind == Trace::Kind::CondWake ? 1 : 0;
	event.begin_ns = event.work_end_ns = event.end_ns = Trace::NowNs();
	event.request_id = id;
	event.arg0 = kind == Trace::Kind::CondWake ? waiter : 1;
	event.arg1 = id;
	Trace::Record(event); // tid=0 intentionally exercises actual caller-ID fill.
}

struct Worker {
	std::atomic<uint32_t> tid {0}, waiter {0}, kind {0};
	std::atomic<uint64_t> command {0}, acknowledged {0}, busy {0}, sleeps {0}, checksum {0};
	std::atomic<bool> stop {false}, finished {false};
};

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline))
#endif
void NotifierFixtureBusy(Worker& worker, uint64_t& value) {
	const auto end = Clock::now() + 2ms;
	do {
		for (unsigned i = 0; i != 1024; ++i) {
			value ^= value << 13;
			value ^= value >> 7;
			value ^= value << 17;
		}
		worker.busy.fetch_add(1, std::memory_order_relaxed);
	} while (Clock::now() < end);
	worker.checksum.store(value, std::memory_order_relaxed);
}

void Work(Worker& worker, bool named_decoy) {
	if (named_decoy) Common::PerfStats::NameThread("NotifierFixtureDecoy");
	worker.tid.store(Common::PerfStats::CurrentOsThreadId(), std::memory_order_release);
	uint64_t value = 0x1023456789abcdefull;
	while (!worker.stop.load(std::memory_order_acquire)) {
		const uint64_t command = worker.command.load(std::memory_order_acquire);
		if (command != worker.acknowledged.load(std::memory_order_relaxed)) {
			Publish(worker.kind.load(std::memory_order_relaxed) == 1 ?
			            Trace::Kind::CondWake : Trace::Kind::FlipSubmit,
			        worker.waiter.load(std::memory_order_relaxed), command);
			worker.acknowledged.store(command, std::memory_order_release);
		}
		NotifierFixtureBusy(worker, value);
		std::this_thread::sleep_for(2ms);
		worker.sleeps.fetch_add(1, std::memory_order_relaxed);
	}
	worker.finished.store(true, std::memory_order_release);
}

void Command(Worker& worker, bool wake, uint32_t waiter) {
	worker.waiter.store(waiter, std::memory_order_relaxed);
	worker.kind.store(wake ? 1 : 2, std::memory_order_relaxed);
	const auto command = worker.command.load(std::memory_order_relaxed) + 1;
	worker.command.store(command, std::memory_order_release);
	Await([&] { return worker.acknowledged.load(std::memory_order_acquire) == command; },
	      "worker did not execute a source command after sampling");
}

void Phase(const char* name, uint32_t selected_tid, std::chrono::milliseconds duration) {
	const auto begin = Trace::NowNs();
	std::this_thread::sleep_for(duration);
	std::printf("Notifier fixture phase: name=%s begin_ns=%llu end_ns=%llu expected_tid=%u\n",
	            name, static_cast<unsigned long long>(begin),
	            static_cast<unsigned long long>(Trace::NowNs()), selected_tid);
	std::fflush(stdout);
}
} // namespace

int main(int argc, char** argv) {
	const bool unknown_only = argc == 2 && std::string_view(argv[1]) == "--unknown-fixture";
	if (!unknown_only && !(argc == 2 && std::string_view(argv[1]) == "--notifier-fixture")) {
		std::fprintf(stderr, "Usage: source_notifier_sampler_tests --notifier-fixture|--unknown-fixture\n");
		return 2;
	}
	Check(!Trace::Enabled() && Trace::ProducerThreadId() == 0 && Trace::NotifierThreadId() == 0,
	      "source identities default unknown before reporter startup");
	Common::PerfStats::Start();
	Check(Common::PerfStats::Active(), "fixture requires the real PerfStats reporter");
	const bool enabled = Trace::Enabled();
	std::thread([] { std::this_thread::sleep_for(20s); Fail("watchdog expired"); }).detach();

	Worker first, second, decoy;
	std::thread a(Work, std::ref(first), false), b(Work, std::ref(second), false),
	            c(Work, std::ref(decoy), true);
	Await([&] { return first.tid.load() && second.tid.load() && decoy.tid.load(); },
	      "live workers did not start");
	const uint32_t producer_tid = Common::PerfStats::CurrentOsThreadId();
	const uint32_t first_tid = first.tid.load(), second_tid = second.tid.load(), decoy_tid = decoy.tid.load();
	Check(first_tid != second_tid && first_tid != decoy_tid && second_tid != decoy_tid &&
	      producer_tid != first_tid && producer_tid != second_tid && producer_tid != decoy_tid,
	      "all fixture identities belong to distinct simultaneously live threads");
	bool decoy_named = false;
	for (const auto& entry: Common::PerfStats::NamedThreads()) {
		if (entry.first == decoy_tid) decoy_named = true;
		Check(entry.first != first_tid && entry.first != second_tid,
		      "notifier workers must be absent from the named registry");
	}
	Check(decoy_named, "matching named decoy is registered to exercise fallback exclusion");
	std::printf("Notifier fixture identities: producer=%u first=%u second=%u decoy=%u trace_enabled=%u\n",
	            producer_tid, first_tid, second_tid, decoy_tid, unsigned(enabled));
	std::fflush(stdout);
	Publish(Trace::Kind::FlipSubmit, 0, 100);
	Check(Trace::ProducerThreadId() == (enabled ? producer_tid : 0), "actual producer publication");
	Check(Trace::NotifierThreadId() == 0, "producer alone cannot select a notifier");
	Command(decoy, true, first_tid); // A wake farther upstream must not select the decoy.
	Check(Trace::NotifierThreadId() == 0, "unrelated waiter notification does not invent a primary notifier");
	Phase("unknown", 0, 1200ms);

	if (!unknown_only) {
		Publish(Trace::Kind::CondWake, producer_tid, 200);
		Check(Trace::NotifierThreadId() == (enabled ? producer_tid : 0),
		      "self-notification is observed truthfully without selecting a suspend target");
		Phase("self_notifier", 0, 1200ms);
		Command(first, true, producer_tid);
		Check(Trace::NotifierThreadId() == (enabled ? first_tid : 0), "actual first notifier publication");
		Phase("first", enabled ? first_tid : 0, 1600ms);
		const auto first_progress = first.busy.load();
		Command(decoy, true, first_tid);
		Check(Trace::NotifierThreadId() == (enabled ? first_tid : 0),
		      "upstream wake cannot replace the primary notifier");
		Publish(Trace::Kind::FlipSubmit, 0, 101);
		Check(Trace::NotifierThreadId() == (enabled ? first_tid : 0),
		      "same-producer flip preserves its observed notifier");
		Command(second, true, producer_tid);
		Check(Trace::NotifierThreadId() == (enabled ? second_tid : 0), "notifier handoff publication");
		Phase("second", enabled ? second_tid : 0, 1600ms);
		Check(first.busy.load() > first_progress, "retired first target continues CPU work");
		Command(decoy, false, 0); // New actual producer clears the previous relationship.
		Check(Trace::ProducerThreadId() == (enabled ? decoy_tid : 0) && Trace::NotifierThreadId() == 0,
		      "producer change invalidates the old notifier selection");
		Command(first, true, producer_tid);
		Check(Trace::NotifierThreadId() == 0, "stale old-producer wake cannot restore selection");
		Phase("producer_changed", 0, 1200ms);
		Command(second, true, decoy_tid);
		Check(Trace::NotifierThreadId() == (enabled ? second_tid : 0), "new producer gets its own observed notifier");
		Phase("new_producer_notifier", enabled ? second_tid : 0, 1600ms);
		Publish(Trace::Kind::FlipSubmit, 0, 102);
		Check(Trace::NotifierThreadId() == 0, "final producer change retires live notifier selection");
		Phase("retired", 0, 1200ms);
	}

	for (Worker* worker: {&first, &second, &decoy}) {
		Check(worker->busy.load() != 0 && worker->sleeps.load() != 0,
		      "every sampled or ignored worker makes CPU and sleeping progress");
		worker->stop.store(true, std::memory_order_release);
	}
	Await([&] { return first.finished.load() && second.finished.load() && decoy.finished.load(); },
	      "worker could not finish after target retirement");
	a.join(); b.join(); c.join();
	std::printf("Notifier fixture PASS: first_busy=%llu first_sleep=%llu second_busy=%llu second_sleep=%llu "
	            "decoy_busy=%llu decoy_sleep=%llu\n",
	            static_cast<unsigned long long>(first.busy.load()), static_cast<unsigned long long>(first.sleeps.load()),
	            static_cast<unsigned long long>(second.busy.load()), static_cast<unsigned long long>(second.sleeps.load()),
	            static_cast<unsigned long long>(decoy.busy.load()), static_cast<unsigned long long>(decoy.sleeps.load()));
	std::fflush(nullptr);
	std::_Exit(0); // Reporter/sampler have process-lifetime threads, no Stop API.
}
