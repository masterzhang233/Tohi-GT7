#include "kernel/eventQueue.h"
#include "common/eventTrace.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

namespace EventQueue = Libs::LibKernel::EventQueue;
using Libs::LibKernel::KERNEL_ERROR_EBADF;
using Libs::LibKernel::KERNEL_ERROR_ENOENT;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "EventQueueLifetimeTests: failed: %s\n", text);
		std::abort();
	}
}

void CheckConcurrentResult(int result, const char* text) {
	Check(result == OK || result == KERNEL_ERROR_EBADF || result == KERNEL_ERROR_ENOENT, text);
}

void TestPeriodicTimerEvents() {
	using namespace std::chrono_literals;
	using Libs::LibKernel::KERNEL_ERROR_ETIMEDOUT;
	Check(EventQueue::KernelAddTimerEvent(EventQueue::KERNEL_EQUEUE_INVALID, 10, 1000, nullptr) ==
	          KERNEL_ERROR_EBADF,
	      "timer rejects invalid queue");
	Check(EventQueue::KernelDeleteTimerEvent(EventQueue::KERNEL_EQUEUE_INVALID, 10) ==
	          KERNEL_ERROR_EBADF,
	      "timer deletion rejects invalid queue");
	EventQueue::KernelEqueue queue = EventQueue::KERNEL_EQUEUE_INVALID;
	Check(EventQueue::KernelCreateEqueue(&queue, "periodic-timer") == OK, "create timer queue");
	auto* user_data = reinterpret_cast<void*>(0x1234);
	Check(EventQueue::KernelAddTimerEvent(queue, 10, 1000, user_data) == OK, "add periodic timer");
	std::this_thread::sleep_for(5ms);
	EventQueue::KernelEvent events[2] {};
	int out = 0;
	Libs::LibKernel::KernelUseconds timeout = 0;
	Check(EventQueue::KernelWaitEqueue(queue, events, 2, &out, &timeout) == OK && out == 1,
	      "periodic occurrences are aggregated into one event");
	Check(events[0].ident == 10 && events[0].filter == EventQueue::KERNEL_EVFILT_TIMER &&
	          events[0].data >= 5 && events[0].udata == user_data,
	      "timer reports identity, userdata and elapsed occurrence count");
	timeout = 100000;
	Check(EventQueue::KernelWaitEqueue(queue, events, 2, &out, &timeout) == OK && out == 1 &&
	          events[0].data >= 1,
	      "periodic timer remains registered after delivery");

	std::this_thread::sleep_for(5ms);
	user_data = reinterpret_cast<void*>(0x5678);
	Check(EventQueue::KernelAddTimerEvent(queue, 10, 3000000000u, user_data) == OK,
	      "update periodic timer interval");
	timeout = 0;
	Check(EventQueue::KernelWaitEqueue(queue, events, 2, &out, &timeout) == OK && out == 1 &&
	          events[0].data >= 5 && events[0].udata == user_data,
	      "timer update preserves accumulated occurrences and updates userdata");
	Check(EventQueue::KernelWaitEqueue(queue, events, 2, &out, &timeout) == KERNEL_ERROR_ETIMEDOUT,
	      "timer update replaces the repeating interval");
	Check(EventQueue::KernelDeleteTimerEvent(queue, 10) == OK, "delete periodic timer");
	Check(EventQueue::KernelAddTimerEvent(queue, 10, 1000, user_data) == OK,
	      "add periodic timer for deletion before next occurrence");
	Check(EventQueue::KernelDeleteTimerEvent(queue, 10) == OK, "delete pending periodic timer");
	timeout = 5000;
	Check(EventQueue::KernelWaitEqueue(queue, events, 2, &out, &timeout) == KERNEL_ERROR_ETIMEDOUT,
	      "deleted periodic timer does not fire after its interval");
	timeout = 0;

	Check(EventQueue::KernelAddTimerEvent(queue, 10, 0, user_data) == OK,
	      "add immediate periodic timer");
	for (int i = 0; i < 2; ++i) {
		Check(EventQueue::KernelAddTimerEvent(queue, 10, 0, user_data) == OK,
		      "update pending immediate timer");
	}
	for (int i = 0; i < 2; ++i) {
		Check(EventQueue::KernelWaitEqueue(queue, events, 2, &out, &timeout) == OK && out == 1 &&
		          events[0].data == 1,
		      "zero interval remains immediately ready after delivery");
	}
	Check(EventQueue::KernelDeleteTimerEvent(queue, 10) == OK, "delete immediate timer");
	Check(EventQueue::KernelWaitEqueue(queue, events, 2, &out, &timeout) == KERNEL_ERROR_ETIMEDOUT,
	      "deleted timer is not delivered again");
	Check(EventQueue::KernelDeleteTimerEvent(queue, 10) == KERNEL_ERROR_ENOENT,
	      "deleting a missing timer reports ENOENT");

	const Libs::LibKernel::KernelTimespec delay {0, 1000000};
	Check(EventQueue::KernelAddHRTimerEvent(queue, 10, &delay, user_data) == OK,
	      "add high resolution timer");
	Check(EventQueue::KernelDeleteTimerEvent(queue, 10) == KERNEL_ERROR_ENOENT,
	      "periodic timer deletion does not delete a high resolution timer with the same id");
	timeout = 100000;
	Check(EventQueue::KernelWaitEqueue(queue, events, 2, &out, &timeout) == OK && out == 1 &&
	          events[0].filter == EventQueue::KERNEL_EVFILT_HRTIMER && events[0].udata == user_data,
	      "shared timer scheduling delivers high resolution event");
	Check(EventQueue::KernelDeleteHRTimerEvent(queue, 10) == KERNEL_ERROR_ENOENT,
	      "high resolution timer is removed after one delivery");
	Check(EventQueue::KernelDeleteEqueue(queue) == OK, "delete timer queue");
	Check(EventQueue::KernelDeleteTimerEvent(queue, 10) == KERNEL_ERROR_EBADF,
	      "timer deletion rejects a deleted queue");
}

