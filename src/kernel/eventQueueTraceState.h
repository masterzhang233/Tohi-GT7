#ifndef KYTY_KERNEL_EVENT_QUEUE_TRACE_STATE_H_
#define KYTY_KERNEL_EVENT_QUEUE_TRACE_STATE_H_

#include <array>
#include <cstddef>
#include <cstdint>

// Diagnostic provenance only. All mutation occurs under the existing queue mutex.
// This sidecar never owns, allocates, removes, or changes a guest pending event.
namespace Libs::LibKernel::EventQueue::TraceState {

enum Flag : uint32_t {
	Unknown = 1u << 0,
	PreArm = 1u << 1,
	Overflow = 1u << 2,
	Coalesced = 1u << 3,
	Repeated = 1u << 4,
	Unsupported = 1u << 5,
	Initial = 1u << 6,
};

enum class Semantics { Queued, Coalesced, Unknown };

struct Occurrence {
	uint64_t first = 0, last = 0, count = 0;
	uint32_t flags = 0;
};

class State {
public:
	static constexpr size_t PendingCapacity = 32;

	uint64_t Registration() const noexcept { return m_registration; }

	void Initialize(uint64_t registration, bool triggered, size_t pending, bool prearm) noexcept {
		m_registration = registration;
		Clear();
		if (triggered || pending != 0) {
			Lose(prearm ? PreArm : Initial);
		}
	}

	// token identifies this invocation, even if no exact delivery can be attributed
	// to it. The caller records its timestamp/payload separately in the trace ring.
	void ObserveTrigger(uint64_t token, bool before_triggered, size_t before_pending,
	                    bool after_triggered, size_t after_pending, Semantics semantics) noexcept {
		Check(before_triggered, before_pending);
		if (!after_triggered) {
			Lose(Unsupported);
			Check(after_triggered, after_pending);
			return;
		}
		if (token == 0 || semantics == Semantics::Unknown) {
			Lose(Unsupported);
			return;
		}
		if ((m_current.flags & Unknown) != 0) {
			return;
		}
		if (!before_triggered && before_pending == 0 && after_pending == 0) {
			m_current = {token, token, 1, 0};
			return;
		}
		if (semantics == Semantics::Queued && before_triggered &&
		    after_pending > before_pending && after_pending - before_pending == 1) {
			if (m_count == PendingCapacity) {
				Lose(Overflow);
				return;
			}
			m_pending[(m_head + m_count) % PendingCapacity] = token;
			++m_count;
			return;
		}
		if (semantics == Semantics::Coalesced && before_triggered && before_pending == 0 &&
		    after_pending == 0) {
			m_current.last = token;
			++m_current.count;
			m_current.flags |= Coalesced;
			return;
		}
		Lose(Unsupported);
	}

	Occurrence CurrentOccurrence() const noexcept { return m_current; }

	// Call only after the original reset and optional pending pop. The bool comes
	// from the actual functional pop, not from the diagnostic FIFO's contents.
	void AfterDelivery(bool triggered, size_t pending, bool popped_pending) noexcept {
		if ((m_current.flags & Unknown) == 0) {
			if (popped_pending) {
				if (m_count == 0) {
					Lose(Unsupported);
				} else {
					const auto token = m_pending[m_head];
					m_head = (m_head + 1) % PendingCapacity;
					--m_count;
					m_current = {token, token, 1, 0};
				}
			} else if (triggered) {
				m_current.flags |= Repeated;
			} else {
				m_current = {};
			}
		}
		Check(triggered, pending);
	}

	// Check before reading current provenance, including a first delivery of an
	// already-ready event. Uncertainty is sticky until the guest queue is empty.
	void Check(bool triggered, size_t pending) noexcept {
		if (!triggered && pending == 0) {
			Clear();
			return;
		}
		if ((m_current.flags & Unknown) == 0 &&
		    (triggered != (m_current.first != 0) || pending != m_count)) {
			Lose(Unsupported);
		}
	}

private:
	void Clear() noexcept {
		m_current = {};
		m_head = 0;
		m_count = 0;
	}
	void Lose(uint32_t reason) noexcept {
		m_current = {0, 0, 0, m_current.flags | Unknown | reason};
		m_head = 0;
		m_count = 0;
	}

	uint64_t m_registration = 0;
	Occurrence m_current {};
	std::array<uint64_t, PendingCapacity> m_pending {};
	size_t m_head = 0, m_count = 0;
};

} // namespace Libs::LibKernel::EventQueue::TraceState

#endif
