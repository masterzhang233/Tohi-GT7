#include "kernel/eventQueue.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/eventTrace.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/timer.h"
#include "graphics/host_gpu/timeline.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fmt/format.h>
#include <limits>
#include <list>
#include <unordered_map>
#include <vector>

namespace Libs::LibKernel::EventQueue {

LIB_NAME("libkernel", "libkernel");

constexpr uint16_t EV_ADD     = 0x01;
constexpr uint16_t EV_ONESHOT = 0x10;
constexpr uint16_t EV_CLEAR   = 0x20;
constexpr uint16_t EV_ERROR   = 0x4000;

static uint64_t MonotonicTimeNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

static std::unordered_map<KernelEqueue, KernelEqueueRef> g_equeues;
static Common::Mutex                                     g_equeues_mutex;
static uint64_t                                          g_next_equeue = 1;

namespace Trace = Common::EventTrace;

// Stack-owned invocation identity. No TLS pairing: nested/parallel waits cannot
// replace another wait's queue or output-item identity.
struct TraceWait {
	uint64_t id = 0, begin_ns = 0, queue = 0, timeout = 0;
	uint32_t tid = 0, mode = 0;
	bool waited = false;
};

static uint32_t TraceFlags(uint32_t state) {
	uint32_t flags = 0;
	if ((state & TraceState::Unknown) != 0) flags |= Trace::Unknown;
	if ((state & TraceState::PreArm) != 0) flags |= Trace::PreArm;
	if ((state & TraceState::Overflow) != 0) flags |= Trace::Overflow;
	if ((state & TraceState::Coalesced) != 0) flags |= Trace::Coalesced;
	if ((state & TraceState::Repeated) != 0) flags |= Trace::Repeated;
	return flags;
}

static Trace::Event QueueTraceEvent(Trace::Kind kind, KernelEqueue queue,
                                    const KernelEqueueEvent& event) {
	Trace::Event record {};
	record.kind = kind;
	record.queue_id = static_cast<uint64_t>(queue);
	record.registration_id = event.trace_state.Registration();
	record.filter = event.event.filter;
	record.ident = event.event.ident;
	record.data = static_cast<uint64_t>(event.event.data);
	record.fflags = event.event.fflags;
	record.event_flags = event.event.flags;
	if (event.event.filter == KERNEL_EVFILT_TIMER || event.event.filter == KERNEL_EVFILT_HRTIMER) {
		record.flags |= Trace::Timer;
	}
	return record;
}

// Registration mode: 1=new, 2=first observed existing, 3=update, 4=delete, 5=close.
static void RecordRegistration(KernelEqueue queue, const KernelEqueueEvent& event, uint32_t mode) {
	auto record = QueueTraceEvent(Trace::Kind::Registration, queue, event);
	record.id = event.trace_state.Registration();
	record.mode = mode;
	record.flags |= TraceFlags(event.trace_state.CurrentOccurrence().flags);
	Trace::Record(record);
}

static void EnsureTraceRegistration(KernelEqueue queue, KernelEqueueEvent& event, bool prearm) {
	if (event.trace_state.Registration() == 0) {
		event.trace_state.Initialize(Trace::NewIdentity(), event.triggered,
		                             event.pending_events.size(), prearm);
		RecordRegistration(queue, event, prearm ? 2u : 1u);
	}
}

static TraceWait BeginTraceWait(KernelEqueue queue, int num, uint32_t mode, uint64_t timeout) {
	if (!Trace::Enabled()) return {};
	const auto tid = Trace::ThreadId();
	if (!Trace::ThreadEligible(tid)) return {};
	TraceWait wait {Trace::NewIdentity(), Trace::NowNs(), static_cast<uint64_t>(queue), timeout,
	                tid, mode, false};
	Trace::Event record {};
	record.kind = Trace::Kind::WaitBegin;
	record.id = wait.id;
	record.queue_id = wait.queue;
	record.tid = tid;
	record.time_ns = wait.begin_ns;
	record.begin_ns = wait.begin_ns;
	record.mode = mode; // 0=infinite, 1=zero-time poll, 2=timed.
	record.extra = timeout;
	record.count = static_cast<uint32_t>(num);
	Trace::Record(record);
	return wait;
}

static void EndTraceWait(const TraceWait& wait, int count) {
	if (wait.id == 0) return;
	Trace::Event record {};
	record.kind = Trace::Kind::WaitEnd;
	record.id = wait.id;
	record.queue_id = wait.queue;
	record.tid = wait.tid;
	record.begin_ns = wait.begin_ns;
	record.time_ns = Trace::NowNs();
	record.mode = wait.mode;
	record.extra = wait.timeout;
	record.count = count > 0 ? static_cast<uint32_t>(count) : 0;
	record.result = count == KERNEL_ERROR_EBADF ? KERNEL_ERROR_EBADF :
	                count == 0 ? KERNEL_ERROR_ETIMEDOUT : OK;
	if (count == KERNEL_ERROR_EBADF) record.flags |= Trace::Closed;
	if (!wait.waited && count > 0) record.flags |= Trace::AlreadyReady;
	Trace::Record(record);
}

static void RecordTriggerFailure(KernelEqueue queue, uintptr_t ident, int16_t filter,
                                 const Trace::CallbackContext* trace, int result, bool observed) {
	if (!Trace::Enabled() || (!observed && (trace == nullptr || trace->id == 0))) return;
	Trace::Event record {};
	record.kind = Trace::Kind::Trigger;
	record.queue_id = static_cast<uint64_t>(queue);
	record.ident = ident;
	record.filter = filter;
	record.result = result;
	record.flags = Trace::Unknown;
	if (result == KERNEL_ERROR_EBADF) record.flags |= Trace::Closed;
	if (trace != nullptr && trace->id != 0) {
		record.parent_id = trace->id;
		record.scheduler_id = trace->scheduler_id;
		record.tick = trace->tick;
		record.submission_id = trace->submission_id;
		record.request_id = trace->request_id;
	} else {
		record.flags |= trace == nullptr ? Trace::Direct : Trace::PreArm;
	}
	Trace::Record(record); // No occurrence or registration exists for this failed trigger.
}

class KernelEqueuePrivate {
public:
	explicit KernelEqueuePrivate(KernelEqueue handle): m_handle(handle) {}
	virtual ~KernelEqueuePrivate();

