#include "common/sourceTrace.h"
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

using namespace Common::SourceTrace;
int main() {
	int failures = 0;
	const auto check = [&](bool ok, const char* message) {
		if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
	};
	check(!Enabled(), "tracing defaults off");
	check(ProducerThreadId() == 0, "no producer thread is guessed before an observed flip");
	check(NotifierThreadId() == 0, "no notifier is guessed before an observed condition wake");
	// Exercise the same publication helper used by enabled Record; enabled/off
	// gating and caller-ID fill are covered by the real-reporter notifier fixture.
	Event producer_a {}, producer_b {}, wake_a {}, wake_b {};
	producer_a.kind = producer_b.kind = Kind::FlipSubmit;
	producer_a.tid = 11; producer_b.tid = 12;
	wake_a.kind = wake_b.kind = Kind::CondWake;
	wake_a.tid = 21; wake_b.tid = 22;
	wake_a.arg0 = 11; wake_b.arg0 = 12;
	wake_a.request_id = wake_b.request_id = 7;
	wake_a.flags = wake_b.flags = 1;
	wake_a.begin_ns = wake_a.work_end_ns = wake_a.end_ns = 100;
	wake_b.begin_ns = wake_b.work_end_ns = wake_b.end_ns = 200;
	Detail::ObserveThreadEvent(wake_a);
	check(NotifierThreadId() == 0, "a wake without an observed producer does not select a target");
	Detail::ObserveThreadEvent(producer_a);
	check(ProducerThreadId() == 11 && NotifierThreadId() == 0, "a producer alone has no notifier");
	for (unsigned invalid = 0; invalid != 7; ++invalid) {
		Event event = wake_a;
		switch (invalid) {
			case 0: event.tid = 0; break;
			case 1: event.request_id = 0; break;
			case 2: event.flags = 4; break;
			case 3: event.arg0 = uint64_t {1} << 32u; break;
			case 4: event.begin_ns = event.work_end_ns = event.end_ns = 0; break;
			case 5: ++event.end_ns; break;
			case 6: event.kind = Kind::CondWait; break;
		}
		Detail::ObserveThreadEvent(event);
		check(NotifierThreadId() == 0, "malformed/unrelated records cannot select a notifier");
	}
	for (uint32_t wake_kind = 1; wake_kind <= 3; ++wake_kind) {
		wake_a.flags = wake_kind;
		Detail::ObserveThreadEvent(wake_a);
		check(NotifierThreadId() == 21, "signal/targeted/broadcast select the directly observed notifier");
	}
	Event upstream = wake_a; upstream.tid = 31; upstream.arg0 = 21;
	Detail::ObserveThreadEvent(upstream);
	Detail::ObserveThreadEvent(producer_a);
	check(NotifierThreadId() == 21, "upstream wake and repeated same-producer flip preserve selection");
	Detail::ObserveThreadEvent(producer_b);
	Detail::ObserveThreadEvent(wake_a);
	check(ProducerThreadId() == 12 && NotifierThreadId() == 0,
	      "producer handoff clears selection and late old-producer wake cannot restore it");
	Detail::ObserveThreadEvent(wake_b);
	check(NotifierThreadId() == 22, "new producer accepts its own observed notifier");
	Event self = wake_b; self.tid = 12;
	Detail::ObserveThreadEvent(self);
	check(NotifierThreadId() == 12, "identity observation preserves self-notification truthfully");

	// Concurrent producer changes and late wakes must never expose a notifier
	// paired with a different producer. This is a real atomic-publication race.
	Detail::g_observed_threads.store(0);
	std::atomic<unsigned> publication_done {0};
	std::atomic<bool> publication_go {false};
	std::thread flips([&] {
		while (!publication_go.load(std::memory_order_acquire)) std::this_thread::yield();
		for (unsigned i = 0; i != 40'000; ++i) {
			Detail::ObserveThreadEvent((i & 1u) ? producer_a : producer_b);
		}
		publication_done.fetch_add(1, std::memory_order_release);
	});
	std::thread wakes([&] {
		while (!publication_go.load(std::memory_order_acquire)) std::this_thread::yield();
		for (unsigned i = 0; i != 80'000; ++i) {
			Detail::ObserveThreadEvent((i & 1u) ? wake_a : wake_b);
		}
		publication_done.fetch_add(1, std::memory_order_release);
	});
	uint64_t mixed_pairs = 0;
	publication_go.store(true, std::memory_order_release);
	while (publication_done.load(std::memory_order_acquire) != 2) {
		const auto pair = ObservedThreadIds();
		if (!((pair.producer_tid == 0 && pair.notifier_tid == 0) ||
		      (pair.producer_tid == 11 && (pair.notifier_tid == 0 || pair.notifier_tid == 21)) ||
		      (pair.producer_tid == 12 && (pair.notifier_tid == 0 || pair.notifier_tid == 22)))) ++mixed_pairs;
	}
	flips.join(); wakes.join();
	check(mixed_pairs == 0, "concurrent observations never publish mismatched producer/notifier pairs");
	Detail::g_observed_threads.store(0);
	Detail::Ring<2> namespaces;
	producer_a.request_id = wake_a.request_id;
	check(namespaces.TryPush(producer_a) && namespaces.TryPush(wake_a),
	      "flip and condition ID namespaces can carry the same numeric ID");
	unsigned flip_seen = 0, wake_seen = 0;
	namespaces.Drain([&](const Event& event) {
		if (event.kind == Kind::FlipSubmit && event.tid == 11 && event.request_id == 7) ++flip_seen;
		if (event.kind == Kind::CondWake && event.tid == 21 && event.arg0 == 11 && event.request_id == 7) ++wake_seen;
	});
	check(flip_seen == 1 && wake_seen == 1, "ring preserves kind and waiter identity without conflating IDs");
	Detail::Ring<2> small;
	Event first {}; first.submission_id = 11;
	Event second {}; second.submission_id = 22;
	check(small.TryPush(first) && small.TryPush(second), "free slots accept records");
	check(!small.TryPush(first), "full ring drops without overwriting");
	uint64_t sum = 0;
	check(small.Drain([&](const Event& e) { sum += e.submission_id; }) == 2 && sum == 33,
	      "full ring retains both original payloads");
	check(small.TryPush(first), "drained slots are reusable");
	check(small.Drain([](const Event&) {}) == 1 && small.Dropped() == 1,
	      "wrap and dropped accounting");

	constexpr uint64_t workers = 8, iterations = 40'000;
	Detail::Ring<128> ring;
	std::atomic<bool> go {false};
	std::atomic<uint32_t> done {0};
	std::vector<std::thread> producers;
	for (uint64_t p = 0; p < workers; ++p) producers.emplace_back([&, p] {
		while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
		for (uint64_t i = 0; i < iterations; ++i) {
			Event event {};
			event.submission_id = p * iterations + i;
			event.request_id = event.submission_id ^ 0x13579abcdefull;
			event.begin_ns = event.submission_id * 19;
			event.end_ns = event.begin_ns + 17;
			(void)ring.TryPush(event);
		}
		done.fetch_add(1, std::memory_order_release);
	});
	std::vector<bool> seen(workers * iterations, false);
	uint64_t consumed = 0, torn = 0, duplicate = 0;
	const auto consume = [&](const Event& event) {
		const auto id = event.submission_id;
		if (id >= seen.size() || event.request_id != (id ^ 0x13579abcdefull) ||
		    event.begin_ns != id * 19 || event.end_ns != event.begin_ns + 17) { ++torn; return; }
		if (seen[id]) ++duplicate;
		seen[id] = true;
		++consumed;
	};
	go.store(true, std::memory_order_release);
	while (done.load(std::memory_order_acquire) != workers) {
		ring.Drain(consume);
		std::this_thread::yield();
	}
	for (auto& producer: producers) producer.join();
	ring.Drain(consume);
	check(torn == 0 && duplicate == 0, "concurrent payloads are neither torn nor duplicated");
	check(consumed == ring.Captured(), "every published payload is drained after producers finish");
	check(ring.Captured() + ring.Dropped() == workers * iterations, "every attempt is accounted for");
	std::printf("Source trace: captured=%llu dropped=%llu failures=%d\n",
	    static_cast<unsigned long long>(ring.Captured()),
	    static_cast<unsigned long long>(ring.Dropped()), failures);
	return failures == 0 ? 0 : 1;
}
