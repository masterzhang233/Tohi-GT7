#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "kernel/eventFlag.h"
#include "kernel/pthread.h"
#include "kernel/signalSleep.h"
#include "libs/errno.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace Libs::LibKernel;
using namespace Libs::LibKernel::EventFlag;
using namespace std::chrono_literals;

constexpr uint32_t AND       = 0x01;
constexpr uint32_t OR        = 0x02;
constexpr uint32_t CLEAR_ALL = 0x10;
constexpr uint32_t CLEAR_PAT = 0x20;
constexpr uint32_t SINGLE    = 0x10;
constexpr uint32_t MULTI     = 0x20;
constexpr uint32_t PRIORITY  = 0x02;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "KernelWaitsTests: failed: %s\n", message);
		std::abort();
	}
}

template <class Ready>
void WaitUntil(const Ready& ready, const char* message) {
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	while (!ready()) {
		Check(std::chrono::steady_clock::now() < deadline, message);
		std::this_thread::sleep_for(100us);
	}
}

void TestSleepDeadline() {
	uint64_t now = 0;
	int dispatches = 0;
	std::vector<uint64_t> sleeps;
	auto clock = [&] { return now; };
	auto sleep = [&](uint64_t ns) {
		sleeps.push_back(ns);
		now += ns + 2000000; // Each host wait oversleeps by 2 ms.
	};
	auto dispatch = [&] {
		++dispatches;
		now += 1000000; // Signal handlers count against the same deadline.
	};
	Detail::SleepWithSignalPoll(30000000, 10000000, clock, sleep, dispatch);
	Check(sleeps == std::vector<uint64_t>({10000000, 10000000, 4000000}),
	      "oversleep and signal delivery reduce the next sleep slice");
	Check(now == 33000000 && dispatches == 3,
	      "sleep does not accumulate the requested slices after its deadline");

	now = 0;
	dispatches = 0;
	sleeps.clear();
	Detail::SleepWithSignalPoll(0, 10000000, clock, sleep, dispatch);
	Check(sleeps.empty() && dispatches == 1, "zero sleep still dispatches pending signals");

	now = UINT64_MAX - 999;
	sleeps.clear();
	Detail::SleepWithSignalPoll(2000, 1000, clock,
	                           [&](uint64_t ns) { sleeps.push_back(ns); now += ns; }, [] {});
	Check(sleeps == std::vector<uint64_t>({1000, 1000}),
	      "elapsed arithmetic does not overflow an absolute deadline");

	// Exercise the guest-facing wrappers too, without a tight host-scheduler upper bound.
	Check(KernelUsleep(0) == OK && KernelSleep(0) == OK, "zero sleep wrappers succeed");
	const auto start = Detail::SteadyNs();
	Check(KernelUsleep(2000) == OK, "microsecond sleep succeeds");
	Check(Detail::SteadyNs() - start >= 2000000, "microsecond sleep does not return early");
	KernelTimespec duration {0, 2000000};
	KernelTimespec remaining {1, 1};
	const auto nano_start = Detail::SteadyNs();
	Check(KernelNanosleep(&duration, &remaining) == OK, "nanosecond sleep succeeds");
	Check(Detail::SteadyNs() - nano_start >= 2000000, "nanosecond sleep does not return early");
	Check(remaining.tv_sec == 0 && remaining.tv_nsec == 0, "completed nanosleep has no remainder");
}

KernelEventFlag Create(uint32_t attr, uint64_t bits = 0) {
	KernelEventFlag flag = nullptr;
	Check(KernelCreateEventFlag(&flag, "kernel-waits-test", attr, bits, nullptr) == OK,
	      "create event flag");
	return flag;
}

// The production condition variable polls pending guest signals outside the flag's mutex.
// Observing that callback proves this thread is queued, without guessing at scheduler delays.
// Holding it there makes Set/Clear/Cancel ordering deterministic before the waiter resumes.
struct WaitState {
	std::atomic<bool> observed {false};
	std::atomic<bool> resume {false};
	uint64_t result = UINT64_MAX;
	int error = KERNEL_ERROR_EBUSY;
};

thread_local WaitState* current_wait = nullptr;

void ObserveWait() {
	if (current_wait != nullptr) {
		current_wait->observed.store(true, std::memory_order_release);
		WaitUntil([&] { return current_wait->resume.load(std::memory_order_acquire); },
		          "test waiter is allowed to resume");
	}
}