void CountDeletedEvent(EventQueue::KernelEqueue, EventQueue::KernelEqueueEvent* event) {
	auto* count = static_cast<std::atomic_uint32_t*>(event->filter.data);
	count->fetch_add(1, std::memory_order_relaxed);
}

struct DuplicateEventOwner {
	std::atomic_uint32_t delete_count {0};
};

void QueueDuplicateEvent(EventQueue::KernelEqueueEvent* event, void* trigger_data) {
	auto next = event->event;
	next.data = reinterpret_cast<intptr_t>(trigger_data);
	if (event->triggered) {
		event->pending_events.push_back(next);
	} else {
		event->event     = next;
		event->triggered = true;
	}
}

void ResetDuplicateEvent(EventQueue::KernelEqueueEvent* event) {
	event->triggered  = false;
	event->event.data = 0;
}

void DeleteDuplicateEvent(EventQueue::KernelEqueue, EventQueue::KernelEqueueEvent* event) {
	auto* owner = static_cast<DuplicateEventOwner*>(event->filter.data);
	owner->delete_count.fetch_add(1, std::memory_order_relaxed);
}

void PoisonDuplicateEvent(EventQueue::KernelEqueueEvent*, void*) {
	Check(false, "duplicate add replaced trigger callback");
}

void TestDuplicateAddPreservesEventState() {
	EventQueue::KernelEqueue queue = EventQueue::KERNEL_EQUEUE_INVALID;
	Check(EventQueue::KernelCreateEqueue(&queue, "duplicate-add") == OK,
	      "create duplicate add queue");

	auto                               original_owner = std::make_shared<DuplicateEventOwner>();
	std::weak_ptr<DuplicateEventOwner> weak_original  = original_owner;
	EventQueue::KernelEqueueEvent      original {};
	original.event.ident              = 17;
	original.event.filter             = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
	original.event.udata              = reinterpret_cast<void*>(0x1111);
	original.filter.data              = original_owner.get();
	original.filter.owner             = original_owner;
	original.filter.trigger_func      = QueueDuplicateEvent;
	original.filter.reset_func        = ResetDuplicateEvent;
	original.filter.delete_event_func = DeleteDuplicateEvent;
	Check(EventQueue::KernelAddEvent(queue, original) == OK, "add original duplicate event");
	Check(EventQueue::KernelTriggerEvent(queue, 17, EventQueue::KERNEL_EVFILT_VIDEO_OUT,
	                                     reinterpret_cast<void*>(0x1234)) == OK,
	      "queue first trigger");
	Check(EventQueue::KernelTriggerEvent(queue, 17, EventQueue::KERNEL_EVFILT_VIDEO_OUT,
	                                     reinterpret_cast<void*>(0x5678)) == OK,
	      "queue pending trigger");

	auto                               replacement_owner = std::make_shared<DuplicateEventOwner>();
	std::weak_ptr<DuplicateEventOwner> weak_replacement  = replacement_owner;
	EventQueue::KernelEqueueEvent      duplicate {};
	duplicate.triggered           = false;
	duplicate.deadline_ns         = 1;
	duplicate.event.ident         = 17;
	duplicate.event.filter        = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
	duplicate.event.data          = 0x7fffffff;
	duplicate.event.udata         = reinterpret_cast<void*>(0x2222);
	duplicate.filter.data         = replacement_owner.get();
	duplicate.filter.owner        = replacement_owner;
	duplicate.filter.trigger_func = PoisonDuplicateEvent;
	Check(EventQueue::KernelAddEvent(queue, duplicate) == OK, "update duplicate event");

	duplicate.filter.owner.reset();
	replacement_owner.reset();
	Check(weak_replacement.expired(), "duplicate owner is not retained");
	original.filter.owner.reset();
	original_owner.reset();
	Check(!weak_original.expired(), "original event owner remains retained");

	EventQueue::KernelEvent         events[2] {};
	int                             out     = 0;
	Libs::LibKernel::KernelUseconds timeout = 0;
	Check(EventQueue::KernelWaitEqueue(queue, events, 2, &out, &timeout) == OK,
	      "read queued duplicate triggers");
	Check(out == 2, "duplicate add preserves pending event count");
	Check(events[0].data == 0x1234 && events[1].data == 0x5678,
	      "duplicate add preserves current and pending event data");
	Check(events[0].udata == reinterpret_cast<void*>(0x2222),
	      "duplicate add updates current user data");
	Check(events[1].udata == reinterpret_cast<void*>(0x2222),
	      "duplicate add updates pending user data");

	EventQueue::KernelEvent timer_event {};
	Check(EventQueue::KernelWaitEqueue(queue, &timer_event, 1, &out, &timeout) == OK && out == 1,
	      "duplicate add updates deadline metadata");
	Check(timer_event.data == 0 && timer_event.udata == reinterpret_cast<void*>(0x2222),
	      "deadline trigger retains updated duplicate metadata");

	auto retained_owner = weak_original.lock();
	Check(retained_owner != nullptr, "original owner alive before delete");
	Check(EventQueue::KernelDeleteEvent(queue, 17, EventQueue::KERNEL_EVFILT_VIDEO_OUT) == OK,
	      "delete duplicate event");
	Check(retained_owner->delete_count.load(std::memory_order_relaxed) == 1,
	      "duplicate add preserves delete callback");
	retained_owner.reset();
	Check(weak_original.expired(), "original owner released on delete");
	Check(EventQueue::KernelDeleteEqueue(queue) == OK, "delete duplicate add queue");
}

