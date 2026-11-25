#include "graphics/host_gpu/renderer/cache/readbackWait.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#include <thread>

namespace R = Libs::Graphics::ReadbackWait;
namespace {
using namespace std::chrono_literals;

void Check(bool ok, const char* what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: readback wait: %s\n", what);
		std::fflush(nullptr);
		std::_Exit(1);
	}
}

// Execute the production dispatch against independently controlled GPU progress
// and CPU publication. No Vulkan device or game is needed for this regression.
struct CompletionState {
	std::mutex mutex;
	std::condition_variable changed;
	uint64_t gpu_done = 0, publication_done = 0;
	unsigned gpu_waits = 0, publication_waits = 0, revalidations = 0;
	bool entered = false, returned = false;
	R::Dependency selected = R::Dependency::None;

	void Wait(R::Dependency dependency, uint64_t tick) {
		std::unique_lock lock(mutex);
		selected = dependency;
		(dependency == R::Dependency::GpuWriter ? gpu_waits : publication_waits)++;
		entered = true;
		changed.notify_all();
		changed.wait(lock, [&] {
			return (dependency == R::Dependency::GpuWriter ? gpu_done : publication_done) >= tick;
		});
	}
};

void TestBlockedDependency(R::Outcome outcome, R::Dependency expected) {
	constexpr uint64_t tick = 7;
	CompletionState state;
	// Writer test: all priority publications are already complete (empty queue).
	// Publication test: GPU work is already complete, but bytes are not published.
	state.gpu_done = expected == R::Dependency::Publication ? tick : 0;
	state.publication_done = expected == R::Dependency::GpuWriter ? tick : 0;
	std::thread reader([&] {
		R::WaitForTick(R::ForStep(outcome, tick, false), tick,
		    [&](uint64_t value) { state.Wait(R::Dependency::GpuWriter, value); },
		    [&](uint64_t value) { state.Wait(R::Dependency::Publication, value); });
		std::lock_guard lock(state.mutex);
		++state.revalidations; // ReadMemory may now ask ReadMemoryStep again.
		state.returned = true;
		state.changed.notify_all();
	});
	{
		std::unique_lock lock(state.mutex);
		Check(state.changed.wait_for(lock, 2s, [&] { return state.entered || state.returned; }),
		      "reader reached its dependency wait");
		Check(state.selected == expected, "actual dispatch selected the required completion source");
		Check(!state.returned && state.revalidations == 0,
		      "completed unrelated dependency must not cause a retry");
		// A wake without the required completion cannot let the reader poll again.
		state.changed.notify_all();
		Check(!state.changed.wait_for(lock, 20ms, [&] { return state.returned; }),
		      "spurious notification does not bypass the required completion");
		(expected == R::Dependency::GpuWriter ? state.gpu_done : state.publication_done) = tick;
		state.changed.notify_all();
		Check(state.changed.wait_for(lock, 2s, [&] { return state.returned; }),
		      "required completion releases the reader for revalidation");
		Check(state.revalidations == 1 && state.gpu_waits + state.publication_waits == 1,
		      "one wait followed by one revalidation, without repeated queue round trips");
	}
	reader.join();
}

void TestNoWaitAndFastTickets() {
	unsigned called = 0;
	const auto unexpected_wait = [&](uint64_t) { ++called; };
	for (unsigned i = 0; i < static_cast<unsigned>(R::Outcome::Count); ++i) {
		const auto outcome = static_cast<R::Outcome>(i);
		const auto no_wait = R::ForStep(outcome, 0, false);
		Check(no_wait == R::Dependency::None, "zero tick without a ticket needs no wait");
		R::WaitForTick(no_wait, 0, unexpected_wait, unexpected_wait);
		for (uint64_t tick: {uint64_t {0}, uint64_t {7}}) {
			const auto ticket = R::ForStep(outcome, tick, true);
			Check(ticket == R::Dependency::FastTicket,
			      "valid fast ticket retains precedence over outcome and tick");
			R::WaitForTick(ticket, tick, unexpected_wait, unexpected_wait);
		}
	}
	Check(called == 0, "no-wait and fast-ticket paths never dispatch a timeline/publication wait");
}

void TestExistingOutcomeClassification() {
	static_assert(static_cast<unsigned>(R::Outcome::Bitmap) == 2);
	static_assert(static_cast<unsigned>(R::Outcome::WaitWriter) == 7);
	static_assert(static_cast<unsigned>(R::Outcome::Download) == 8);
	static_assert(static_cast<unsigned>(R::Outcome::Count) == 11);
	for (unsigned i = 0; i < static_cast<unsigned>(R::Outcome::Count); ++i) {
		const auto outcome = static_cast<R::Outcome>(i);
		if (outcome != R::Outcome::Bitmap && outcome != R::Outcome::WaitWriter) {
			Check(R::ForStep(outcome, 7, false) == R::Dependency::Publication,
			      "other nonzero-tick outcomes retain publication waiting");
		}
	}
}
} // namespace

int main() {
	TestBlockedDependency(R::Outcome::Bitmap, R::Dependency::GpuWriter);
	TestBlockedDependency(R::Outcome::WaitWriter, R::Dependency::GpuWriter);
	TestBlockedDependency(R::Outcome::Download, R::Dependency::Publication);
	TestBlockedDependency(R::Outcome::LiftWaited, R::Dependency::Publication);
	TestNoWaitAndFastTickets();
	TestExistingOutcomeClassification();
	std::puts("PASS: readback wait (6 groups; production classification/dispatch, simulated completions)");
	return 0;
}