std::thread StartWait(KernelEventFlag flag, WaitState& state, uint64_t bits, uint32_t mode) {
	return std::thread([&, flag, bits, mode] {
		current_wait = &state;
		state.error = KernelWaitEventFlag(flag, bits, mode, &state.result, nullptr);
		current_wait = nullptr;
	});
}

void WaitQueued(WaitState& state) {
	WaitUntil([&] { return state.observed.load(std::memory_order_acquire); },
	          "event-flag waiter reaches its condition wait");
}

void Resume(WaitState& state) {
	state.resume.store(true, std::memory_order_release);
}

void TestSetThenClear() {
	auto* flag = Create(SINGLE);
	WaitState state;
	auto waiter = StartWait(flag, state, 1, AND);
	WaitQueued(state);
	Check(KernelPollEventFlag(flag, 1, OR, nullptr) == KERNEL_ERROR_EPERM,
	      "single-waiter flag rejects an additional waiter");
	Check(KernelSetEventFlag(flag, 1) == OK, "set releases the matching waiter");
	Check(KernelClearEventFlag(flag, 0) == OK, "clear flag before waiter resumes");
	int count = -1;
	Check(KernelCancelEventFlag(flag, 8, &count) == OK && count == 0,
	      "cancel does not count a waiter already released by set");
	Resume(state);
	waiter.join();
	Check(state.error == OK && state.result == 1,
	      "set result survives a clear and cancel before the host waiter resumes");
	Check(KernelDeleteEventFlag(flag) == OK, "delete set-clear flag");
}

void TestQueueOrder(uint32_t attributes, uint32_t clear_mode) {
	auto* flag = Create(attributes);
	WaitState first, second;
	auto first_thread = StartWait(flag, first, 1, OR | clear_mode);
	WaitQueued(first);
	auto second_thread = StartWait(flag, second, 2, OR);
	WaitQueued(second);
	Check(KernelSetEventFlag(flag, 3) == OK, "set two bits in queue order");
	int canceled = -1;
	Check(KernelCancelEventFlag(flag, 8, &canceled) == OK, "cancel remaining queued waiters");
	Resume(first);
	Resume(second);
	first_thread.join();
	second_thread.join();
	Check(first.error == OK && first.result == 3, "first waiter takes the original pattern");
	if (clear_mode == CLEAR_ALL) {
		Check(canceled == 1 && second.error == KERNEL_ERROR_ECANCELED && second.result == 0,
		      "first waiter's clear prevents the following waiter from consuming the same set");
	} else {
		Check(canceled == 0 && second.error == OK && second.result == 2,
		      "clear-pattern only consumes the first waiter's requested bits");
	}
	Check(KernelDeleteEventFlag(flag) == OK, "delete ordered-waiter flag");
}

void TestCancelAndReuse() {
	auto* flag = Create(SINGLE, 4);
	WaitState state;
	auto waiter = StartWait(flag, state, 1, AND);
	WaitQueued(state);
	int count = 0;
	Check(KernelCancelEventFlag(flag, 8, &count) == OK && count == 1,
	      "cancel reports the number of waiting threads");
	Resume(state);
	waiter.join();
	Check(state.error == KERNEL_ERROR_ECANCELED && state.result == 0,
	      "canceled waiter gets the cancellation result and zero pattern");
	uint64_t pattern = 0;
	Check(KernelPollEventFlag(flag, 8, AND, &pattern) == OK && pattern == 8,
	      "cancel installs the replacement pattern for subsequent waits");
	Check(KernelDeleteEventFlag(flag) == OK, "delete reused flag");
}

void TestDeleteWaiterLifetime() {
	auto* flag = Create(SINGLE, 4);
	WaitState waiting, deleting;
	deleting.resume.store(true);
	auto waiter = StartWait(flag, waiting, 1, AND);
	WaitQueued(waiting);
	std::atomic<bool> deleted {false};
	std::thread deleter([&] {
		current_wait = &deleting;
		Check(KernelDeleteEventFlag(flag) == OK, "delete a flag with a queued waiter");
		deleted.store(true, std::memory_order_release);
		current_wait = nullptr;
	});
	// The destructor must wait for the stack-owned waiter to stop using the flag's mutex.
	WaitQueued(deleting);
	Check(!deleted.load(std::memory_order_acquire), "delete retains the flag until its waiter leaves");
	Resume(waiting);
	waiter.join();
	deleter.join();
	Check(waiting.error == KERNEL_ERROR_EACCES && waiting.result == 0,
	      "deleted flag releases the waiter with access denied and zero pattern");
}