struct SimulatedVideoOutEventState;

struct SimulatedVideoOutRegistration {
	EventQueue::KernelEqueue                     handle = EventQueue::KERNEL_EQUEUE_INVALID;
	std::shared_ptr<SimulatedVideoOutEventState> state;
	uint64_t                                     marker = 0x123456789abcdef0ull;
};

struct SimulatedVideoOutEventState {
	SimulatedVideoOutEventState(std::atomic_uint32_t& stage, std::atomic_uint32_t& destroy_count)
	    : stage(stage), destroy_count(destroy_count) {}

	~SimulatedVideoOutEventState() { destroy_count.fetch_add(1, std::memory_order_relaxed); }

	std::mutex                                                  mutex;
	std::vector<std::shared_ptr<SimulatedVideoOutRegistration>> queues;
	std::atomic_uint32_t&                                       stage;
	std::atomic_uint32_t&                                       destroy_count;
	uint64_t                                                    marker = 0xfedcba9876543210ull;
};

void DetachSimulatedVideoOutEvent(EventQueue::KernelEqueue       queue,
                                  EventQueue::KernelEqueueEvent* event) {
	auto* registration = static_cast<SimulatedVideoOutRegistration*>(event->filter.data);
	Check(registration != nullptr && registration->handle == queue,
	      "simulated registration identity");
	auto state = registration->state;
	Check(state != nullptr, "simulated event owns shared state");
	state->stage.store(1, std::memory_order_release);
	while (state->stage.load(std::memory_order_acquire) != 2) {
		std::this_thread::yield();
	}

	{
		std::lock_guard lock(state->mutex);
		const auto      entry = std::find_if(
		    state->queues.begin(), state->queues.end(),
		    [registration](const auto& candidate) { return candidate.get() == registration; });
		if (entry != state->queues.end()) {
			state->queues.erase(entry);
		}
	}
	event->filter.owner.reset();
	Check(state->marker == 0xfedcba9876543210ull && registration->marker == 0x123456789abcdef0ull,
	      "callback state survives simulated port destruction");
}