	KYTY_CLASS_NO_COPY(KernelEqueuePrivate);

	[[nodiscard]] const std::string& GetName() const { return m_name; }
	void                             SetName(const std::string& m_name) { this->m_name = m_name; }

	int AddEvent(const KernelEqueueEvent& event);
	int TriggerEvent(uintptr_t ident, int16_t filter, void* trigger_data,
	                 const Trace::CallbackContext* trace = nullptr);
	int DeleteEvent(uintptr_t ident, int16_t filter);

	int  GetTriggeredEvents(KernelEvent* ev, int num, TraceWait* trace = nullptr);
	int  WaitForEvents(KernelEvent* ev, int num, uint32_t micros, TraceWait* trace = nullptr);
	void Close();

	// Guests poll some queues with short timeouts in a tight loop. Only the first
	// LOGGED_TIMEOUTS consecutive timed-out waits are logged; the rest are counted and reported
	// by the next wait that receives events.
	static constexpr uint32_t LOGGED_TIMEOUTS = 4;
	std::atomic<uint32_t>     consecutive_timeouts {0};

private:
	void TriggerExpiredTimers(uint64_t now_ns);
	bool GetNextTimerWaitMicros(uint64_t now_ns, uint32_t* wait_micros) const;

	std::list<KernelEqueueEvent> m_events;
	Common::Mutex                m_mutex;
	Common::CondVar              m_cond_var;
	std::string                  m_name;
	KernelEqueue                 m_handle = KERNEL_EQUEUE_INVALID;
	bool                         m_closed = false;
	bool                         m_trace_observed = false; // guarded by m_mutex
};

KernelEqueuePrivate::~KernelEqueuePrivate() {
	Close();
}

void KernelEqueuePrivate::Close() {
	Common::LockGuard lock(m_mutex);

	if (m_closed) {
		return;
	}
	m_closed = true;
	for (auto& event: m_events) {
		if (m_trace_observed && Trace::Enabled() && event.trace_state.Registration() != 0) {
			RecordRegistration(m_handle, event, 5);
		}
		if (event.filter.delete_event_func != nullptr) {
			auto owner = event.filter.owner;
			event.filter.delete_event_func(m_handle, &event);
		}
	}
	m_events.clear();
	m_cond_var.SignalAll();
}

int KernelEqueuePrivate::GetTriggeredEvents(KernelEvent* ev, int num, TraceWait* trace) {
	Common::LockGuard lock(m_mutex);

	EXIT_IF(num < 1);
	if (m_closed) {
		return KERNEL_ERROR_EBADF;
	}
	if (trace != nullptr && trace->id != 0) m_trace_observed = true;
	const bool trace_active = m_trace_observed && Trace::Enabled();

	TriggerExpiredTimers(MonotonicTimeNs());

	int ret = 0;

	for (auto it = m_events.begin(); it != m_events.end();) {
		auto& event = *it;
		bool  erase = false;
		while (event.triggered) {
			if (trace_active) {
				EnsureTraceRegistration(m_handle, event, true);
				event.trace_state.Check(event.triggered, event.pending_events.size());
			}
			if (trace_active && trace != nullptr && trace->id != 0) {
				const auto occurrence = event.trace_state.CurrentOccurrence();
				auto record = QueueTraceEvent(Trace::Kind::Delivery, m_handle, event);
				record.id = occurrence.first;
				record.parent_id = trace->id;
				record.tid = trace->tid;
				record.index = static_cast<uint32_t>(ret);
				record.count = static_cast<uint32_t>(std::min<uint64_t>(occurrence.count, UINT32_MAX));
				record.extra = occurrence.last;
				record.mode = occurrence.flags; // Preserve all local unknown reasons.
				record.flags |= TraceFlags(occurrence.flags);
				if (occurrence.count > UINT32_MAX) {
					record.flags |= Trace::Unknown | Trace::Overflow;
					record.mode |= TraceState::Unknown | TraceState::Overflow;
				}
				if (!trace->waited) record.flags |= Trace::AlreadyReady;
				record.begin_ns = Trace::NowNs();
				ev[ret++] = event.event;
				record.time_ns = Trace::NowNs();
				Trace::Record(record);
			} else {
				ev[ret++] = event.event;
			}
			if ((event.event.flags & EV_ONESHOT) != 0) {
				erase = true;
				break;
			}

			if (event.filter.reset_func != nullptr) {
				event.filter.reset_func(&event);
			} else if ((event.event.flags & EV_CLEAR) != 0) {
				event.triggered    = false;
				event.event.fflags = 0;
				event.event.data   = 0;
			}

			bool popped_pending = false;
			if (!event.pending_events.empty()) {
				event.event = event.pending_events.front();
				event.pending_events.pop_front();
				event.triggered = true;
				popped_pending = true;
			}
			if (trace_active) {
				event.trace_state.AfterDelivery(event.triggered, event.pending_events.size(),
				                                popped_pending);
			}

			if (ret >= num) {
				break;
			}
		}
		it = (erase ? m_events.erase(it) : std::next(it));
		if (ret >= num) {
			break;
		}
	}

	return ret;
}

void KernelEqueuePrivate::TriggerExpiredTimers(uint64_t now_ns) {
	for (auto& event: m_events) {
		if (event.deadline_ns != 0 && event.deadline_ns <= now_ns) {
			const bool trace_active = m_trace_observed && Trace::Enabled();
			const auto deadline = event.deadline_ns;
			const bool was_triggered = event.triggered;
			const auto pending = event.pending_events.size();
			if (trace_active) EnsureTraceRegistration(m_handle, event, true);
			uint64_t expirations = was_triggered ? 0 : 1;
			if (event.event.filter == KERNEL_EVFILT_TIMER) {
				const auto count = event.interval_ns == 0 ? !event.triggered :
				    1 + (now_ns - event.deadline_ns) / event.interval_ns;
				event.event.data += static_cast<intptr_t>(count);
				event.deadline_ns += count * event.interval_ns;
				expirations = count;
			}
			event.triggered = true;
			if (trace_active && expirations != 0) {
				const auto token = Trace::NewIdentity();
				event.trace_state.ObserveTrigger(token, was_triggered, pending,
				                                event.triggered, event.pending_events.size(),
				                                TraceState::Semantics::Coalesced);
				auto record = QueueTraceEvent(Trace::Kind::Trigger, m_handle, event);
				record.id = token;
				record.begin_ns = now_ns;
				record.time_ns = Trace::NowNs();
				record.extra = deadline; // Logical deadline, not a notifying-thread timestamp.
				record.count = static_cast<uint32_t>(std::min<uint64_t>(expirations, UINT32_MAX));
				record.mode = event.trace_state.CurrentOccurrence().flags;
				record.flags |= Trace::Timer | TraceFlags(record.mode);
				if (expirations > UINT32_MAX) {
					record.flags |= Trace::Unknown | Trace::Overflow;
					record.mode |= TraceState::Unknown | TraceState::Overflow;
				}
				Trace::Record(record);
			}
		}
	}
}

bool KernelEqueuePrivate::GetNextTimerWaitMicros(uint64_t now_ns, uint32_t* wait_micros) const {
	EXIT_IF(wait_micros == nullptr);

	uint64_t nearest_deadline = UINT64_MAX;
	for (const auto& event: m_events) {
		if (!event.triggered && event.deadline_ns != 0) {
			nearest_deadline = std::min(nearest_deadline, event.deadline_ns);
		}
	}
	if (nearest_deadline == UINT64_MAX) {
		return false;
	}

	const auto remaining_ns = nearest_deadline > now_ns ? nearest_deadline - now_ns : 0;
	const auto rounded_us   = std::max<uint64_t>(1, (remaining_ns + 999u) / 1000u);
	*wait_micros            = static_cast<uint32_t>(std::min<uint64_t>(rounded_us, UINT32_MAX));
	return true;
}

int KernelEqueuePrivate::WaitForEvents(KernelEvent* ev, int num, uint32_t micros, TraceWait* trace) {
	Common::LockGuard lock(m_mutex);

	EXIT_IF(num < 1);
	if (m_closed) {
		return KERNEL_ERROR_EBADF;
	}

	uint32_t      elapsed = 0;
	Common::Timer t;
	t.Start();

	for (;;) {
		int ret = GetTriggeredEvents(ev, num, trace);

		if (ret != 0 || (elapsed >= micros && micros != 0)) {
			return ret;
		}

		uint32_t   timer_wait = 0;
		const bool has_timer  = GetNextTimerWaitMicros(MonotonicTimeNs(), &timer_wait);
		if (trace != nullptr && trace->id != 0) trace->waited = true;
		if (micros == 0 && !has_timer) {
			m_cond_var.Wait(&m_mutex);
		} else {
			const auto external_wait = micros != 0 ? micros - elapsed : UINT32_MAX;
			m_cond_var.WaitFor(&m_mutex,
			                   has_timer ? std::min(external_wait, timer_wait) : external_wait);
		}

		elapsed = static_cast<uint32_t>(t.GetTimeS() * 1000000.0);
	}

	return 0;
}

int KernelEqueuePrivate::AddEvent(const KernelEqueueEvent& event) {
	Common::LockGuard lock(m_mutex);

	if (m_closed) {
		return KERNEL_ERROR_EBADF;
	}
	auto it = std::find_if(m_events.begin(), m_events.end(),
	                       [ident = event.event.ident, filter = event.event.filter](const auto& e) {
		                       return e.event.ident == ident && e.event.filter == filter;
	                       });
	if (it != m_events.end()) {
		TriggerExpiredTimers(MonotonicTimeNs());
		it->deadline_ns = event.deadline_ns;
		it->interval_ns = event.interval_ns;
		it->event.udata = event.event.udata;
		for (auto& pending: it->pending_events) {
			pending.udata = event.event.udata;
		}
		if (m_trace_observed && Trace::Enabled()) {
			EnsureTraceRegistration(m_handle, *it, true);
			RecordRegistration(m_handle, *it, 3);
		}
	} else {
		m_events.push_back(event);
		auto& added = m_events.back();
		added.trace_state.Initialize(0, false, 0, false);
		if (m_trace_observed && Trace::Enabled()) {
			// A new registration must not inherit the source object's sidecar identity.
			added.trace_state.Initialize(Trace::NewIdentity(), added.triggered,
			                             added.pending_events.size(), false);
			RecordRegistration(m_handle, added, 1);
		}
	}

	m_cond_var.Signal();
	return OK;
}

int KernelEqueuePrivate::TriggerEvent(uintptr_t ident, int16_t filter, void* trigger_data,
                                    const Trace::CallbackContext* trace) {
	Common::LockGuard lock(m_mutex);

	if (m_closed) {
		RecordTriggerFailure(m_handle, ident, filter, trace, KERNEL_ERROR_EBADF, m_trace_observed);
		return KERNEL_ERROR_EBADF;
	}
	auto it = std::find_if(m_events.begin(), m_events.end(), [ident, filter](const auto& e) {
		return e.event.ident == ident && e.event.filter == filter;
	});
	if (it != m_events.end()) {
		auto& event = *it;
		const bool trace_active = m_trace_observed && Trace::Enabled();
		bool was_triggered = false;
		size_t pending_before = 0;
		uint64_t token = 0, begin = 0;
		if (trace_active) {
			was_triggered = event.triggered;
			pending_before = event.pending_events.size();
			EnsureTraceRegistration(m_handle, event, true);
			token = Trace::NewIdentity();
			begin = Trace::NowNs();
		}

		if (event.filter.trigger_func != nullptr) {
			event.filter.trigger_func(&event, trigger_data);
		} else {
			event.triggered = true;
		}
		if (trace_active) {
			const auto end = Trace::NowNs();
			const auto pending_after = event.pending_events.size();
			const auto semantics = filter == KERNEL_EVFILT_GRAPHICS || filter == KERNEL_EVFILT_AMPR ||
			                               filter == KERNEL_EVFILT_VIDEO_OUT
			                           ? TraceState::Semantics::Queued
			                           : filter == KERNEL_EVFILT_USER
			                                 ? TraceState::Semantics::Coalesced
			                                 : TraceState::Semantics::Unknown;
			event.trace_state.ObserveTrigger(token, was_triggered, pending_before,
			                                event.triggered, pending_after, semantics);
			auto record = QueueTraceEvent(Trace::Kind::Trigger, m_handle, event);
			// A queued callback leaves current event intact; its newly appended payload
			// is the occurrence this invocation actually produced.
			if (pending_after > pending_before) {
				const auto& payload = event.pending_events.back();
				record.filter = payload.filter;
				record.ident = payload.ident;
				record.data = static_cast<uint64_t>(payload.data);
				record.fflags = payload.fflags;
				record.event_flags = payload.flags;
			}
			record.id = token;
			record.begin_ns = begin;
			record.time_ns = end;
			record.count = 1;
			record.mode = event.trace_state.CurrentOccurrence().flags & ~TraceState::Repeated;
			if (semantics == TraceState::Semantics::Queued &&
			    (record.mode & TraceState::Unknown) == 0) {
				// The new queued occurrence is independent of any coalesced/repeated
				// metadata on the still-current head.
				record.mode = 0;
			}
			record.flags |= TraceFlags(record.mode);
			if (trace == nullptr) {
				record.flags |= Trace::Direct;
			} else if (trace->id == 0) {
				// A callback captured before arming remains unknown; never recapture TLS.
				record.flags |= Trace::Unknown | Trace::PreArm;
			} else {
				record.parent_id = trace->id;
				record.scheduler_id = trace->scheduler_id;
				record.tick = trace->tick;
				record.submission_id = trace->submission_id;
				record.request_id = trace->request_id;
			}
			Trace::Record(record);
		}

		m_cond_var.Signal();

		return OK;
	}

	RecordTriggerFailure(m_handle, ident, filter, trace, KERNEL_ERROR_ENOENT, m_trace_observed);
	return KERNEL_ERROR_ENOENT;
}

static void UserEventTriggerFunc(KernelEqueueEvent* event, void* trigger_data) {
	EXIT_IF(event == nullptr);
	event->triggered   = true;
	event->event.data  = reinterpret_cast<intptr_t>(trigger_data);
	event->event.udata = trigger_data;
}

static void AmprEventTriggerFunc(KernelEqueueEvent* event, void* trigger_data) {
	EXIT_IF(event == nullptr);
	auto triggered_event = event->event;
	triggered_event.data = static_cast<intptr_t>(reinterpret_cast<uintptr_t>(trigger_data));
	if (event->triggered) {
		event->pending_events.push_back(triggered_event);
	} else {
		event->event     = triggered_event;
		event->triggered = true;
	}
}

static void UserEventResetFunc(KernelEqueueEvent* event) {
	EXIT_IF(event == nullptr);
	if ((event->event.flags & EV_CLEAR) != 0) {
		event->triggered    = false;
		event->event.fflags = 0;
		event->event.data   = 0;
	}
}

int KernelEqueuePrivate::DeleteEvent(uintptr_t ident, int16_t filter) {
	Common::LockGuard lock(m_mutex);

	if (m_closed) {
		return KERNEL_ERROR_EBADF;
	}
	auto it = std::find_if(m_events.begin(), m_events.end(), [ident, filter](const auto& e) {
		return e.event.ident == ident && e.event.filter == filter;
	});
	if (it != m_events.end()) {
		auto& event = *it;
		if (m_trace_observed && Trace::Enabled()) {
			EnsureTraceRegistration(m_handle, event, true);
			RecordRegistration(m_handle, event, 4);
		}

		if (event.filter.delete_event_func != nullptr) {
			auto owner = event.filter.owner;
			event.filter.delete_event_func(m_handle, &event);
		}

		m_events.erase(it);

		return OK;
	}

	return KERNEL_ERROR_ENOENT;
}

KernelEqueueRef KernelPinEqueue(KernelEqueue eq) {
	if (eq == KERNEL_EQUEUE_INVALID) {
		return {};
	}

	Common::LockGuard lock(g_equeues_mutex);
	const auto        entry = g_equeues.find(eq);
	return entry != g_equeues.end() ? entry->second : KernelEqueueRef {};
}

int KYTY_SYSV_ABI KernelCreateEqueue(KernelEqueue* eq, const char* name) {
	PRINT_NAME();

	if (eq == nullptr || name == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	{
		Common::LockGuard lock(g_equeues_mutex);
		if (g_next_equeue > static_cast<uint64_t>(std::numeric_limits<KernelEqueue>::max())) {
			EXIT("event queue handle space exhausted\n");
		}
		*eq        = static_cast<KernelEqueue>(g_next_equeue++);
		auto owner = std::make_shared<KernelEqueuePrivate>(*eq);
		owner->SetName(std::string(name));
		g_equeues.emplace(*eq, std::move(owner));
	}

	LOGF("\tEqueue create: %s\n", name);

	return OK;
}

int KYTY_SYSV_ABI KernelAddEvent(KernelEqueue eq, const KernelEqueueEvent& event) {
	auto owner = KernelPinEqueue(eq);
	if (!owner) {
		return KERNEL_ERROR_EBADF;
	}

	return owner->AddEvent(event);
}

int KYTY_SYSV_ABI KernelTriggerEvent(KernelEqueue eq, uintptr_t ident, int16_t filter,
                                     void* trigger_data) {
	return KernelTriggerEventTraced(eq, ident, filter, trigger_data, nullptr);
}

int KernelTriggerEventTraced(KernelEqueue eq, uintptr_t ident, int16_t filter,
                            void* trigger_data, const Trace::CallbackContext* trace) {
	auto owner = KernelPinEqueue(eq);
	if (!owner) {
		RecordTriggerFailure(eq, ident, filter, trace, KERNEL_ERROR_EBADF, false);
		return KERNEL_ERROR_EBADF;
	}

	return owner->TriggerEvent(ident, filter, trigger_data, trace);
}

int KYTY_SYSV_ABI KernelDeleteEvent(KernelEqueue eq, uintptr_t ident, int16_t filter) {
	auto owner = KernelPinEqueue(eq);
	if (!owner) {
		return KERNEL_ERROR_EBADF;
	}

	return owner->DeleteEvent(ident, filter);
}

int KYTY_SYSV_ABI KernelDeleteEqueue(KernelEqueue eq) {
	PRINT_NAME();

	KernelEqueueRef owner;

	{
		Common::LockGuard lock(g_equeues_mutex);
		const auto        entry = g_equeues.find(eq);
		if (entry == g_equeues.end()) {
			return KERNEL_ERROR_EBADF;
		}
		owner = std::move(entry->second);
		g_equeues.erase(entry);
	}

	LOGF("\tEqueue delete: %s\n", owner->GetName().c_str());
	owner->Close();

	return OK;
}

int KYTY_SYSV_ABI KernelWaitEqueue(KernelEqueue eq, KernelEvent* ev, int num, int* out,
                                   const KernelUseconds* timo) {
	PRINT_NAME();

	auto owner = KernelPinEqueue(eq);
	if (!owner) {
		return KERNEL_ERROR_EBADF;
	}

	if (ev == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	if (num < 1) {
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(out == nullptr);

	const bool quiet = owner->consecutive_timeouts.load(std::memory_order_relaxed) >=
	                   KernelEqueuePrivate::LOGGED_TIMEOUTS;
	if (!quiet) {
		LOGF("\tEqueue wait: %s, caller = 0x%016" PRIx64 ", eq = 0x%016" PRIx64
		     ", ev = 0x%016" PRIx64 ", num = %d, timo = %s, thread_id = %d\n",
		     owner->GetName().c_str(), reinterpret_cast<uint64_t>(__builtin_return_address(0)),
		     static_cast<uint64_t>(eq), reinterpret_cast<uint64_t>(ev), num,
		     (timo == nullptr ? "inf" : fmt::format("{}", *timo).c_str()),
		     Common::Thread::GetThreadIdUnique());
	}

	// KYTY_TIMELINE / F6: when the game waits for an event (GPU events and flips among them) and
	// what woke it (the first event's ident and filter).
	Libs::Graphics::Timeline::Mark("eq-wait", static_cast<uint64_t>(eq),
	                               timo == nullptr ? UINT64_MAX : static_cast<uint64_t>(*timo));
	TraceWait trace_wait {};
	int delivered = 0;
	if (timo == nullptr) {
		trace_wait = BeginTraceWait(eq, num, 0, UINT64_MAX);
		*out = delivered = owner->WaitForEvents(ev, num, 0, &trace_wait);
	}

	if (timo != nullptr) {
		if (*timo == 0) {
			trace_wait = BeginTraceWait(eq, num, 1, 0);
			*out = delivered = owner->GetTriggeredEvents(ev, num, &trace_wait);
		} else {
			const auto micros = *timo; // The existing timed-call read; no diagnostic guest read.
			trace_wait = BeginTraceWait(eq, num, 2, micros);
			*out = delivered = owner->WaitForEvents(ev, num, micros, &trace_wait);
		}
	}
	EndTraceWait(trace_wait, delivered);
	Libs::Graphics::Timeline::Mark(
	    "eq-woke", *out > 0 ? static_cast<uint64_t>(ev[0].ident) : 0,
	    *out > 0 ? static_cast<uint64_t>(static_cast<uint32_t>(ev[0].filter)) : 0);

	if (*out == KERNEL_ERROR_EBADF) {
		return KERNEL_ERROR_EBADF;
	}
	if (*out == 0) {
		const auto timeouts =
		    owner->consecutive_timeouts.fetch_add(1, std::memory_order_relaxed) + 1;
		if (timeouts <= KernelEqueuePrivate::LOGGED_TIMEOUTS) {
			LOGF("\tEqueue wait timedout: %s%s\n", owner->GetName().c_str(),
			     timeouts == KernelEqueuePrivate::LOGGED_TIMEOUTS
			         ? " (further timeouts are counted, not logged)"
			         : "");
		}
		return KERNEL_ERROR_ETIMEDOUT;
	}

	if (const auto timeouts = owner->consecutive_timeouts.exchange(0, std::memory_order_relaxed);
	    timeouts > KernelEqueuePrivate::LOGGED_TIMEOUTS) {
		LOGF("\tEqueue wait: %s, %u timed-out waits were not logged\n", owner->GetName().c_str(),
		     timeouts - KernelEqueuePrivate::LOGGED_TIMEOUTS);
	}
	LOGF("\tEqueue wait received %u events: ident = 0x%016" PRIx64
	     ", filter = %d, flags = 0x%04" PRIx16 ", fflags = 0x%08" PRIx32 ", data = 0x%016" PRIx64
	     ", udata = 0x%016" PRIx64 "\n",
	     *out, static_cast<uint64_t>(ev[0].ident), ev[0].filter, ev[0].flags, ev[0].fflags,
	     static_cast<uint64_t>(ev[0].data), reinterpret_cast<uint64_t>(ev[0].udata));

	return OK;
}

int KYTY_SYSV_ABI KernelAddUserEvent(KernelEqueue eq, int id) {
	PRINT_NAME();

	LOGF("\t user event add: eq = 0x%016" PRIx64 ", id = %d\n", static_cast<uint64_t>(eq), id);

	KernelEqueueEvent event {};
	event.event.ident         = static_cast<uintptr_t>(id);
	event.event.filter        = KERNEL_EVFILT_USER;
	event.event.flags         = EV_ADD;
	event.event.fflags        = 0;
	event.event.data          = 0;
	event.event.udata         = nullptr;
	event.filter.trigger_func = UserEventTriggerFunc;
	event.filter.reset_func   = UserEventResetFunc;

	return KernelAddEvent(eq, event);
}

int KYTY_SYSV_ABI KernelAddUserEventEdge(KernelEqueue eq, int id) {
	PRINT_NAME();

	LOGF("\t user event edge add: eq = 0x%016" PRIx64 ", id = %d\n", static_cast<uint64_t>(eq), id);

	KernelEqueueEvent event {};
	event.event.ident         = static_cast<uintptr_t>(id);
	event.event.filter        = KERNEL_EVFILT_USER;
	event.event.flags         = EV_ADD | EV_CLEAR;
	event.event.fflags        = 0;
	event.event.data          = 0;
	event.event.udata         = nullptr;
	event.filter.trigger_func = UserEventTriggerFunc;
	event.filter.reset_func   = UserEventResetFunc;

	return KernelAddEvent(eq, event);
}

int KYTY_SYSV_ABI KernelTriggerUserEvent(KernelEqueue eq, int id, void* udata) {
	PRINT_NAME();

	LOGF("\t user event trigger: eq = 0x%016" PRIx64 ", id = %d, udata = 0x%016" PRIx64 "\n",
	     static_cast<uint64_t>(eq), id, reinterpret_cast<uint64_t>(udata));

	return KernelTriggerEvent(eq, static_cast<uintptr_t>(id), KERNEL_EVFILT_USER, udata);
}

int KYTY_SYSV_ABI KernelTriggerUserEventForAll(int id, void* udata) {
	int                          triggered = 0;
	std::vector<KernelEqueueRef> queues;

	{
		Common::LockGuard lock(g_equeues_mutex);
		queues.reserve(g_equeues.size());
		for (const auto& [handle, queue]: g_equeues) {
			queues.push_back(queue);
		}
	}
	for (const auto& eq: queues) {
		if (eq->TriggerEvent(static_cast<uintptr_t>(id), KERNEL_EVFILT_USER, udata) == OK) {
			triggered++;
		}
	}

	return (triggered > 0 ? OK : KERNEL_ERROR_ENOENT);
}

int KYTY_SYSV_ABI KernelDeleteUserEvent(KernelEqueue eq, int id) {
	PRINT_NAME();

	LOGF("\t user event delete: eq = 0x%016" PRIx64 ", id = %d\n", static_cast<uint64_t>(eq), id);

	return KernelDeleteEvent(eq, static_cast<uintptr_t>(id), KERNEL_EVFILT_USER);
}

static int AddTimerEvent(KernelEqueue eq, int id, uint64_t delay_ns, bool periodic, void* udata) {
	const auto now_ns = MonotonicTimeNs();
	KernelEqueueEvent event {};
	event.deadline_ns  = delay_ns <= UINT64_MAX - now_ns ? now_ns + delay_ns : UINT64_MAX;
	event.interval_ns  = periodic ? delay_ns : 0;
	event.event.ident  = static_cast<uintptr_t>(id);
	event.event.filter = periodic ? KERNEL_EVFILT_TIMER : KERNEL_EVFILT_HRTIMER;
	event.event.flags  = EV_ADD | (periodic ? EV_CLEAR : EV_ONESHOT);
	event.event.udata  = udata;
	return KernelAddEvent(eq, event);
}

int KYTY_SYSV_ABI KernelAddTimerEvent(KernelEqueue eq, int id, KernelUseconds usec, void* udata) {
	return AddTimerEvent(eq, id, static_cast<uint64_t>(usec) * 1000, true, udata);
}

int KYTY_SYSV_ABI KernelDeleteTimerEvent(KernelEqueue eq, int id) {
	return KernelDeleteEvent(eq, static_cast<uintptr_t>(id), KERNEL_EVFILT_TIMER);
}

int KYTY_SYSV_ABI KernelAddHRTimerEvent(KernelEqueue eq, int id, const KernelTimespec* ts,
                                        void* udata) {
	if (ts == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}
	if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000ll ||
	    static_cast<uint64_t>(ts->tv_sec) >
	        (UINT64_MAX - static_cast<uint64_t>(ts->tv_nsec)) / 1000000000ull) {
		return KERNEL_ERROR_EINVAL;
	}

	const auto delay_ns =
	    static_cast<uint64_t>(ts->tv_sec) * 1000000000ull + static_cast<uint64_t>(ts->tv_nsec);
	return AddTimerEvent(eq, id, delay_ns, false, udata);
}

int KYTY_SYSV_ABI KernelDeleteHRTimerEvent(KernelEqueue eq, int id) {
	return KernelDeleteEvent(eq, static_cast<uintptr_t>(id), KERNEL_EVFILT_HRTIMER);
}

int KYTY_SYSV_ABI KernelAddAmprEvent(KernelEqueue eq, int id, void* udata) {
	PRINT_NAME();

	LOGF("\t AMPR event add: eq = 0x%016" PRIx64 ", id = %d, udata = 0x%016" PRIx64 "\n",
	     static_cast<uint64_t>(eq), id, reinterpret_cast<uint64_t>(udata));

	if (eq != KERNEL_EQUEUE_INVALID) {
		KernelEqueueEvent event {};
		event.event.ident         = static_cast<uintptr_t>(id);
		event.event.filter        = KERNEL_EVFILT_AMPR;
		event.event.flags         = EV_ADD | EV_CLEAR;
		event.event.fflags        = 0;
		event.event.data          = 0;
		event.event.udata         = udata;
		event.filter.trigger_func = AmprEventTriggerFunc;
		event.filter.reset_func   = UserEventResetFunc;
		(void)KernelAddEvent(eq, event);
	}

	return OK;
}

int KYTY_SYSV_ABI KernelAddAmprSystemEvent(KernelEqueue eq, int id, void* udata) {
	PRINT_NAME();

	LOGF("\t AMPR system event add: eq = 0x%016" PRIx64 ", id = %d, udata = 0x%016" PRIx64 "\n",
	     static_cast<uint64_t>(eq), id, reinterpret_cast<uint64_t>(udata));

	return KernelAddAmprEvent(eq, id, udata);
}

int KYTY_SYSV_ABI KernelDeleteAmprEvent(KernelEqueue eq, int id) {
	PRINT_NAME();

	LOGF("\t AMPR event delete: eq = 0x%016" PRIx64 ", id = %d\n", static_cast<uint64_t>(eq), id);

	if (eq != KERNEL_EQUEUE_INVALID) {
		(void)KernelDeleteEvent(eq, static_cast<uintptr_t>(id), KERNEL_EVFILT_AMPR);
	}

	return OK;
}

int KYTY_SYSV_ABI KernelDeleteAmprSystemEvent(KernelEqueue eq, int id) {
	PRINT_NAME();

	LOGF("\t AMPR system event delete: eq = 0x%016" PRIx64 ", id = %d\n", static_cast<uint64_t>(eq),
	     id);

	return KernelDeleteAmprEvent(eq, id);
}

intptr_t KYTY_SYSV_ABI KernelGetEventData(const KernelEvent* ev) {
	PRINT_NAME();

	if (ev != nullptr) {
		return ev->data;
	}

	return 0;
}

intptr_t KYTY_SYSV_ABI KernelGetEventFflags(const KernelEvent* ev) {
	PRINT_NAME();

	if (ev != nullptr) {
		return ev->fflags;
	}

	return 0;
}

int KYTY_SYSV_ABI KernelGetEventFilter(const KernelEvent* ev) {
	PRINT_NAME();

	if (ev != nullptr) {
		return ev->filter;
	}

	return 0;
}

uintptr_t KYTY_SYSV_ABI KernelGetEventId(const KernelEvent* ev) {
	PRINT_NAME();

	if (ev != nullptr) {
		return ev->ident;
	}

	return 0;
}

void* KYTY_SYSV_ABI KernelGetEventUserData(const KernelEvent* ev) {
	PRINT_NAME();

	if (ev != nullptr) {
		return ev->udata;
	}

	return nullptr;
}

int KYTY_SYSV_ABI KernelGetEventError(const KernelEvent* ev) {
	PRINT_NAME();

	if (ev != nullptr && (ev->flags & EV_ERROR) != 0) {
		return static_cast<int>(ev->data);
	}

	return 0;
}

} // namespace Libs::LibKernel::EventQueue
