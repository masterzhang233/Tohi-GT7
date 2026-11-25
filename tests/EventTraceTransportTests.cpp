#include "common/eventTrace.h"
#include "common/sourceTrace.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

// Actual perfStats.cpp transport, called synchronously. No PerfReport thread,
// emulator, GPU, native guest waits, environment persistence or user arm file.
namespace S = Common::SourceTrace;
namespace E = Common::EventTrace;
namespace {
void Check(bool ok, const char* message) {
	if (!ok) { std::fprintf(stderr, "FAIL: event trace transport: %s\n", message); std::exit(1); }
}
void SetEnv(const char* name, const std::string& value) {
#ifdef _WIN32
	Check(_putenv_s(name, value.c_str()) == 0, "fixture process environment configured");
#else
	Check(setenv(name, value.c_str(), 1) == 0, "fixture process environment configured");
#endif
}
struct ArmFile {
	std::filesystem::path path;
	~ArmFile() { std::error_code ignored; std::filesystem::remove(path, ignored); }
};
}
int main() {
	ArmFile arm {std::filesystem::temp_directory_path() /
	             ("kyty-event-trace-fixture-" + std::to_string(E::ThreadId()) + "-" +
	              std::to_string(S::NowNs()) + ".arm")};
	Check(!std::filesystem::exists(arm.path), "fresh fixture arm path is absent");
	SetEnv("KYTY_GT7_SOURCE_TRACE", "1");
	SetEnv("KYTY_GT7_SOURCE_CHAIN", "1");
	SetEnv("KYTY_GT7_EVENT_TRACE", "1");
	SetEnv("KYTY_GT7_SOURCE_ARM_FILE", arm.path.string());
	S::Initialize(false); E::Initialize(false);
	Check(!S::Enabled() && !E::Enabled(), "disabled reporter cannot activate transport");
	S::Initialize(true); E::Initialize(true);
	Check(S::Enabled() && !S::Armed() && !E::Enabled(), "configured transport waits for arm file");
	S::Event premature_source {};
	premature_source.kind = S::Kind::FlipSubmit;
	premature_source.tid = E::ThreadId();
	premature_source.begin_ns = premature_source.end_ns = premature_source.work_end_ns = S::NowNs();
	S::Record(premature_source);
	E::Event premature_event {};
	premature_event.kind = E::Kind::Trigger;
	premature_event.id = 900001;
	premature_event.time_ns = S::NowNs();
	E::Record(premature_event); S::Drain(); E::Drain();
	Check(S::ProducerThreadId() == 0 && S::Detail::g_condition_threads.Admitted() == 0,
	      "pre-arm source records do not publish participant identity");
	{
		std::ofstream file(arm.path, std::ios::binary | std::ios::trunc);
		file << "one-shot fixture arm\n";
		Check(file.good(), "fixture arm file created");
	}
	S::Drain();
	const auto arm_ns = S::ArmTimestamp();
	const auto summary_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5100);
	Check(arm_ns > 1 && S::Armed() && E::Enabled(), "existing Drain arms both transports once");
	// Explicitly stale timestamps remain excluded even after arming.
	S::Record(premature_source); E::Record(premature_event);
	E::Event crossing = premature_event;
	crossing.id = 900002; crossing.begin_ns = arm_ns - 1; crossing.time_ns = S::NowNs();
	E::Record(crossing);
	auto producer = premature_source;
	producer.begin_ns = producer.end_ns = producer.work_end_ns = S::NowNs();
	producer.request_id = 101;
	S::Record(producer);
	Check(E::ThreadEligible(producer.tid), "armed source observation enables actual producer coverage");
	S::t_context = {501, 101, 2, 3};
	const auto callback = E::CaptureCallback(701, 801, 901, 55, 0);
	Check(callback.id != 0, "armed callback receives identity");
	E::EmitCallback(E::Kind::CallbackQueued, callback, 0, S::NowNs());
	const auto tick_wait_begin = S::NowNs();
	E::EmitCallback(E::Kind::TickWait, callback, tick_wait_begin, S::NowNs());
	E::EmitCallback(E::Kind::CallbackBegin, callback, 0, S::NowNs());
	const auto registration = E::NewIdentity(), wait = E::NewIdentity(), occurrence = E::NewIdentity();
	E::Event registration_event {};
	registration_event.kind = E::Kind::Registration; registration_event.id = registration;
	registration_event.queue_id = 201; registration_event.registration_id = registration;
	registration_event.filter = -14; registration_event.ident = 901; registration_event.mode = 1;
	E::Record(registration_event);
	E::Event wait_begin {};
	wait_begin.kind = E::Kind::WaitBegin; wait_begin.id = wait; wait_begin.queue_id = 201;
	wait_begin.time_ns = S::NowNs(); wait_begin.count = 1; wait_begin.mode = 2; wait_begin.extra = 1000;
	E::Record(wait_begin);
	E::Event trigger {};
	trigger.kind = E::Kind::Trigger; trigger.id = occurrence; trigger.parent_id = callback.id;
	trigger.queue_id = 201; trigger.registration_id = registration; trigger.filter = -14;
	trigger.ident = 901; trigger.data = 55; trigger.count = 1;
	trigger.scheduler_id = callback.scheduler_id; trigger.tick = callback.tick;
	trigger.submission_id = callback.submission_id; trigger.request_id = callback.request_id;
	trigger.begin_ns = trigger.time_ns = S::NowNs();
	E::Record(trigger);
	auto delivery = trigger;
	delivery.kind = E::Kind::Delivery; delivery.parent_id = wait; delivery.extra = occurrence;
	delivery.scheduler_id = delivery.tick = delivery.submission_id = delivery.request_id = 0;
	delivery.begin_ns = 0; delivery.time_ns = S::NowNs();
	E::Record(delivery);
	auto wait_end = wait_begin;
	wait_end.kind = E::Kind::WaitEnd; wait_end.begin_ns = wait_begin.time_ns; wait_end.time_ns = S::NowNs();
	E::Record(wait_end);
	E::EmitCallback(E::Kind::CallbackEnd, callback, 0, S::NowNs());
	E::Drain(); S::Drain();
	const auto id_before = E::NewIdentity();
	Check(!S::TryArm(S::NowNs()) && S::ArmTimestamp() == arm_ns && E::NewIdentity() > id_before,
	      "repeated explicit arm preserves timestamp and IDs");
	{
		std::ofstream file(arm.path, std::ios::binary | std::ios::trunc);
		file << "repeat arm must not reset\n";
	}
	S::Drain();
	Check(S::ArmTimestamp() == arm_ns && S::ProducerThreadId() == producer.tid,
	      "rewritten existing arm file cannot reset live identity state");
	std::this_thread::sleep_until(summary_deadline);
	// Summary accounting must be one source and nine event records. The parser
	// verifies real output counters; rejected pre-arm/straddling attempts are absent.
	S::Drain(); E::Drain();
	std::printf("PASS: event trace transport arm_ns=%llu producer_tid=%u callback=%llu wait=%llu occurrence=%llu expected_source_records=1 expected_event_records=9\n",
	            static_cast<unsigned long long>(arm_ns), producer.tid,
	            static_cast<unsigned long long>(callback.id), static_cast<unsigned long long>(wait),
	            static_cast<unsigned long long>(occurrence));
}