void TestCallbackStateOutlivesPort() {
	EventQueue::KernelEqueue queue = EventQueue::KERNEL_EQUEUE_INVALID;
	Check(EventQueue::KernelCreateEqueue(&queue, "shared-port-state") == OK,
	      "create shared port state queue");

	std::atomic_uint32_t stage {0};
	std::atomic_uint32_t destroy_count {0};
	auto port_state = std::make_shared<SimulatedVideoOutEventState>(stage, destroy_count);
	std::weak_ptr<SimulatedVideoOutEventState> weak_state = port_state;
	auto registration = std::make_shared<SimulatedVideoOutRegistration>();
	std::weak_ptr<SimulatedVideoOutRegistration> weak_registration = registration;
	registration->handle                                           = queue;
	registration->state                                            = port_state;
	port_state->queues.push_back(registration);

	EventQueue::KernelEqueueEvent event {};
	event.event.ident              = 8;
	event.event.filter             = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
	event.filter.data              = registration.get();
	event.filter.owner             = registration;
	event.filter.delete_event_func = DetachSimulatedVideoOutEvent;
	Check(EventQueue::KernelAddEvent(queue, event) == OK, "add shared port state event");
	event.filter.owner.reset();

	std::jthread close([&] {
		Check(EventQueue::KernelDeleteEqueue(queue) == OK, "delete shared port state queue");
	});
	while (stage.load(std::memory_order_acquire) != 1) {
		std::this_thread::yield();
	}

	std::vector<std::shared_ptr<SimulatedVideoOutRegistration>> detached;
	{
		std::lock_guard lock(port_state->mutex);
		detached = std::move(port_state->queues);
	}
	registration.reset();
	detached.clear();
	port_state.reset();
	Check(!weak_state.expired(), "callback state outlives simulated port object");
	Check(!weak_registration.expired(), "registration outlives simulated port object");

	stage.store(2, std::memory_order_release);
	close.join();
	Check(weak_registration.expired(), "detached registration is released");
	Check(weak_state.expired(), "detached event state is released");
	Check(destroy_count.load(std::memory_order_relaxed) == 1,
	      "shared event state is destroyed exactly once");
}

struct OwnedCallbackPayload {
	OwnedCallbackPayload(std::atomic_uint32_t& stage, std::atomic_uint32_t& delete_count,
	                     std::atomic_uint32_t& destroy_count)
	    : stage(stage), delete_count(delete_count), destroy_count(destroy_count) {}

	std::atomic_uint32_t& stage;
	std::atomic_uint32_t& delete_count;
	std::atomic_uint32_t& destroy_count;
	uint64_t              marker = 0xc0dec0dec0dec0deull;

	~OwnedCallbackPayload() { destroy_count.fetch_add(1, std::memory_order_relaxed); }
};

