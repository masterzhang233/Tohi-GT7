#include "kernel/eventQueueTraceState.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace T = Libs::LibKernel::EventQueue::TraceState;
namespace {
void Check(bool ok, const char* what) {
	if (!ok) { std::fprintf(stderr, "FAIL: event queue provenance: %s\n", what); std::exit(1); }
}
void Exact(const T::State& state, uint64_t token, uint32_t flags = 0) {
	const auto event = state.CurrentOccurrence();
	Check(event.first == token && event.last == token && event.count == 1 && event.flags == flags,
	      "exact occurrence preserves token and multiplicity");
}
void Unknown(const T::State& state, uint32_t reason) {
	const auto event = state.CurrentOccurrence();
	Check(event.first == 0 && event.last == 0 && event.count == 0 &&
	      (event.flags & (T::Unknown | reason)) == (T::Unknown | reason),
	      "unknown provenance cannot retain a seemingly exact token");
}
void Empty(const T::State& state) {
	const auto event = state.CurrentOccurrence();
	Check(event.first == 0 && event.last == 0 && event.count == 0 && event.flags == 0,
	      "actual guest emptiness resets only occurrence provenance");
}
void TestQueuedAndWrap() {
	T::State state;
	state.Initialize(77, false, 0, false);
	state.ObserveTrigger(101, false, 0, true, 0, T::Semantics::Queued);
	state.ObserveTrigger(102, true, 0, true, 1, T::Semantics::Queued);
	state.ObserveTrigger(103, true, 1, true, 2, T::Semantics::Queued);
	state.Check(true, 2); Exact(state, 101);
	state.AfterDelivery(true, 1, true); Exact(state, 102);
	// A partial result batch does not consume the following occurrence.
	state.Check(true, 1); Exact(state, 102);
	state.AfterDelivery(true, 0, true); Exact(state, 103);
	state.AfterDelivery(false, 0, false); Empty(state);
	Check(state.Registration() == 77, "draining does not change registration generation");
	// Keep current + one pending occupied while repeatedly wrapping the sidecar.
	state.ObserveTrigger(1000, false, 0, true, 0, T::Semantics::Queued);
	for (uint64_t token = 1001; token != 1201; ++token) {
		state.ObserveTrigger(token, true, 0, true, 1, T::Semantics::Queued);
		state.AfterDelivery(true, 0, true); Exact(state, token);
	}
	state.AfterDelivery(false, 0, false); Empty(state);
}
void TestCapacityAndRecovery() {
	T::State state;
	state.Initialize(91, false, 0, false);
	state.ObserveTrigger(1, false, 0, true, 0, T::Semantics::Queued);
	for (size_t n = 0; n < T::State::PendingCapacity; ++n) {
		state.ObserveTrigger(n + 2, true, n, true, n + 1, T::Semantics::Queued);
	}
	state.Check(true, 32); Exact(state, 1);
	// Current is separate: 32 pending plus current all retain exact FIFO order.
	for (size_t n = 0; n < 32; ++n) {
		state.AfterDelivery(true, 31 - n, true); Exact(state, n + 2);
	}
	state.AfterDelivery(false, 0, false);
	state.ObserveTrigger(100, false, 0, true, 0, T::Semantics::Queued);
	for (size_t n = 0; n < 33; ++n) {
		state.ObserveTrigger(n + 101, true, n, true, n + 1, T::Semantics::Queued);
	}
	Unknown(state, T::Overflow);
	for (size_t remaining = 32; remaining != 0; --remaining) {
		state.AfterDelivery(true, remaining, true); Unknown(state, T::Overflow);
	}
	state.AfterDelivery(true, 0, true); Unknown(state, T::Overflow);
	// More arrivals while old unknown current remains cannot manufacture identity.
	state.ObserveTrigger(999, true, 0, true, 1, T::Semantics::Queued);
	Unknown(state, T::Overflow);
	state.AfterDelivery(true, 0, true); Unknown(state, T::Overflow);
	state.AfterDelivery(false, 0, false); Empty(state);
	state.ObserveTrigger(1000, false, 0, true, 0, T::Semantics::Queued); Exact(state, 1000);
}
void TestPreArmAndInitial() {
	for (bool prearm: {false, true}) {
		T::State state;
		state.Initialize(111, true, 2, prearm);
		const auto reason = prearm ? T::PreArm : T::Initial;
		Unknown(state, reason);
		state.ObserveTrigger(777, true, 2, true, 3, T::Semantics::Queued);
		Unknown(state, reason);
		for (size_t pending = 3; pending != 0; --pending) {
			state.AfterDelivery(true, pending - 1, true); Unknown(state, reason);
		}
		state.AfterDelivery(false, 0, false); Empty(state);
		state.ObserveTrigger(778, false, 0, true, 0, T::Semantics::Queued); Exact(state, 778);
	}
	T::State empty_prearm;
	empty_prearm.Initialize(112, false, 0, true);
	empty_prearm.ObserveTrigger(779, false, 0, true, 0, T::Semantics::Queued);
	Exact(empty_prearm, 779);
}
void TestCoalescingAndRepeatedDelivery() {
	T::State state;
	state.Initialize(120, false, 0, false);
	state.ObserveTrigger(10, false, 0, true, 0, T::Semantics::Coalesced);
	Exact(state, 10);
	state.AfterDelivery(true, 0, false); Exact(state, 10, T::Repeated);
	state.AfterDelivery(true, 0, false); Exact(state, 10, T::Repeated);
	state.ObserveTrigger(20, true, 0, true, 0, T::Semantics::Coalesced);
	state.ObserveTrigger(30, true, 0, true, 0, T::Semantics::Coalesced);
	const auto occurrence = state.CurrentOccurrence();
	Check(occurrence.first == 10 && occurrence.last == 30 && occurrence.count == 3 &&
	      occurrence.flags == (T::Repeated | T::Coalesced),
	      "coalesced multiplicity counts triggers, not repeated deliveries");
	state.AfterDelivery(false, 0, false); Empty(state);
	state.ObserveTrigger(40, false, 0, true, 0, T::Semantics::Coalesced); Exact(state, 40);
}
void TestUnexpectedFunctionalTransitions() {
	for (unsigned which = 0; which != 6; ++which) {
		T::State state;
		state.Initialize(130 + which, false, 0, false);
		state.ObserveTrigger(100, false, 0, true, 0, T::Semantics::Queued);
		switch (which) {
			case 0: state.Check(true, 1); break; // guest pending unobserved
			case 1: state.AfterDelivery(true, 0, true); break; // unknown pop
			case 2: state.ObserveTrigger(0, true, 0, true, 1, T::Semantics::Queued); break;
			case 3: state.ObserveTrigger(200, true, 0, true, 0, T::Semantics::Unknown); break;
			case 4: state.ObserveTrigger(200, true, 0, true, 2, T::Semantics::Queued); break;
			case 5: state.ObserveTrigger(200, true, 0, false, 1, T::Semantics::Queued); break;
		}
		Unknown(state, T::Unsupported);
		state.Check(true, 0); Unknown(state, T::Unsupported);
		state.Check(false, 0); Empty(state);
	}
}
}
int main() {
	TestQueuedAndWrap(); TestCapacityAndRecovery(); TestPreArmAndInitial();
	TestCoalescingAndRepeatedDelivery(); TestUnexpectedFunctionalTransitions();
	std::puts("PASS: event queue provenance helper (FIFO/capacity/pre-arm/coalescing/recovery)");
}
