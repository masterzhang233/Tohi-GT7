#ifndef KYTY_COMMON_SOURCE_TRACE_H_
#define KYTY_COMMON_SOURCE_TRACE_H_

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

// Opt-in source-frame diagnosis. Producers only copy POD into bounded slots: no
// allocation, logging, waiting, or access to the renderer from an exception handler.
namespace Common::SourceTrace {
enum class Kind : uint32_t { FlipSubmit, FlipPrepare, FlipReady, Process, Command, Fault, CondWait, CondWake };
struct Event {
	Kind kind {};
	uint32_t flags = 0, tid = 0;
	uint64_t begin_ns = 0, end_ns = 0, work_end_ns = 0;
	uint64_t submission_id = 0, request_id = 0, arg0 = 0, arg1 = 0;
};
struct Context {
	uint64_t submission_id = 0, request_id = 0;
	uint32_t queue_id = 0, type = 0;
};
struct ThreadIds {
	uint32_t producer_tid = 0, notifier_tid = 0;
};
inline thread_local Context t_context {};
namespace Detail {
inline std::atomic<bool> g_enabled {false};
inline std::atomic<bool> g_chain_enabled {false};
// 1 means immediate capture (legacy behavior); 0 waits for the reporter's one-shot
// arm request. A real timestamp rejects scopes straddling arming. Never reset live.
inline std::atomic<uint64_t> g_arm_ns {1};
inline constexpr size_t ConditionThreadLimit = 32;
inline constexpr uint64_t BaseRecordBudget = 120'000, ExtraConditionRecordBudget = 360'000;

// Diagnostic admission has a fixed lifetime budget. Exhaustion does not disable
// source tracing or restore synchronous per-frame logging.
template <uint64_t Limit> class RecordBudget {
	std::atomic<uint64_t> m_attempted {0};
public:
	bool TryAdmit() noexcept { return m_attempted.fetch_add(1, std::memory_order_relaxed) < Limit; }
	bool Open() const noexcept { return Attempted() < Limit; }
	uint64_t Attempted() const noexcept { return m_attempted.load(std::memory_order_relaxed); }
	uint64_t Skipped() const noexcept { const auto n = Attempted(); return n > Limit ? n - Limit : 0; }
};

// Append-only historical coverage, not a dependency graph or thread-lifetime
// identity. Old producers and reused numeric OS TIDs can broaden coverage. No
// allocation, lock, reset, removal, or retry loop is needed on the notifying lane.
template <size_t Capacity> class ConditionThreadRegistry {
	static_assert(Capacity > 0);
	std::array<std::atomic<uint32_t>, Capacity> m_tids {};
	std::atomic<uint64_t> m_admitted {0}, m_rejected {0};
public:
	bool Contains(uint32_t tid) const noexcept {
		if (tid == 0) return false;
		for (const auto& slot: m_tids) {
			if (slot.load(std::memory_order_relaxed) == tid) return true;
		}
		return false;
	}
	bool Admit(uint32_t tid) noexcept {
		if (tid == 0) return false;
		for (auto& slot: m_tids) {
			auto current = slot.load(std::memory_order_relaxed);
			if (current == tid) return true;
			if (current != 0) continue;
			if (slot.compare_exchange_strong(current, tid, std::memory_order_relaxed)) {
				m_admitted.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
			// A concurrent identical admission owns this slot; do not duplicate it.
			if (current == tid) return true;
		}
		m_rejected.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	uint32_t At(size_t index) const noexcept {
		return index < Capacity ? m_tids[index].load(std::memory_order_relaxed) : 0;
	}
	uint64_t Admitted() const noexcept { return m_admitted.load(std::memory_order_relaxed); }
	// Rejections count attempts, not distinct threads. Later wakes may retry.
	uint64_t Rejected() const noexcept { return m_rejected.load(std::memory_order_relaxed); }
};
inline ConditionThreadRegistry<ConditionThreadLimit> g_condition_threads;
inline RecordBudget<ExtraConditionRecordBudget> g_extra_condition_budget;
inline std::atomic<uint64_t> g_chain_budget_blocked {0};
// One snapshot prevents a late wake for the old producer from attaching its notifier
// to a newly observed producer. High 32 bits: producer; low 32 bits: notifier.
inline std::atomic<uint64_t> g_observed_threads {0};

inline bool ValidConditionWake(const Event& event) noexcept {
	return event.kind == Kind::CondWake && event.tid != 0 && event.request_id != 0 &&
	       event.flags >= 1 && event.flags <= 3 && event.arg0 != 0 && event.arg0 <= UINT32_MAX &&
	       event.begin_ns != 0 && event.begin_ns == event.work_end_ns && event.begin_ns == event.end_ns;
}

// Bucket priority is evaluated at Record time; it is not an identity or causal
// assertion about a wait that may have started before a producer handoff.
inline bool UseExtraConditionBucket(const Event& event, uint32_t producer_tid) noexcept {
	return g_chain_enabled.load(std::memory_order_relaxed) &&
	       ((event.kind == Kind::CondWait && event.tid != producer_tid) ||
	        (event.kind == Kind::CondWake && event.arg0 != producer_tid));
}

inline void ObserveThreadEvent(const Event& event) noexcept {
	if (event.tid == 0) return;
	auto observed = g_observed_threads.load(std::memory_order_relaxed);
	if (g_chain_enabled.load(std::memory_order_relaxed)) {
		if (event.kind == Kind::FlipSubmit) {
			(void)g_condition_threads.Admit(event.tid);
		} else if (ValidConditionWake(event) &&
		           (event.arg0 == static_cast<uint32_t>(observed >> 32u) ||
		            g_condition_threads.Contains(static_cast<uint32_t>(event.arg0)))) {
			(void)g_condition_threads.Admit(event.tid);
		}
	}
	if (event.kind == Kind::FlipSubmit) {
		while (static_cast<uint32_t>(observed >> 32u) != event.tid) {
			if (g_observed_threads.compare_exchange_strong(
			        observed, static_cast<uint64_t>(event.tid) << 32u,
			        std::memory_order_relaxed)) return;
		}
	} else if (ValidConditionWake(event)) {
		// Only notifications to the current producer select a sampling target.
		// A notification to that notifier cannot extend the selection recursively.
		while (static_cast<uint32_t>(observed >> 32u) == event.arg0) {
			const auto desired = (observed & 0xffffffff00000000ull) | event.tid;
			if (desired == observed || g_observed_threads.compare_exchange_strong(
			        observed, desired, std::memory_order_relaxed)) return;
		}
	}
}

// Multiple producers, exactly one consumer. Slot publication owns the payload;
// ticket wrap does not overwrite a live payload. Contention/full rings drop instead
// of blocking the caller. Drain order is NOT chronological; use event timestamps.
template <size_t Capacity, typename Payload = Event> class Ring {
	static_assert(Capacity > 0);
	struct Slot {
		std::atomic<uint32_t> state {0}; // free, writing, ready
		Payload event {};
	};
	std::array<Slot, Capacity> m_slots {};
	std::atomic<uint64_t> m_next {0}, m_captured {0}, m_dropped {0};
public:
	bool TryPush(const Payload& event) noexcept {
		auto& slot = m_slots[m_next.fetch_add(1, std::memory_order_relaxed) % Capacity];
		uint32_t free = 0;
		if (!slot.state.compare_exchange_strong(free, 1, std::memory_order_acquire)) {
			m_dropped.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		slot.event = event;
		m_captured.fetch_add(1, std::memory_order_relaxed);
		slot.state.store(2, std::memory_order_release);
		return true;
	}
	template <typename Consume> size_t Drain(Consume&& consume) {
		size_t count = 0;
		for (auto& slot: m_slots) {
			if (slot.state.load(std::memory_order_acquire) != 2) continue;
			const Payload event = slot.event;
			slot.state.store(0, std::memory_order_release);
			consume(event);
			++count;
		}
		return count;
	}
	uint64_t Captured() const noexcept { return m_captured.load(std::memory_order_relaxed); }
	uint64_t Dropped() const noexcept { return m_dropped.load(std::memory_order_relaxed); }
};
} // namespace Detail

[[nodiscard]] inline bool Enabled() noexcept {
	return Detail::g_enabled.load(std::memory_order_acquire);
}
[[nodiscard]] inline uint64_t ArmTimestamp() noexcept {
	return Detail::g_arm_ns.load(std::memory_order_acquire);
}
[[nodiscard]] inline bool Armed() noexcept { return Enabled() && ArmTimestamp() != 0; }
[[nodiscard]] inline bool TryArm(uint64_t now_ns) noexcept {
	if (!Enabled() || now_ns <= 1) return false;
	uint64_t expected = 0;
	return Detail::g_arm_ns.compare_exchange_strong(expected, now_ns, std::memory_order_release);
}
[[nodiscard]] inline bool ChainEnabled() noexcept {
	return Enabled() && Detail::g_chain_enabled.load(std::memory_order_relaxed);
}
// Latest observed successful CPU flip caller. Sampling follows the actual caller,
// including a thread absent from the optional named-thread registry. A TID is an
// observation, not proof that all upstream game work runs on that thread.
[[nodiscard]] inline uint32_t ProducerThreadId() noexcept {
	return static_cast<uint32_t>(Detail::g_observed_threads.load(std::memory_order_relaxed) >> 32u);
}
// Last observed valid notifier of that producer, not an inferred job dependency.
// A new producer clears it; repeated submissions from the same producer retain it.
[[nodiscard]] inline uint32_t NotifierThreadId() noexcept {
	return static_cast<uint32_t>(Detail::g_observed_threads.load(std::memory_order_relaxed));
}
[[nodiscard]] inline ThreadIds ObservedThreadIds() noexcept {
	const auto observed = Detail::g_observed_threads.load(std::memory_order_relaxed);
	return {static_cast<uint32_t>(observed >> 32u), static_cast<uint32_t>(observed)};
}
[[nodiscard]] inline bool ExtraConditionAdmissionOpen() noexcept {
	return ChainEnabled() && Detail::g_extra_condition_budget.Open();
}
// Ordinary-wait coverage only. The current producer remains eligible even when
// the historical registry is full; its 32 entries are not an absolute participant
// cap. Exhaustion prevents new extra waits, while already-started waits complete
// through the existing Record path and can be counted as budget-skipped.
[[nodiscard]] inline bool ConditionThreadEligible(uint32_t tid) noexcept {
	if (!Armed() || tid == 0) return false;
	if (tid == ProducerThreadId()) return true;
	if (!ChainEnabled() || !Detail::g_condition_threads.Contains(tid)) return false;
	if (Detail::g_extra_condition_budget.Open()) return true;
	Detail::g_chain_budget_blocked.fetch_add(1, std::memory_order_relaxed);
	return false;
}
[[nodiscard]] inline uint64_t NowNs() noexcept {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count());
}
void Record(Event event) noexcept;
// Called once during PerfStats startup, never from a fault. Requires a running
// reporter; KYTY_PERF=0 explicitly leaves this facility disabled.
void Initialize(bool reporter_enabled);
// Single consumer: existing PerfReport thread, once per 250 ms when enabled.
void Drain();
} // namespace Common::SourceTrace
#endif