void DeleteOwnedEvent(EventQueue::KernelEqueue queue, EventQueue::KernelEqueueEvent* event) {
	auto* payload = static_cast<OwnedCallbackPayload*>(event->filter.data);
	Check(payload != nullptr, "owned callback payload");
	Check(!EventQueue::KernelPinEqueue(queue), "owned callback runs after registry removal");
	payload->delete_count.fetch_add(1, std::memory_order_relaxed);
	event->filter.owner.reset();
	payload->stage.store(1, std::memory_order_release);
	while (payload->stage.load(std::memory_order_acquire) != 2) {
		std::this_thread::yield();
	}
	Check(payload->marker == 0xc0dec0dec0dec0deull, "owned callback payload remains valid");
}

void TestCallbackOwnsPayload() {
	EventQueue::KernelEqueue queue = EventQueue::KERNEL_EQUEUE_INVALID;
	Check(EventQueue::KernelCreateEqueue(&queue, "owned-callback") == OK,
	      "create owned callback queue");

	std::atomic_uint32_t stage {0};
	std::atomic_uint32_t delete_count {0};
	std::atomic_uint32_t destroy_count {0};
	auto registration = std::make_shared<OwnedCallbackPayload>(stage, delete_count, destroy_count);
	std::weak_ptr<OwnedCallbackPayload>                weak_registration = registration;
	std::vector<std::shared_ptr<OwnedCallbackPayload>> port_registrations {registration};
	{
		EventQueue::KernelEqueueEvent event {};
		event.event.ident              = 2;
		event.event.filter             = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
		event.filter.data              = registration.get();
		event.filter.owner             = registration;
		event.filter.delete_event_func = DeleteOwnedEvent;
		Check(EventQueue::KernelAddEvent(queue, event) == OK, "add owned callback event");
	}

	std::jthread close(
	    [&] { Check(EventQueue::KernelDeleteEqueue(queue) == OK, "delete owned callback queue"); });
	while (stage.load(std::memory_order_acquire) != 1) {
		std::this_thread::yield();
	}

	Check(!EventQueue::KernelPinEqueue(queue),
	      "owned callback queue removed while callback blocked");
	port_registrations.clear();
	registration.reset();
	Check(!weak_registration.expired(), "delete callback retains detached payload");

	stage.store(2, std::memory_order_release);
	close.join();
	Check(delete_count.load(std::memory_order_relaxed) == 1, "owned callback runs exactly once");
	Check(weak_registration.expired(), "owned callback payload released with event");
	Check(destroy_count.load(std::memory_order_relaxed) == 1,
	      "owned callback payload destroyed exactly once");
}

void TestPinnedClose() {
	EventQueue::KernelEqueue queue = EventQueue::KERNEL_EQUEUE_INVALID;
	Check(EventQueue::KernelCreateEqueue(&queue, "pinned-close") == OK, "create pinned queue");

	std::atomic_uint32_t          delete_count {0};
	EventQueue::KernelEqueueEvent event {};
	event.event.ident              = 1;
	event.event.filter             = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
	event.filter.data              = &delete_count;
	event.filter.delete_event_func = CountDeletedEvent;
	Check(EventQueue::KernelAddEvent(queue, event) == OK, "add callback event");

	auto owner = EventQueue::KernelPinEqueue(queue);
	Check(owner != nullptr, "pin live queue");
	Check(EventQueue::KernelDeleteEqueue(queue) == OK, "delete pinned queue");
	Check(delete_count.load(std::memory_order_relaxed) == 1, "close invokes callback once");
	Check(!EventQueue::KernelPinEqueue(queue), "deleted queue leaves registry");
	Check(EventQueue::KernelTriggerEvent(queue, 1, EventQueue::KERNEL_EVFILT_VIDEO_OUT, nullptr) ==
	          KERNEL_ERROR_EBADF,
	      "stale trigger rejected");
	Check(EventQueue::KernelDeleteEqueue(queue) == KERNEL_ERROR_EBADF,
	      "second queue delete rejected");

	owner.reset();
	Check(delete_count.load(std::memory_order_relaxed) == 1,
	      "deferred destruction does not repeat callback");
}

