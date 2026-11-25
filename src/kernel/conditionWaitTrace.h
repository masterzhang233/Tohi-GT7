#ifndef KYTY_KERNEL_CONDITION_WAIT_TRACE_H_
#define KYTY_KERNEL_CONDITION_WAIT_TRACE_H_

#include "common/sourceTrace.h"

#include <atomic>
#include <cstdint>

// Diagnostic state belongs to the existing pthread waiter. Matching-owner POD accesses
// use the condition's existing mutex. The atomic tag prevents a nested/borrowed guest
// identity using another condition mutex from touching that POD. No new lock is taken.
namespace Libs::LibKernel::ConditionWaitTrace {

struct State {
	// A detached signal helper can retain this guest identity across multiple waits.
	// Once borrowed, exclude future attribution for this object, including pool reuse.
	std::atomic<bool> borrowed_identity {false};
	// Condition objects are aligned. Bit 0 marks ambiguous reentrancy for the entire
	// outer wait. The owner address is only an internal tag and is never recorded.
	std::atomic<uintptr_t> owner {0};
	uint64_t id = 0, begin_ns = 0;
	uint32_t waiter_tid = 0, api = 0;
};

inline std::atomic<uint64_t> g_next_wait_id {0};

inline bool NoteNested(State& state) noexcept {
	auto owner = state.owner.load(std::memory_order_acquire);
	while (owner != 0) {
		if ((owner & 1u) != 0) return true;
		// Retry only if ownership changed. Giving up on a replacement owner could
		// allow a later notification to be attributed to an ambiguous outer wait.
		if (state.owner.compare_exchange_strong(owner, owner | uintptr_t {1},
		                                     std::memory_order_acq_rel)) return true;
	}
	return false;
}

inline void MarkBorrowed(State& state) noexcept {
	state.borrowed_identity.store(true, std::memory_order_release);
	(void)NoteNested(state);
}

inline bool Begin(State& state, const void* condition, uint32_t waiter_tid, uint32_t api) noexcept {
	// Even a different OS thread borrowing this guest identity, or a caller no longer
	// the latest producer, can invalidate the outer wait's notification attribution.
	if (NoteNested(state) || state.borrowed_identity.load(std::memory_order_acquire)) return false;
	const auto owner = reinterpret_cast<uintptr_t>(condition);
	if (owner == 0 || (owner & 1u) != 0 || !Common::SourceTrace::ConditionThreadEligible(waiter_tid)) {
		return false;
	}
	uintptr_t empty = 0;
	if (!state.owner.compare_exchange_strong(empty, owner, std::memory_order_acq_rel)) {
		(void)NoteNested(state);
		return false;
	}
	// The matching condition mutex is held until after these fields are initialized.
	state.id = g_next_wait_id.fetch_add(1, std::memory_order_relaxed) + 1;
	state.begin_ns = Common::SourceTrace::NowNs();
	state.waiter_tid = waiter_tid;
	state.api = api;
	return true;
}

// Called just before notify_one. Publication follows the notification while the same
// condition mutex still protects the waiter; the timestamp immediately precedes that call.
inline Common::SourceTrace::Event MakeWake(const State& state, const void* condition, uint64_t sequence,
                                           uint32_t wake_kind) noexcept {
	Common::SourceTrace::Event event {};
	const auto owner = reinterpret_cast<uintptr_t>(condition);
	if (owner != 0 && (owner & 1u) == 0 &&
	    !state.borrowed_identity.load(std::memory_order_acquire) &&
	    state.owner.load(std::memory_order_acquire) == owner) {
		event.kind = Common::SourceTrace::Kind::CondWake;
		event.flags = wake_kind;
		event.begin_ns = Common::SourceTrace::NowNs();
		event.work_end_ns = event.end_ns = event.begin_ns;
		event.request_id = state.id;
		// The notifier's current GPU submission context, not a flip request or the
		// originating guest job. Zero means no submission context is active here.
		event.submission_id = Common::SourceTrace::t_context.submission_id;
		event.arg0 = state.waiter_tid;
		event.arg1 = sequence;
	}
	return event;
}

inline void PublishWake(Common::SourceTrace::Event event) noexcept {
	if (event.request_id != 0) Common::SourceTrace::Record(event);
}

// Copy the completed condition-wait phase, then clear its published identity before
// releasing the condition mutex. The local event survives guest-mutex reacquisition.
inline Common::SourceTrace::Event CaptureReady(State& state, const void* condition,
                                               bool aborted = false) noexcept {
	Common::SourceTrace::Event event {};
	const auto owner = reinterpret_cast<uintptr_t>(condition);
	if (owner != 0 && (owner & 1u) == 0 &&
	    (state.owner.load(std::memory_order_acquire) & ~uintptr_t {1}) == owner) {
		event.kind = Common::SourceTrace::Kind::CondWait;
		event.flags = state.api | (aborted ? 256u : 0u);
		event.tid = state.waiter_tid;
		event.begin_ns = state.begin_ns;
		event.work_end_ns = Common::SourceTrace::NowNs();
		event.request_id = state.id;
		state.id = state.begin_ns = 0;
		state.waiter_tid = state.api = 0;
		// Combining ownership and uncertainty in this exchange prevents a concurrent
		// nested entry's mark from being lost between a separate flag read and reset.
		if ((state.owner.exchange(0, std::memory_order_acq_rel) & 1u) != 0 ||
		    state.borrowed_identity.load(std::memory_order_acquire)) event.flags |= 512u;
	}
	return event;
}

// native_result is the native errno result before the existing guest-error mapping.
// An aborted record ends after failed guest-mutex unlock, without a reacquire phase.
inline void Complete(Common::SourceTrace::Event event, uint64_t poll_returns,
                     int native_result) noexcept {
	if (event.request_id == 0) return;
	event.end_ns = Common::SourceTrace::NowNs();
	event.arg0 = poll_returns;
	event.arg1 = static_cast<uint32_t>(native_result);
	Common::SourceTrace::Record(event);
}

} // namespace Libs::LibKernel::ConditionWaitTrace

#endif // KYTY_KERNEL_CONDITION_WAIT_TRACE_H_
