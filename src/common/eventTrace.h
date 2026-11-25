#ifndef KYTY_COMMON_EVENT_TRACE_H_
#define KYTY_COMMON_EVENT_TRACE_H_

#include "common/perfStats.h"
#include "common/sourceTrace.h"

#include <atomic>
#include <cstdint>

// Opt-in diagnostic only. No guest ABI, notification, queue or wait policy changes.
// IDs are process-local opaque identities; CPU timestamps are steady_clock ns.
namespace Common::EventTrace {
enum class Kind : uint32_t {
	WaitBegin = 1, WaitEnd, Delivery, Trigger, Registration,
	CallbackQueued, TickSubmit, TickWait, CallbackBegin, CallbackEnd,
};
enum Flag : uint32_t {
	Unknown = 1u << 0, PreArm = 1u << 1, Overflow = 1u << 2,
	Coalesced = 1u << 3, Repeated = 1u << 4, Timer = 1u << 5,
	Direct = 1u << 6, GenericCallback = 1u << 7,
	AlreadyReady = 1u << 8, Closed = 1u << 9,
};

// Identity conventions:
// Wait*: id=wait invocation; parent_id=0; queue_id=queue handle.
// Delivery: id=occurrence (0 if unknown), parent_id=wait invocation.
// Trigger: id=occurrence, parent_id=callback (0 for direct/unknown origins).
// Registration: id=registration generation. Duplicate Add preserves generation.
// Callback*: id=callback; TickWait uses the callback id; TickSubmit id=0.
// Delivery/Trigger carry full signed filter, ident and event payload. index is
// returned-item index; count is multiplicity/count; extra carries a kind-specific
// timeout/deadline/last-trigger value, documented by the producer and parser.
struct Event {
	Kind kind {};
	uint32_t flags = 0, tid = 0;
	int32_t filter = 0, result = 0;
	uint32_t count = 0, index = 0, fflags = 0, event_flags = 0, mode = 0;
	uint64_t time_ns = 0, begin_ns = 0;
	uint64_t id = 0, parent_id = 0, queue_id = 0, registration_id = 0;
	uint64_t scheduler_id = 0, tick = 0, submission_id = 0, request_id = 0;
	uint64_t ident = 0, data = 0, extra = 0;
};

struct CallbackContext {
	uint64_t id = 0, scheduler_id = 0, tick = 0;
	uint64_t submission_id = 0, request_id = 0, scheduled_ns = 0;
	uint32_t source_queue = 0, source_type = 0, flags = 0;
	int32_t event_id = 0;
	uint32_t context_id = 0;
};

// Owned by the command-buffer producer, like the scheduler's draw/dispatch
// counters. Observe only after a priority callback has actually been queued.
// Submit consumes the marker before handing a by-value job to another thread.
// A different tick/scheduler or an empty pre-arm context never selects a job.
class PriorityTickSelection {
public:
	void ObserveQueued(const CallbackContext& context, uint64_t scheduler_id,
	                   uint64_t queued_tick) noexcept {
		if (context.id != 0 && context.scheduler_id == scheduler_id &&
		    context.tick == queued_tick) {
			m_tick = queued_tick;
			m_selected = true;
		}
	}
	[[nodiscard]] bool Take(uint64_t submitted_tick) noexcept {
		const bool selected = m_selected && m_tick == submitted_tick;
		m_selected = false;
		m_tick = 0;
		return selected;
	}
private:
	uint64_t m_tick = 0;
	bool m_selected = false;
};

namespace Detail {
inline std::atomic<bool> g_enabled {false};
inline std::atomic<uint64_t> g_next_id {1};
inline std::atomic<uint64_t> g_unselected_ticks {0};
}

[[nodiscard]] inline bool Enabled() noexcept {
	return Detail::g_enabled.load(std::memory_order_acquire) && SourceTrace::Armed();
}
// Historical coverage only, independent of the ordinary condition event quota.
// Registration does not establish a current dependency or OS thread lifetime.
[[nodiscard]] inline bool ThreadEligible(uint32_t tid) noexcept {
	return Enabled() && tid != 0 && (tid == SourceTrace::ProducerThreadId() ||
	    SourceTrace::Detail::g_condition_threads.Contains(tid));
}
[[nodiscard]] inline uint64_t NowNs() noexcept { return SourceTrace::NowNs(); }
// May be used once for scheduler identity before arming; it never denotes an event.
[[nodiscard]] inline uint64_t NewIdentity() noexcept {
	return Detail::g_next_id.fetch_add(1, std::memory_order_relaxed);
}
[[nodiscard]] inline uint32_t ThreadId() noexcept { return PerfStats::CurrentOsThreadId(); }
// Active selection is separate from admission/ring loss. Do not count jobs
// handed off before arming, even if the submission worker sees them later.
inline void NoteUnselectedTick(uint64_t handoff_ns) noexcept {
	if (Enabled() && handoff_ns != 0 && handoff_ns >= SourceTrace::ArmTimestamp()) {
		Detail::g_unselected_ticks.fetch_add(1, std::memory_order_relaxed);
	}
}
void Record(Event event) noexcept;
void Initialize(bool reporter_enabled);
void Drain();

[[nodiscard]] inline CallbackContext CaptureCallback(uint64_t scheduler_id, uint64_t tick,
	int32_t event_id = 0, uint32_t context_id = 0, uint32_t flags = GenericCallback) noexcept {
	if (!Enabled()) return {};
	const auto source = SourceTrace::t_context;
	return {NewIdentity(), scheduler_id, tick, source.submission_id, source.request_id,
	        NowNs(), source.queue_id, source.type, flags, event_id, context_id};
}

inline Event CallbackEvent(Kind kind, const CallbackContext& context,
	uint64_t begin_ns = 0, uint64_t time_ns = 0) noexcept {
	Event event {};
	event.kind = kind;
	event.flags = context.flags;
	event.id = context.id;
	event.scheduler_id = context.scheduler_id;
	event.tick = context.tick;
	event.submission_id = context.submission_id;
	event.request_id = context.request_id;
	event.ident = static_cast<uint64_t>(static_cast<int64_t>(context.event_id));
	event.data = context.context_id;
	event.index = context.source_queue;
	event.mode = context.source_type;
	event.extra = context.scheduled_ns;
	event.begin_ns = begin_ns;
	event.time_ns = time_ns;
	return event;
}

inline void EmitCallback(Kind kind, const CallbackContext& context,
	uint64_t begin_ns = 0, uint64_t time_ns = 0) noexcept {
	if (context.id != 0 && Enabled()) Record(CallbackEvent(kind, context, begin_ns, time_ns));
}
} // namespace Common::EventTrace
#endif
