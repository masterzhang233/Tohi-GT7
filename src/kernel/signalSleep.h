#ifndef KYTY_KERNEL_SIGNAL_SLEEP_H_
#define KYTY_KERNEL_SIGNAL_SLEEP_H_

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace Libs::LibKernel::Detail {

inline uint64_t SteadyNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Count actual elapsed time, including oversleep and signal dispatch, instead of accumulating
// requested slices. Elapsed subtraction also avoids overflowing an absolute deadline.
template <class Clock, class Sleep, class Dispatch>
void SleepWithSignalPoll(uint64_t duration_ns, uint64_t poll_ns, const Clock& now,
                         const Sleep& sleep, const Dispatch& dispatch) {
	if (duration_ns == 0) {
		dispatch();
		return;
	}
	const uint64_t start = now();
	for (uint64_t elapsed = 0; elapsed < duration_ns; elapsed = now() - start) {
		sleep(std::min(duration_ns - elapsed, poll_ns));
		dispatch();
	}
}

} // namespace Libs::LibKernel::Detail

#endif // KYTY_KERNEL_SIGNAL_SLEEP_H_