void TestTimeoutsAndValidation() {
	auto* flag = Create(0, 4);
	uint64_t pattern = 0;
	Check(KernelPollEventFlag(flag, 1, OR, &pattern) == KERNEL_ERROR_EBUSY && pattern == 4,
	      "poll reports the current pattern without a timed wait");
	const auto start = Detail::SteadyNs();
	for (int i = 0; i < 8; ++i) {
		KernelUseconds timeout = 0;
		Check(KernelWaitEventFlag(flag, 1, OR, &pattern, &timeout) == KERNEL_ERROR_ETIMEDOUT,
		      "zero timeout waits until the next kernel tick");
		Check(pattern == 4 && timeout == 0, "timeout returns current bits and zero remaining time");
	}
	Check(Detail::SteadyNs() - start >= 7000000, "successive zero timeouts each advance a tick");
	KernelUseconds timeout = 5000;
	const auto timed_start = Detail::SteadyNs();
	Check(KernelWaitEventFlag(flag, 1, OR, &pattern, &timeout) == KERNEL_ERROR_ETIMEDOUT,
	      "unsatisfied finite wait times out");
	Check(Detail::SteadyNs() - timed_start >= 5000000 && timeout == 0,
	      "tick rounding never shortens a requested timeout");
	timeout = std::numeric_limits<KernelUseconds>::max();
	Check(KernelWaitEventFlag(flag, 4, AND, &pattern, &timeout) == OK &&
	          timeout == std::numeric_limits<KernelUseconds>::max(),
	      "immediately satisfied wait preserves a full-width timeout");
	for (uint32_t mode: {0u, 3u, 0x31u}) {
		Check(KernelWaitEventFlag(flag, 1, mode, nullptr, nullptr) == KERNEL_ERROR_EINVAL &&
		          KernelPollEventFlag(flag, 1, mode, nullptr) == KERNEL_ERROR_EINVAL,
		      "invalid mode returns EINVAL instead of terminating the emulator");
	}
	Check(KernelPollEventFlag(flag, 0, OR, nullptr) == KERNEL_ERROR_EINVAL,
	      "zero wait pattern is invalid");
	Check(KernelPollEventFlag(nullptr, 1, 0, nullptr) == KERNEL_ERROR_EINVAL &&
	          KernelPollEventFlag(nullptr, 1, OR, nullptr) == KERNEL_ERROR_ESRCH,
	      "argument validation precedes null handle validation");
	Check(KernelDeleteEventFlag(flag) == OK, "delete timeout flag");
	for (uint32_t attr: {3u, 0x30u, 0x40u}) {
		Check(KernelCreateEventFlag(&flag, "invalid", attr, 0, nullptr) == KERNEL_ERROR_EINVAL,
		      "reject incompatible or unknown creation attributes");
	}
	Check(KernelCreateEventFlag(&flag, std::string(32, 'n').c_str(), 0, 0, nullptr) ==
	          KERNEL_ERROR_ENAMETOOLONG, "reject names exceeding 31 characters");
	Check(KernelCreateEventFlag(&flag, std::string(31, 'n').c_str(), 0, 0, nullptr) == OK,
	      "accept 31-character names");
	Check(KernelDeleteEventFlag(flag) == OK, "delete maximum-name flag");
}

} // namespace

int RunKernelWaitsTests() {
	Common::InitializeThreads();
	Common::Subsystems subsystems;
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	Config::Load(options);
	subsystems.Initialize<Log::Lifecycle>();
	TestSleepDeadline();
	// This selector runs in its own process, before guest pthread initialization. Restore the
	// default callback before returning; no game, graphics context or other test is running.
	Common::CondVar::SetWaitPollCallback(ObserveWait);
	TestSetThenClear();
	TestQueueOrder(MULTI, CLEAR_ALL);
	TestQueueOrder(MULTI | PRIORITY, CLEAR_ALL); // Equal priorities retain FIFO order.
	TestQueueOrder(MULTI, CLEAR_PAT);
	TestCancelAndReuse();
	TestDeleteWaiterLifetime();
	TestTimeoutsAndValidation();
	Common::CondVar::SetWaitPollCallback(nullptr);
	subsystems.Destroy();
	std::printf("KernelWaitsTests: deadline sleep and event-flag cases passed\n");
	return 0;
}