void TestStaleHandleNeverAliasesNewQueue() {
	EventQueue::KernelEqueue stale = EventQueue::KERNEL_EQUEUE_INVALID;
	Check(EventQueue::KernelCreateEqueue(&stale, "stale-handle") == OK, "create stale queue");
	Check(EventQueue::KernelDeleteEqueue(stale) == OK, "delete stale queue");

	EventQueue::KernelEqueue replacement = EventQueue::KERNEL_EQUEUE_INVALID;
	Check(EventQueue::KernelCreateEqueue(&replacement, "replacement") == OK,
	      "create replacement queue");
	Check(stale != replacement, "queue handles are never recycled");
	Check(!EventQueue::KernelPinEqueue(stale), "stale handle does not pin");
	Check(EventQueue::KernelAddUserEvent(stale, 11) == KERNEL_ERROR_EBADF,
	      "stale handle cannot mutate replacement");
	Check(EventQueue::KernelAddUserEvent(replacement, 11) == OK,
	      "replacement handle remains valid");
	Check(EventQueue::KernelTriggerUserEvent(stale, 11, nullptr) == KERNEL_ERROR_EBADF,
	      "stale handle cannot trigger replacement");
	Check(EventQueue::KernelTriggerUserEvent(replacement, 11, nullptr) == OK,
	      "replacement event triggers");
	Check(EventQueue::KernelDeleteEqueue(replacement) == OK, "delete replacement queue");
}

void TestConcurrentCloseCallback() {
	for (uint32_t iteration = 0; iteration < 64; iteration++) {
		EventQueue::KernelEqueue queue = EventQueue::KERNEL_EQUEUE_INVALID;
		Check(EventQueue::KernelCreateEqueue(&queue, "callback-race") == OK,
		      "create callback race queue");

		std::atomic_uint32_t          delete_count {0};
		EventQueue::KernelEqueueEvent callback_event {};
		callback_event.event.ident              = 9;
		callback_event.event.filter             = EventQueue::KERNEL_EVFILT_GRAPHICS;
		callback_event.filter.data              = &delete_count;
		callback_event.filter.delete_event_func = CountDeletedEvent;
		Check(EventQueue::KernelAddEvent(queue, callback_event) == OK, "add callback race event");

		std::atomic_bool start {false};
		std::jthread     trigger([&] {
			while (!start.load(std::memory_order_acquire)) {
				std::this_thread::yield();
			}
			for (uint32_t i = 0; i < 256; i++) {
				CheckConcurrentResult(EventQueue::KernelTriggerEvent(
				                          queue, 9, EventQueue::KERNEL_EVFILT_GRAPHICS, nullptr),
				                      "callback race trigger result");
			}
		});

		start.store(true, std::memory_order_release);
		Check(EventQueue::KernelDeleteEqueue(queue) == OK, "callback race queue delete");
		trigger.join();
		Check(delete_count.load(std::memory_order_relaxed) == 1,
		      "concurrent close invokes callback exactly once");
	}
}

void TestConcurrentDelete() {
	for (uint32_t iteration = 0; iteration < 64; iteration++) {
		EventQueue::KernelEqueue queue = EventQueue::KERNEL_EQUEUE_INVALID;
		Check(EventQueue::KernelCreateEqueue(&queue, "concurrent-delete") == OK,
		      "create concurrent queue");
		EventQueue::KernelEqueueEvent event {};
		event.event.ident  = 7;
		event.event.filter = EventQueue::KERNEL_EVFILT_USER;
		Check(EventQueue::KernelAddEvent(queue, event) == OK, "add concurrent event");

		std::atomic_bool start {false};
		std::jthread     mutate([&] {
			while (!start.load(std::memory_order_acquire)) {
				std::this_thread::yield();
			}
			for (uint32_t i = 0; i < 64; i++) {
				CheckConcurrentResult(EventQueue::KernelAddEvent(queue, event),
				                      "concurrent add result");
				CheckConcurrentResult(EventQueue::KernelTriggerEvent(
				                          queue, 7, EventQueue::KERNEL_EVFILT_USER, nullptr),
				                      "concurrent trigger result");
				CheckConcurrentResult(
				    EventQueue::KernelDeleteEvent(queue, 7, EventQueue::KERNEL_EVFILT_USER),
				    "concurrent event delete result");
			}
		});
		std::jthread     trigger([&] {
			while (!start.load(std::memory_order_acquire)) {
				std::this_thread::yield();
			}
			for (uint32_t i = 0; i < 128; i++) {
				CheckConcurrentResult(EventQueue::KernelTriggerEvent(
				                          queue, 7, EventQueue::KERNEL_EVFILT_USER, nullptr),
				                      "parallel trigger result");
			}
		});

		start.store(true, std::memory_order_release);
		Check(EventQueue::KernelDeleteEqueue(queue) == OK, "concurrent queue delete");
		mutate.join();
		trigger.join();
		Check(!EventQueue::KernelPinEqueue(queue), "concurrent queue removed from registry");
	}
}

struct TraceResetProbe {
	std::array<EventQueue::TraceState::Occurrence, 64> occurrences {};
	std::array<uint64_t, 64> registrations {};
	size_t count = 0;
};

void CaptureTraceAndReset(EventQueue::KernelEqueueEvent* event) {
	auto& probe = *static_cast<TraceResetProbe*>(event->filter.data);
	Check(probe.count < probe.occurrences.size(), "fixture reset capture bounded");
	probe.occurrences[probe.count] = event->trace_state.CurrentOccurrence();
	probe.registrations[probe.count++] = event->trace_state.Registration();
	ResetDuplicateEvent(event);
}

void TestTraceIntegrationPreservesQueue() {
	namespace E = Common::EventTrace;
	namespace S = Common::SourceTrace;
	namespace F = EventQueue::TraceState;
	EventQueue::KernelEqueue queue = EventQueue::KERNEL_EQUEUE_INVALID;
	Check(EventQueue::KernelCreateEqueue(&queue, "trace-integration") == OK, "create trace queue");
	TraceResetProbe probe;
	EventQueue::KernelEqueueEvent event {};
	event.event.ident = 91;
	event.event.filter = EventQueue::KERNEL_EVFILT_GRAPHICS;
	event.filter.data = &probe;
	event.filter.trigger_func = QueueDuplicateEvent;
	event.filter.reset_func = CaptureTraceAndReset;
	Check(EventQueue::KernelAddEvent(queue, event) == OK, "register traced fixture event");
	Check(EventQueue::KernelTriggerEvent(queue, 91, event.event.filter,
	                                    reinterpret_cast<void*>(10)) == OK,
	      "unarmed trigger retains guest event");
	// Fixture startup only: earlier tests finished with tracing disabled and no
	// live traced scope. Production arming never resets this timestamp.
	S::Detail::g_arm_ns.store(0);
	S::Detail::g_enabled.store(true);
	S::Detail::g_chain_enabled.store(true);
	S::Detail::g_observed_threads.store(uint64_t {E::ThreadId()} << 32u);
	Check(S::TryArm(S::NowNs()), "fixture arms once after earlier lifetime tests finish");
#ifdef _WIN32
	Check(_putenv_s("KYTY_GT7_EVENT_TRACE", "1") == 0, "set fixture event trace option");
#else
	Check(setenv("KYTY_GT7_EVENT_TRACE", "1", 1) == 0, "set fixture event trace option");
#endif
	E::Initialize(true);
	Check(E::Enabled(), "actual event trace initialization enables fixture");
	EventQueue::KernelEvent returned[40] {};
	int out = 0;
	Libs::LibKernel::KernelUseconds timeout = 0;
	Check(EventQueue::KernelWaitEqueue(queue, returned, 1, &out, &timeout) == OK &&
	      out == 1 && returned[0].data == 10, "first observed wait preserves pre-arm item");
	Check((probe.occurrences[0].flags & (F::Unknown | F::PreArm)) == (F::Unknown | F::PreArm) &&
	      probe.occurrences[0].first == 0, "pre-arm pending item never gains a fabricated token");
	const auto registration = probe.registrations[0];
	Check(registration != 0, "first observed registration has a generation");
	const auto callback = E::CaptureCallback(501, 9, 91, 0, 0);
	for (intptr_t value: {11, 12}) {
		Check(EventQueue::KernelTriggerEventTraced(queue, 91, event.event.filter,
		                                          reinterpret_cast<void*>(value), &callback) == OK,
		      "trace context does not change queued trigger result");
	}
	event.event.udata = reinterpret_cast<void*>(0x4567);
	Check(EventQueue::KernelAddEvent(queue, event) == OK, "update traced registration");
	for (intptr_t value: {11, 12}) {
		Check(EventQueue::KernelWaitEqueue(queue, returned, 1, &out, &timeout) == OK &&
		      out == 1 && returned[0].data == value && returned[0].udata == event.event.udata,
		      "partial traced delivery preserves FIFO and duplicate-add userdata");
	}
	Check(probe.occurrences[1].flags == 0 && probe.occurrences[2].flags == 0 &&
	      probe.occurrences[1].first != 0 && probe.occurrences[2].first != 0 &&
	      probe.occurrences[1].first != probe.occurrences[2].first &&
	      probe.registrations[1] == registration && probe.registrations[2] == registration,
	      "duplicate add preserves generation and distinct queued occurrence identities");
	for (intptr_t value = 100; value < 134; ++value) {
		Check(EventQueue::KernelTriggerEvent(queue, 91, event.event.filter,
		                                    reinterpret_cast<void*>(value)) == OK,
		      "guest pending queue remains usable past diagnostic capacity");
	}
	Check(EventQueue::KernelWaitEqueue(queue, returned, 40, &out, &timeout) == OK && out == 34,
	      "diagnostic overflow does not drop guest occurrences");
	for (int i = 0; i < 34; ++i) {
		Check(returned[i].data == 100 + i && probe.occurrences[3 + i].first == 0 &&
		      (probe.occurrences[3 + i].flags & (F::Unknown | F::Overflow)) ==
		          (F::Unknown | F::Overflow), "overflow stays unknown through real FIFO drain");
	}
	Check(EventQueue::KernelTriggerEvent(queue, 91, event.event.filter,
	                                    reinterpret_cast<void*>(200)) == OK &&
	      EventQueue::KernelWaitEqueue(queue, returned, 1, &out, &timeout) == OK &&
	      returned[0].data == 200 && probe.occurrences[37].flags == 0 &&
	      probe.occurrences[37].first != 0, "empty queue restores exact occurrence tracking");
	Check(EventQueue::KernelDeleteEvent(queue, 91, event.event.filter) == OK &&
	      EventQueue::KernelAddEvent(queue, event) == OK &&
	      EventQueue::KernelTriggerEvent(queue, 91, event.event.filter,
	                                    reinterpret_cast<void*>(201)) == OK &&
	      EventQueue::KernelWaitEqueue(queue, returned, 1, &out, &timeout) == OK &&
	      probe.registrations[38] != registration && returned[0].data == 201,
	      "delete/re-add changes generation without changing delivery");
	Check(EventQueue::KernelTriggerEventTraced(queue, 999, event.event.filter, nullptr, &callback) ==
	          KERNEL_ERROR_ENOENT, "explicit traced missing registration preserves ENOENT");
	Check(EventQueue::KernelDeleteEqueue(queue) == OK &&
	      EventQueue::KernelTriggerEventTraced(queue, 91, event.event.filter, nullptr, &callback) ==
	          KERNEL_ERROR_EBADF, "explicit traced deleted queue preserves EBADF");
	E::Drain(); // Real transport output is available for the offline parser fixture.
	E::Detail::g_enabled.store(false);
	S::Detail::g_enabled.store(false);
}

} // namespace

int main() {
	TestPeriodicTimerEvents();
	TestDuplicateAddPreservesEventState();
	TestCallbackStateOutlivesPort();
	TestCallbackOwnsPayload();
	TestPinnedClose();
	TestStaleHandleNeverAliasesNewQueue();
	TestConcurrentCloseCallback();
	TestConcurrentDelete();
	TestTraceIntegrationPreservesQueue();
	std::printf("EventQueueLifetimeTests: all cases passed\n");
	return 0;
}
