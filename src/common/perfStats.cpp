#include "common/perfStats.h"

#include "common/common.h"
#include "common/cpuAffinity.h"
#include "common/hostSymbols.h"
#include "common/logging/log.h"
#include "common/sampler.h"
#include "common/sourceTrace.h"
#include "common/eventTrace.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep
#include <psapi.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <fstream>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Common::PerfStats {

namespace {

bool MirrorSilentDiagnostics() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GT7_PERF_DIAGNOSTICS");
		return value != nullptr && std::string_view(value) == "1";
	}();
	return enabled;
}

void WriteDiagnostic(std::string_view text) {
	// WriteUnmuted still obeys the Silent sink. Only explicit performance diagnostics
	// also reach the launcher's redirected stdout; ordinary Silent play stays silent.
	if (MirrorSilentDiagnostics() && Log::IsSilent() &&
	    Log::GetDirection() == Log::Direction::Silent) {
		Log::WriteToConsoleAndLog(text);
	} else {
		Log::WriteUnmuted(text);
	}
}

uint32_t IntervalSeconds() {
	static const uint32_t seconds = [] {
		const char* value = std::getenv("KYTY_PERF");
		return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 2u;
	}();
	return seconds;
}

uint32_t OsThreadId() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return static_cast<uint32_t>(GetCurrentThreadId());
#else
	return static_cast<uint32_t>(syscall(SYS_gettid));
#endif
}

struct Counters {
	std::array<std::atomic<uint64_t>, static_cast<size_t>(GpuThread::Count)> gpu {};

	std::atomic<uint64_t> draws {0};
	std::atomic<uint64_t> dispatches {0};
	std::atomic<uint64_t> presents {0};
	std::atomic<uint64_t> downloads {0};
	std::atomic<uint64_t> download_bytes {0};
	std::atomic<uint64_t> faults {0};
	std::atomic<uint64_t> fault_ns {0};
	std::atomic<uint64_t> fault_reads {0};
	std::atomic<uint64_t> fault_emulated {0};
	std::atomic<uint64_t> long_faults {0}; // 1 ms or more
	std::atomic<uint64_t> longest_fault_ns {0};

	// Game-thread accesses through the GPU thread (BufferCache::ReadMemory).
	std::atomic<uint64_t> readbacks {0};
	std::atomic<uint64_t> readback_pickup_ns {0};
	std::atomic<uint64_t> readback_record_ns {0};
	std::atomic<uint64_t> readback_wait_ns {0};
	std::atomic<uint64_t> readback_longest_wait_ns {0};
	std::atomic<uint64_t> readback_steps {0};
	std::atomic<uint64_t> readback_ticks_behind {0};

	// Stores to GPU-written pages.
	std::atomic<uint64_t> clean_stores {0};
	std::atomic<uint64_t> clean_store_ns {0};
	std::atomic<uint64_t> declined_stores {0};
	std::atomic<uint64_t> unsupported_stores {0};

	// Readahead downloads (BufferCache, KYTY_READAHEAD).
	std::atomic<uint64_t> readaheads {0};
	std::atomic<uint64_t> readahead_bytes {0};
	// Reads copied on the readback queue (BufferCache::FastReadback).
	std::atomic<uint64_t> fast_readbacks {0};
	std::atomic<uint64_t> fast_readback_bytes {0};
	std::atomic<uint64_t> fast_readback_wait_ns {0};
	std::atomic<uint64_t> longest_fast_readback_ns {0};

	// The host GPU's queue: submission to completion.
	std::atomic<uint64_t> ticks_done {0};
	std::atomic<uint64_t> tick_latency_ns {0};
	std::atomic<uint64_t> longest_tick_latency_ns {0};
	std::atomic<uint64_t> ticks_submitted {0};

	// Waits of KYTY_STALL_MS or more for the host GPU, and the longest host-side sections.
	std::atomic<uint64_t>                                               stalls {0};
	std::atomic<uint64_t>                                               longest_stall_ns {0};
	// Light grid uploads checked, those holding cells of more than 1024 lights, and such cells.
	std::atomic<uint64_t>                                               light_grids {0};
	std::atomic<uint64_t>                                               light_grids_garbage {0};
	std::atomic<uint64_t>                                               light_grid_cells {0};
	std::atomic<uint64_t>                                               light_grid_views {0};
	std::atomic<uint64_t> light_grid_views_garbage {0};
	std::array<std::atomic<uint64_t>, static_cast<size_t>(Host::Count)> longest_host_ns {};

	// Waits for the graphics caches' spin locks, by lock.
	std::array<std::atomic<uint64_t>, static_cast<size_t>(Lock::Count)> lock_waits {};
	std::array<std::atomic<uint64_t>, static_cast<size_t>(Lock::Count)> lock_wait_ns {};
	std::array<std::atomic<uint64_t>, static_cast<size_t>(Lock::Count)> longest_lock_wait_ns {};
};
Counters g_counters;

std::atomic<ExtraReporter> g_extra_reporter {nullptr};
std::atomic<ReportHook>    g_report_hook {nullptr};

uint64_t NowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

void StoreMax(std::atomic<uint64_t>& longest, uint64_t value) {
	for (auto current = longest.load(std::memory_order_relaxed);
	     value > current &&
	     !longest.compare_exchange_weak(current, value, std::memory_order_relaxed);) {
	}
}

// When each recent tick was submitted, by tick modulo the ring's size; a slot belongs to the tick
// it names.
constexpr size_t                                TickRingSize = 4096;
std::array<std::atomic<uint64_t>, TickRingSize> g_tick_ids {};
std::array<std::atomic<uint64_t>, TickRingSize> g_tick_submitted_ns {};
std::atomic<uint64_t>                           g_last_completed_tick {0};

// A thread's time in faults; slots are never freed (a thread that ends keeps its last values).
struct FaultSlot {
	uint32_t              tid = 0;
	std::atomic<uint64_t> ns {0};
	uint64_t              reported_ns = 0; // reporter thread
};
std::mutex                              g_slots_mutex;
std::vector<std::unique_ptr<FaultSlot>> g_slots;
thread_local FaultSlot*                 t_slot = nullptr;

std::mutex                                g_names_mutex;
std::unordered_map<uint32_t, std::string> g_names;

std::mutex  g_configuration_mutex;
std::string g_configuration;
std::string g_window_configuration;

std::string ThreadName(uint32_t tid) {
	{
		std::scoped_lock lock(g_names_mutex);
		if (const auto it = g_names.find(tid); it != g_names.end()) {
			return it->second;
		}
	}
	return fmt::format("tid {}", tid);
}

// Each thread's CPU time (user and kernel) so far, in nanoseconds.
std::unordered_map<uint32_t, uint64_t> ThreadCpuTimes() {
	std::unordered_map<uint32_t, uint64_t> times;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		return times;
	}
	const auto    process = GetCurrentProcessId();
	THREADENTRY32 entry {};
	entry.dwSize = sizeof(entry);
	for (BOOL more = Thread32First(snapshot, &entry); more != FALSE;
	     more      = Thread32Next(snapshot, &entry)) {
		if (entry.th32OwnerProcessID != process) {
			continue;
		}
		HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
		if (thread == nullptr) {
			continue;
		}
		FILETIME created {}, exited {}, kernel {}, user {};
		if (GetThreadTimes(thread, &created, &exited, &kernel, &user) != FALSE) {
			const auto value = [](const FILETIME& time) {
				return (static_cast<uint64_t>(time.dwHighDateTime) << 32u) | time.dwLowDateTime;
			};
			times[entry.th32ThreadID] = (value(kernel) + value(user)) * 100u;
		}
		CloseHandle(thread);
	}
	CloseHandle(snapshot);
#else
	DIR* directory = opendir("/proc/self/task");
	if (directory == nullptr) {
		return times;
	}
	const auto ticks = static_cast<uint64_t>(std::max(sysconf(_SC_CLK_TCK), 1l));
	while (const auto* entry = readdir(directory)) {
		if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
			continue;
		}
		std::ifstream file(std::string("/proc/self/task/") + entry->d_name + "/stat");
		std::string   line;
		if (!std::getline(file, line)) {
			continue;
		}
		// The fields after the command name (which may hold spaces) are counted from its ')'.
		const auto close = line.rfind(')');
		if (close == std::string::npos) {
			continue;
		}
		std::vector<std::string> fields;
		size_t                   at = close + 2;
		while (at < line.size()) {
			const auto end = line.find(' ', at);
			fields.push_back(line.substr(at, end == std::string::npos ? end : end - at));
			if (end == std::string::npos) {
				break;
			}
			at = end + 1;
		}
		if (fields.size() < 13) {
			continue;
		}
		// utime and stime: fields 14 and 15 of the line, 12 and 13 after the state.
		const auto cpu = std::strtoull(fields[11].c_str(), nullptr, 10) +
		                 std::strtoull(fields[12].c_str(), nullptr, 10);
		times[static_cast<uint32_t>(std::strtoul(entry->d_name, nullptr, 10))] =
		    cpu * 1'000'000'000ull / ticks;
	}
	closedir(directory);
#endif
	return times;
}

// Round 125: the process's memory and what Windows has left, at the end of each summary: a
// process that runs out of commit (RAM and page file) ends without a word of its own.
std::string MemoryText() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	PROCESS_MEMORY_COUNTERS_EX counters {};
	counters.cb = sizeof(counters);
	MEMORYSTATUSEX status {};
	status.dwLength = sizeof(status);
	if (K32GetProcessMemoryInfo(GetCurrentProcess(),
	                            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
	                            sizeof(counters)) == FALSE ||
	    GlobalMemoryStatusEx(&status) == FALSE) {
		return {};
	}
	constexpr double gb = 1024.0 * 1024.0 * 1024.0;
	return fmt::format(
	    " | memory: {:.1f} GB committed, {:.1f} GB in RAM; Windows: {:.1f} of {:.1f} "
	    "GB commit left, {:.1f} GB RAM free",
	    static_cast<double>(counters.PrivateUsage) / gb,
	    static_cast<double>(counters.WorkingSetSize) / gb,
	    static_cast<double>(status.ullAvailPageFile) / gb,
	    static_cast<double>(status.ullTotalPageFile) / gb,
	    static_cast<double>(status.ullAvailPhys) / gb);
#else
	return {};
#endif
}

std::string Percent(uint64_t part, uint64_t whole) {
	return fmt::format("{:.0f}%",
	                   whole == 0 ? 0.0
	                              : 100.0 * static_cast<double>(part) / static_cast<double>(whole));
}

void Report(double seconds, std::unordered_map<uint32_t, uint64_t>& last_cpu) {
	if (MirrorSilentDiagnostics()) {
		WriteDiagnostic(fmt::format("Perf batch: steady_ms={:.3f} window_s={:.3f}\n",
		                            static_cast<double>(NowNs()) / 1e6, seconds));
	}
	auto&      c    = g_counters;
	const auto take = [](std::atomic<uint64_t>& value) {
		return value.exchange(0, std::memory_order_relaxed);
	};
	const auto interval = static_cast<uint64_t>(seconds * 1e9);
	const auto idle     = take(c.gpu[static_cast<size_t>(GpuThread::Idle)]);
	const auto blocked  = take(c.gpu[static_cast<size_t>(GpuThread::Blocked)]);
	const auto busy     = take(c.gpu[static_cast<size_t>(GpuThread::Busy)]);
	const auto host_gpu = take(c.gpu[static_cast<size_t>(GpuThread::HostGpuWait)]);
	const auto draws    = take(c.draws);
	const auto dispatch = take(c.dispatches);
	const auto presents = take(c.presents);
	const auto loads    = take(c.downloads);
	const auto bytes    = take(c.download_bytes);
	const auto faults   = take(c.faults);
	const auto fault_ns = take(c.fault_ns);
	const auto reads    = take(c.fault_reads);
	const auto emulated = take(c.fault_emulated);
	const auto longs    = take(c.long_faults);
	const auto longest  = take(c.longest_fault_ns);
	const auto frames   = std::max<uint64_t>(presents, 1);
	const auto ops      = draws + dispatch;

	std::string configuration;
	{
		std::scoped_lock lock(g_configuration_mutex);
		configuration = g_configuration.empty() ? std::string() : " [" + g_configuration + "]";
	}
	WriteDiagnostic(fmt::format(
	    "Perf: {:.1f} s, {:.1f} fps{} | GPU thread: busy {} (host GPU {}), idle {}, blocked {} | "
	    "{} draws, {} dispatches a frame, {:.1f} us each | faults {:.0f}/s ({:.0f} reads, "
	    "{:.0f} emulated), {:.0f} ms/s in handlers, {:.0f}/s of 1 ms or more, longest {:.1f} ms "
	    "| downloads {:.0f}/s, {:.1f} MB/s{}\n",
	    seconds, static_cast<double>(presents) / seconds, configuration, Percent(busy, interval),
	    Percent(host_gpu, interval), Percent(idle, interval), Percent(blocked, interval),
	    draws / frames, dispatch / frames,
	    ops == 0 ? 0.0 : static_cast<double>(busy) / 1000.0 / static_cast<double>(ops),
	    static_cast<double>(faults) / seconds, static_cast<double>(reads) / seconds,
	    static_cast<double>(emulated) / seconds, static_cast<double>(fault_ns) / 1e6 / seconds,
	    static_cast<double>(longs) / seconds, static_cast<double>(longest) / 1e6,
	    static_cast<double>(loads) / seconds, static_cast<double>(bytes) / 1e6 / seconds,
	    MemoryText()));

	// The host GPU's queue, and what game-thread accesses to GPU-written memory wait for.
	{
		const auto submitted   = take(c.ticks_submitted);
		const auto done        = take(c.ticks_done);
		const auto latency     = take(c.tick_latency_ns);
		const auto latency_max = take(c.longest_tick_latency_ns);
		const auto readbacks   = take(c.readbacks);
		const auto pickup      = take(c.readback_pickup_ns);
		const auto record      = take(c.readback_record_ns);
		const auto wait        = take(c.readback_wait_ns);
		const auto wait_max    = take(c.readback_longest_wait_ns);
		const auto steps       = take(c.readback_steps);
		const auto behind      = take(c.readback_ticks_behind);
		const auto stores      = take(c.clean_stores);
		const auto store_ns    = take(c.clean_store_ns);
		const auto declined    = take(c.declined_stores);
		const auto unsupported = take(c.unsupported_stores);
		const auto aheads      = take(c.readaheads);
		const auto ahead_bytes = take(c.readahead_bytes);
		const auto fasts       = take(c.fast_readbacks);
		const auto fast_bytes  = take(c.fast_readback_bytes);
		const auto fast_wait   = take(c.fast_readback_wait_ns);
		const auto fast_max    = take(c.longest_fast_readback_ns);
		const auto stalls      = take(c.stalls);
		const auto stall_max   = take(c.longest_stall_ns);
		const auto grids       = take(c.light_grids);
		const auto bad_grids   = take(c.light_grids_garbage);
		const auto grid_cells  = take(c.light_grid_cells);
		const auto grid_views  = take(c.light_grid_views);
		const auto bad_views   = take(c.light_grid_views_garbage);
		const auto acquire_max = take(c.longest_host_ns[static_cast<size_t>(Host::Acquire)]);
		const auto present_max = take(c.longest_host_ns[static_cast<size_t>(Host::Present)]);
		const auto lock_max    = take(c.longest_host_ns[static_cast<size_t>(Host::QueueLock)]);
		const auto per         = [](uint64_t sum, uint64_t count, double unit) {
            return count == 0 ? 0.0 : static_cast<double>(sum) / unit / static_cast<double>(count);
		};
		std::string extra;
		if (const auto reporter = g_extra_reporter.load(std::memory_order_acquire);
		    reporter != nullptr) {
			extra = reporter();
		}
		WriteDiagnostic(fmt::format(
		    "Perf GPU:{} command buffers {:.0f}/s, done {:.1f} ms after submission (longest "
		    "{:.1f}) "
		    "| readbacks {:.0f}/s: taken up in {:.0f} us, {:.0f} us on the GPU thread, landed "
		    "{:.2f} ms later (longest {:.1f}), {:.1f} steps, {:.1f} command buffers unfinished | "
		    "stores to GPU-written pages done in place {:.0f}/s ({:.0f} us each), declined "
		    "{:.0f}/s, "
		    "not handled {:.0f}/s | readahead {:.0f}/s ({:.1f} MB/s) | fast readbacks {:.0f}/s "
		    "({:.1f} MB/s, {:.0f} us each, longest {:.0f}) | stalls {} (longest {:.0f} "
		    "ms) | light grids {} checked, {} with garbage ({} cells), views {} checked, {} with "
		    "garbage | longest swapchain acquire "
		    "{:.1f} ms, present {:.1f} ms, queue lock wait {:.1f} ms{}\n",
		    configuration, static_cast<double>(submitted) / seconds, per(latency, done, 1e6),
		    static_cast<double>(latency_max) / 1e6, static_cast<double>(readbacks) / seconds,
		    per(pickup, readbacks, 1e3), per(record, readbacks, 1e3), per(wait, readbacks, 1e6),
		    static_cast<double>(wait_max) / 1e6, per(steps, readbacks, 1.0),
		    per(behind, readbacks, 1.0), static_cast<double>(stores) / seconds,
		    per(store_ns, stores, 1e3), static_cast<double>(declined) / seconds,
		    static_cast<double>(unsupported) / seconds, static_cast<double>(aheads) / seconds,
		    static_cast<double>(ahead_bytes) / 1e6 / seconds, static_cast<double>(fasts) / seconds,
		    static_cast<double>(fast_bytes) / 1e6 / seconds, per(fast_wait, fasts, 1e3),
		    static_cast<double>(fast_max) / 1e3, stalls, static_cast<double>(stall_max) / 1e6,
		    grids, bad_grids, grid_cells, grid_views, bad_views,
		    static_cast<double>(acquire_max) / 1e6, static_cast<double>(present_max) / 1e6,
		    static_cast<double>(lock_max) / 1e6, extra));
	}

	// The graphics caches' locks: how often a thread found one held, and how long it waited.
	{
		std::string          line = fmt::format("Perf locks:{}", configuration);
		constexpr std::array names {"texture cache", "memory tracker", "renderer",
		                            "renderer held by the presenter"};
		constexpr std::array nouns {"waits", "waits", "waits", "holds"};
		static_assert(names.size() == static_cast<size_t>(Lock::Count));
		for (size_t i = 0; i < names.size(); i++) {
			const auto waits        = take(c.lock_waits[i]);
			const auto wait_ns      = take(c.lock_wait_ns[i]);
			const auto longest_wait = take(c.longest_lock_wait_ns[i]);
			line += fmt::format("{} {} {:.0f} {}/s, {:.1f} ms/s, longest {:.2f} ms",
			                    i == 0 ? "" : " |", names[i], static_cast<double>(waits) / seconds,
			                    nouns[i], static_cast<double>(wait_ns) / 1e6 / seconds,
			                    static_cast<double>(longest_wait) / 1e6);
		}
		line += SpinLocks() ? " (spinning as before round 65)" : "";
		WriteDiagnostic(line + "\n");
	}

	// The busiest threads, and those that spent longest in faults.
	auto                                       now_cpu = ThreadCpuTimes();
	std::vector<std::pair<uint64_t, uint32_t>> cpu;
	uint64_t                                   total = 0;
	for (const auto& [tid, ns]: now_cpu) {
		const auto it    = last_cpu.find(tid);
		const auto delta = it == last_cpu.end() || ns < it->second ? 0 : ns - it->second;
		total += delta;
		if (delta != 0) {
			cpu.emplace_back(delta, tid);
		}
	}
	last_cpu = std::move(now_cpu);
	std::ranges::sort(cpu, std::greater {});
	std::string line = fmt::format("Perf threads: CPU {:.1f} cores:",
	                               static_cast<double>(total) / static_cast<double>(interval));
	for (size_t i = 0; i < std::min<size_t>(cpu.size(), 10); i++) {
		line += fmt::format("{} {} {}", i == 0 ? "" : ",", ThreadName(cpu[i].second),
		                    Percent(cpu[i].first, interval));
	}
	std::vector<std::pair<uint64_t, uint32_t>> stalled;
	{
		std::scoped_lock lock(g_slots_mutex);
		for (auto& slot: g_slots) {
			const auto ns     = slot->ns.load(std::memory_order_relaxed);
			const auto delta  = ns - slot->reported_ns;
			slot->reported_ns = ns;
			if (delta >= 1'000'000) {
				stalled.emplace_back(delta, slot->tid);
			}
		}
	}
	std::ranges::sort(stalled, std::greater {});
	if (!stalled.empty()) {
		line += " | in faults:";
		for (size_t i = 0; i < std::min<size_t>(stalled.size(), 6); i++) {
			line += fmt::format("{} {} {:.0f} ms", i == 0 ? "" : ",", ThreadName(stalled[i].second),
			                    static_cast<double>(stalled[i].first) / 1e6);
		}
	}
	WriteDiagnostic(line + "\n");
}

void Run() {
	const auto interval = std::chrono::seconds(IntervalSeconds());
	auto       last     = std::chrono::steady_clock::now();
	auto       cpu      = ThreadCpuTimes();
	for (;;) {
		if (Common::SourceTrace::Enabled()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			Common::SourceTrace::Drain();
			Common::EventTrace::Drain();
		} else {
			std::this_thread::sleep_for(interval);
		}
		const auto now     = std::chrono::steady_clock::now();
		if (now - last < interval) continue;
		const auto seconds = std::chrono::duration<double>(now - last).count();
		last               = now;
		Report(seconds, cpu);
		if (const auto hook = g_report_hook.load(std::memory_order_acquire); hook != nullptr) {
			hook();
		}
	}
}

} // namespace

bool Enabled() {
	return IntervalSeconds() != 0;
}

namespace Detail {

void AddGpuThreadSlow(GpuThread phase, uint64_t nanoseconds) {
	g_counters.gpu[static_cast<size_t>(phase)].fetch_add(nanoseconds, std::memory_order_relaxed);
}

void AddDrawSlow() {
	g_counters.draws.fetch_add(1, std::memory_order_relaxed);
}

void AddDispatchSlow() {
	g_counters.dispatches.fetch_add(1, std::memory_order_relaxed);
}

void AddPresentSlow() {
	g_counters.presents.fetch_add(1, std::memory_order_relaxed);
}

void AddDownloadSlow(uint64_t bytes) {
	g_counters.downloads.fetch_add(1, std::memory_order_relaxed);
	g_counters.download_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void AddFaultSlow(uint64_t nanoseconds, bool read, bool emulated) {
	auto& c = g_counters;
	c.faults.fetch_add(1, std::memory_order_relaxed);
	c.fault_ns.fetch_add(nanoseconds, std::memory_order_relaxed);
	if (read) {
		c.fault_reads.fetch_add(1, std::memory_order_relaxed);
	}
	if (emulated) {
		c.fault_emulated.fetch_add(1, std::memory_order_relaxed);
	}
	if (nanoseconds >= 1'000'000) {
		c.long_faults.fetch_add(1, std::memory_order_relaxed);
	}
	for (auto longest = c.longest_fault_ns.load(std::memory_order_relaxed);
	     nanoseconds > longest && !c.longest_fault_ns.compare_exchange_weak(
	                                  longest, nanoseconds, std::memory_order_relaxed);) {
	}
	if (t_slot == nullptr) {
		auto slot = std::make_unique<FaultSlot>();
		slot->tid = OsThreadId();
		std::scoped_lock lock(g_slots_mutex);
		t_slot = slot.get();
		g_slots.push_back(std::move(slot));
	}
	t_slot->ns.fetch_add(nanoseconds, std::memory_order_relaxed);
}

void AddReadbackSlow(uint64_t pickup_ns, uint64_t record_ns, uint64_t wait_ns, uint32_t steps,
                     uint64_t ticks_behind) {
	auto& c = g_counters;
	c.readbacks.fetch_add(1, std::memory_order_relaxed);
	c.readback_pickup_ns.fetch_add(pickup_ns, std::memory_order_relaxed);
	c.readback_record_ns.fetch_add(record_ns, std::memory_order_relaxed);
	c.readback_wait_ns.fetch_add(wait_ns, std::memory_order_relaxed);
	c.readback_steps.fetch_add(steps, std::memory_order_relaxed);
	c.readback_ticks_behind.fetch_add(ticks_behind, std::memory_order_relaxed);
	StoreMax(c.readback_longest_wait_ns, wait_ns);
}

void AddCleanStoreSlow(uint64_t nanoseconds) {
	g_counters.clean_stores.fetch_add(1, std::memory_order_relaxed);
	g_counters.clean_store_ns.fetch_add(nanoseconds, std::memory_order_relaxed);
}

void AddDeclinedStoreSlow() {
	g_counters.declined_stores.fetch_add(1, std::memory_order_relaxed);
}

void AddUnsupportedStoreSlow() {
	g_counters.unsupported_stores.fetch_add(1, std::memory_order_relaxed);
}

void TickSubmittedSlow(uint64_t tick) {
	const auto slot = tick % TickRingSize;
	g_tick_submitted_ns[slot].store(NowNs(), std::memory_order_relaxed);
	g_tick_ids[slot].store(tick, std::memory_order_release);
	g_counters.ticks_submitted.fetch_add(1, std::memory_order_relaxed);
}

void TickCompletedSlow(uint64_t tick) {
	// Once per tick, the first time a waiter sees it done (several operations wait for one tick).
	for (auto last = g_last_completed_tick.load(std::memory_order_relaxed);;) {
		if (tick <= last) {
			return;
		}
		if (g_last_completed_tick.compare_exchange_weak(last, tick, std::memory_order_relaxed)) {
			break;
		}
	}
	const auto slot = tick % TickRingSize;
	if (g_tick_ids[slot].load(std::memory_order_acquire) != tick) {
		return;
	}
	const auto submitted = g_tick_submitted_ns[slot].load(std::memory_order_relaxed);
	const auto now       = NowNs();
	if (now < submitted) {
		return;
	}
	auto& c = g_counters;
	c.ticks_done.fetch_add(1, std::memory_order_relaxed);
	c.tick_latency_ns.fetch_add(now - submitted, std::memory_order_relaxed);
	StoreMax(c.longest_tick_latency_ns, now - submitted);
}

void AddStallSlow(uint64_t nanoseconds, bool distinct) {
	if (distinct) {
		g_counters.stalls.fetch_add(1, std::memory_order_relaxed);
	}
	StoreMax(g_counters.longest_stall_ns, nanoseconds);
}

void AddHostSlow(uint32_t which, uint64_t nanoseconds) {
	if (which < g_counters.longest_host_ns.size()) {
		StoreMax(g_counters.longest_host_ns[which], nanoseconds);
	}
}

void AddReadaheadSlow(uint64_t bytes) {
	g_counters.readaheads.fetch_add(1, std::memory_order_relaxed);
	g_counters.readahead_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void AddFastReadbackSlow(uint64_t wait_ns, uint64_t bytes) {
	g_counters.fast_readbacks.fetch_add(1, std::memory_order_relaxed);
	g_counters.fast_readback_bytes.fetch_add(bytes, std::memory_order_relaxed);
	g_counters.fast_readback_wait_ns.fetch_add(wait_ns, std::memory_order_relaxed);
	StoreMax(g_counters.longest_fast_readback_ns, wait_ns);
}

void AddLightGridSlow(uint32_t garbage_cells) {
	g_counters.light_grids.fetch_add(1, std::memory_order_relaxed);
	if (garbage_cells != 0) {
		g_counters.light_grids_garbage.fetch_add(1, std::memory_order_relaxed);
		g_counters.light_grid_cells.fetch_add(garbage_cells, std::memory_order_relaxed);
	}
}

void AddLightGridViewSlow(uint32_t garbage_cells) {
	g_counters.light_grid_views.fetch_add(1, std::memory_order_relaxed);
	if (garbage_cells != 0) {
		g_counters.light_grid_views_garbage.fetch_add(1, std::memory_order_relaxed);
	}
}

void AddLockWaitSlow(uint32_t lock, uint64_t nanoseconds) {
	if (lock >= g_counters.lock_waits.size()) {
		return;
	}
	g_counters.lock_waits[lock].fetch_add(1, std::memory_order_relaxed);
	g_counters.lock_wait_ns[lock].fetch_add(nanoseconds, std::memory_order_relaxed);
	StoreMax(g_counters.longest_lock_wait_ns[lock], nanoseconds);
}

} // namespace Detail

void SetSpinLocks(bool spin) {
	Detail::g_spin_locks.store(spin, std::memory_order_relaxed);
}

bool SpinLocksFromEnvironment() {
	static const bool spin = [] {
		const char* value = std::getenv("KYTY_LOCKS_SPIN");
		return value != nullptr && value[0] == '1';
	}();
	return spin;
}

void SetReportHook(ReportHook hook) {
	g_report_hook.store(hook, std::memory_order_release);
}

void SetExtraReporter(ExtraReporter reporter) {
	g_extra_reporter.store(reporter, std::memory_order_release);
}

uint32_t CurrentOsThreadId() {
	return OsThreadId();
}

std::string ThreadNameOf(uint32_t tid) {
	return ThreadName(tid);
}

std::vector<std::pair<uint32_t, std::string>> NamedThreads() {
	std::scoped_lock                              lock(g_names_mutex);
	std::vector<std::pair<uint32_t, std::string>> threads(g_names.begin(), g_names.end());
	return threads;
}

void NameThread(const char* name) {
	if (name == nullptr || name[0] == '\0') {
		return;
	}
	{
		std::scoped_lock lock(g_names_mutex);
		g_names[OsThreadId()] = name;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Windows 10 1607 and later; looked up so that older systems still start.
	using SetDescription              = HRESULT(WINAPI*)(HANDLE, PCWSTR);
	static const auto set_description = reinterpret_cast<SetDescription>(reinterpret_cast<void*>(
	    GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
	if (set_description != nullptr) {
		wchar_t wide[64] {};
		if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, static_cast<int>(std::size(wide))) >
		    0) {
			set_description(GetCurrentThread(), wide);
		}
	}
#endif
}

void SetConfiguration(std::string_view name) {
	std::scoped_lock lock(g_configuration_mutex);
	g_configuration = std::string(name);
}

std::string Configuration() {
	std::scoped_lock lock(g_configuration_mutex);
	return g_configuration;
}

void SetWindowConfiguration(std::string_view name) {
	std::scoped_lock lock(g_configuration_mutex);
	g_window_configuration = std::string(name);
}

std::string WindowConfiguration() {
	std::scoped_lock lock(g_configuration_mutex);
	return g_window_configuration.empty() ? g_configuration : g_window_configuration;
}

void Start() {
	static std::once_flag started;
	std::call_once(started, [] {
		// The host symbols (dbghelp, else the map file next to the executable) load in the
		// background: the first slow wait's host stack (MasterSemaphore) then does not pay for
		// them on a thread that matters.
		std::thread([] {
			NameThread("SymbolLoad");
			std::this_thread::sleep_for(std::chrono::seconds(3));
			(void)Common::HostSymbol(reinterpret_cast<uint64_t>(&Report));
		}).detach();
		std::string reason;
		if (const auto set = Common::StartCpuSet(&reason); !set.empty()) {
			std::string description;
			const bool  ok = Common::SetProcessCpuSet(set.c_str(), &description);
			LOGF("CPU: %s=%s (%s): %s\n", reason == "KYTY_CPU_SET" ? "KYTY_CPU_SET" : "placement",
			     set.c_str(), reason.c_str(), description.c_str());
			(void)ok;
		} else {
			LOGF("CPU: the threads' placement is left alone (%s; KYTY_CPU_SET=gpuvcache places the "
			     "GPU-side threads on the processors sharing the largest L3 cache)\n",
			     reason.c_str());
		}
		SetSpinLocks(SpinLocksFromEnvironment());
		Common::Sampler::Start();
		Common::SourceTrace::Initialize(Enabled());
		Common::EventTrace::Initialize(Enabled());
		if (!Enabled()) {
			return;
		}
		Detail::g_enabled.store(true, std::memory_order_relaxed);
		WriteDiagnostic(fmt::format(
		    "Perf: a summary every {} s (KYTY_PERF=<seconds>, 0 turns it off)\n",
		    IntervalSeconds()));
		std::thread([] {
			NameThread("PerfReport");
			Run();
		}).detach();
	});
}

} // namespace Common::PerfStats

namespace Common::SourceTrace {
namespace {
// Process-lifetime storage, initialized before producers are enabled. No pointer
// to live guest or renderer objects is retained. The process tail after the last
// completed drain is deliberately incomplete (<=250 ms normally), not fabricated.
Detail::Ring<4096> g_ring;
Detail::Ring<8192> g_extra_condition_ring;
Detail::RecordBudget<Detail::BaseRecordBudget> g_base_budget;
uint64_t g_drained = 0, g_extra_drained = 0, g_last_report_ns = 0;
bool g_budget_reported = false, g_extra_budget_reported = false;
std::array<bool, Detail::ConditionThreadLimit> g_identity_reported {};
uint64_t g_identity_unavailable = 0;
std::filesystem::path g_arm_file;

std::string QuoteThreadName(std::string_view name) {
	std::string result = "\"";
	for (const unsigned char ch: name) {
		if (ch == '"' || ch == '\\') { result += '\\'; result += static_cast<char>(ch); }
		else if (ch < 0x20) result += fmt::format("\\u{:04x}", ch);
		else result += static_cast<char>(ch);
	}
	result += '"';
	return result;
}

void AppendConditionThreadIdentities(std::string& batch) {
	if (!ChainEnabled()) return;
	for (size_t slot = 0; slot != Detail::ConditionThreadLimit; ++slot) {
		const auto tid = Detail::g_condition_threads.At(slot);
		if (tid == 0 || g_identity_reported[slot]) continue;
		g_identity_reported[slot] = true;
		const auto query_ns = NowNs();
		uint64_t created = 0;
		bool live = false, name_verified = false, name_truncated = false;
		std::string name = "<unavailable>";
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		// Query only, on the reporter. This is the current handle's identity, not
		// proof that an older event with the same numeric TID belongs to its lifetime.
		HANDLE handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, tid);
		if (handle != nullptr) {
			live = GetProcessIdOfThread(handle) == GetCurrentProcessId() &&
			       WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
			if (live) {
				FILETIME creation {}, exited {}, kernel {}, user {};
				if (GetThreadTimes(handle, &creation, &exited, &kernel, &user) != FALSE) {
					created = (static_cast<uint64_t>(creation.dwHighDateTime) << 32u) | creation.dwLowDateTime;
				}
				using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
				static const auto get_description = reinterpret_cast<GetDescription>(
				    GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
				PWSTR description = nullptr;
				if (get_description != nullptr && SUCCEEDED(get_description(handle, &description))) {
					name_verified = true;
					name = "<unnamed>";
					if (description != nullptr) {
						size_t length = 0;
						while (length < 90 && description[length] != L'\0') ++length;
						name_truncated = length == 90 && description[length] != L'\0';
						std::array<char, 361> utf8 {};
						const int bytes = length == 0 ? 0 : WideCharToMultiByte(
						    CP_UTF8, 0, description, static_cast<int>(length), utf8.data(), 360, nullptr, nullptr);
						if (bytes > 0) name.assign(utf8.data(), static_cast<size_t>(bytes));
					}
				}
				if (description != nullptr) LocalFree(description);
			}
			CloseHandle(handle);
		}
#endif
		if (!live) ++g_identity_unavailable;
		batch += fmt::format(
		    "GT7 source chain thread: slot={} tid={} query_ns={} live={} created={} "
		    "name_verified={} name_truncated={} name={}\n",
		    slot, tid, query_ns, live ? 1 : 0, created, name_verified ? 1 : 0,
		    name_truncated ? 1 : 0, QuoteThreadName(name));
	}
}
}

void Initialize(bool reporter_enabled) {
	const char* value = std::getenv("KYTY_GT7_SOURCE_TRACE");
	if (!reporter_enabled || value == nullptr || std::string_view(value) != "1") return;
	// Initialize clock and platform thread-id facilities outside exception paths.
	g_last_report_ns = NowNs();
	(void)Common::PerfStats::CurrentOsThreadId();
	const char* chain_value = std::getenv("KYTY_GT7_SOURCE_CHAIN");
	const bool chain = chain_value != nullptr && std::string_view(chain_value) == "1";
	Detail::g_chain_enabled.store(chain, std::memory_order_relaxed);
	if (const char* arm_file = std::getenv("KYTY_GT7_SOURCE_ARM_FILE"); arm_file != nullptr && *arm_file != '\0') {
		g_arm_file = std::filesystem::u8path(arm_file);
		Detail::g_arm_ns.store(0, std::memory_order_relaxed);
	}
	Log::WriteToConsoleAndLog(fmt::format(
	    "GT7 source trace: version=1 clock=steady_ns slots={} budget={} slow_ns=10000000 drain_ms=250 "
	    "tail_incomplete=1 prepare_text_suppressed=1 chain_enabled={} chain_limit={} "
	    "base_slots=4096 base_budget={} extra_slots={} extra_budget={}\n",
	    chain ? 12288 : 4096, Detail::BaseRecordBudget + (chain ? Detail::ExtraConditionRecordBudget : 0),
	    chain ? 1 : 0, Detail::ConditionThreadLimit, Detail::BaseRecordBudget,
	    chain ? 8192 : 0, chain ? Detail::ExtraConditionRecordBudget : 0));
	Detail::g_enabled.store(true, std::memory_order_release);
	if (!g_arm_file.empty()) {
		Log::WriteToConsoleAndLog("GT7 source trace arm: waiting=1 one_shot=1 pre_arm_events=excluded\n");
	}
}

void Record(Event event) noexcept {
	if (!Armed() || event.begin_ns < ArmTimestamp()) return;
	if (event.tid == 0) event.tid = Common::PerfStats::CurrentOsThreadId();
	Detail::ObserveThreadEvent(event);
	if (Detail::UseExtraConditionBucket(event, ProducerThreadId())) {
		if (Detail::g_extra_condition_budget.TryAdmit()) (void)g_extra_condition_ring.TryPush(event);
	} else if (g_base_budget.TryAdmit()) {
		(void)g_ring.TryPush(event);
	}
}

void Drain() {
	if (!Armed()) {
		// Only the existing reporter touches the filesystem; gameplay lanes merely
		// check the atomic timestamp. A fresh run has a fresh arm-file path.
		std::error_code error;
		if (g_arm_file.empty() || !std::filesystem::is_regular_file(g_arm_file, error) || error) return;
		const auto arm_ns = NowNs();
		if (!TryArm(arm_ns)) return;
		g_last_report_ns = arm_ns;
		Log::WriteToConsoleAndLog(fmt::format(
		    "GT7 source trace arm: waiting=0 arm_ns={} one_shot=1 pre_arm_events=excluded\n", arm_ns));
	}
	const bool chain = ChainEnabled();
	const auto report_begin_ns = chain ? NowNs() : 0;
	std::string batch;
	const auto append_event = [&](const Event& event) {
		batch += fmt::format(
		    "GT7 source event: kind={} tid={} flags={} begin_ns={} work_end_ns={} "
		    "end_ns={} submission={} request={} arg0={} arg1={}\n",
		    static_cast<uint32_t>(event.kind), event.tid, event.flags, event.begin_ns,
		    event.work_end_ns, event.end_ns, event.submission_id, event.request_id,
		    event.arg0, event.arg1);
	};
	const auto base_events = g_ring.Drain(append_event);
	const auto extra_events = chain ? g_extra_condition_ring.Drain(append_event) : 0;
	g_drained += base_events;
	g_extra_drained += extra_events;
	AppendConditionThreadIdentities(batch);
	const auto now = NowNs();
	// Ordered samples, not an atomic transaction: admitted attempts may still be
	// in flight. Reading attempts last preserves captured+dropped <= attempted.
	const auto base_captured = g_ring.Captured(), extra_captured = g_extra_condition_ring.Captured();
	const auto base_dropped = g_ring.Dropped(), extra_dropped = g_extra_condition_ring.Dropped();
	const auto base_attempted = g_base_budget.Attempted();
	const auto extra_attempted = Detail::g_extra_condition_budget.Attempted();
	const bool base_exhausted = base_attempted >= Detail::BaseRecordBudget;
	const bool extra_exhausted = ChainEnabled() && extra_attempted >= Detail::ExtraConditionRecordBudget;
	if (now - g_last_report_ns >= 5'000'000'000ull || (base_exhausted && !g_budget_reported) ||
	    (extra_exhausted && !g_extra_budget_reported)) {
		const auto base_skipped = base_attempted > Detail::BaseRecordBudget ? base_attempted - Detail::BaseRecordBudget : 0;
		const auto extra_skipped = extra_attempted > Detail::ExtraConditionRecordBudget
		                               ? extra_attempted - Detail::ExtraConditionRecordBudget : 0;
		const auto chain_admitted = Detail::g_condition_threads.Admitted();
		batch += fmt::format(
		    "GT7 source trace summary: steady_ns={} attempted={} captured={} drained={} "
		    "dropped={} budget_skipped={} exhausted={} tail_incomplete=1 "
		    "base_attempted={} base_captured={} base_drained={} base_dropped={} base_budget_skipped={} base_exhausted={} "
		    "extra_attempted={} extra_captured={} extra_drained={} extra_dropped={} extra_budget_skipped={} extra_exhausted={} "
		    "chain_enabled={} chain_limit={} chain_count={} chain_admitted={} chain_rejected={} "
		    "chain_budget_blocked={} chain_identity_unavailable={}\n",
		    now, base_attempted + extra_attempted, base_captured + extra_captured, g_drained + g_extra_drained,
		    base_dropped + extra_dropped, base_skipped + extra_skipped, (base_exhausted || extra_exhausted) ? 1 : 0,
		    base_attempted, base_captured, g_drained, base_dropped, base_skipped, base_exhausted ? 1 : 0,
		    extra_attempted, extra_captured, g_extra_drained, extra_dropped, extra_skipped, extra_exhausted ? 1 : 0,
		    chain ? 1 : 0, Detail::ConditionThreadLimit, chain_admitted,
		    chain_admitted, Detail::g_condition_threads.Rejected(),
		    Detail::g_chain_budget_blocked.load(std::memory_order_relaxed), g_identity_unavailable);
		g_last_report_ns = now;
		g_budget_reported = base_exhausted;
		g_extra_budget_reported = extra_exhausted;
	}
	// Only the existing background reporter formats or flushes output. Slots are
	// already released before this I/O; full/contended slots drop without waiting.
	if (!batch.empty()) Log::WriteToConsoleAndLog(batch);
	if (chain) {
		const auto report_end_ns = NowNs();
		// Reporter wall time includes formatting, identity queries, and the main
		// batch write. Formatting/writing this separate timing line is excluded.
		Log::WriteToConsoleAndLog(fmt::format(
		    "GT7 source trace report: begin_ns={} end_ns={} wall_ns={} base_events={} extra_events={}\n",
		    report_begin_ns, report_end_ns, report_end_ns - report_begin_ns, base_events, extra_events));
	}
}
} // namespace Common::SourceTrace

namespace Common::EventTrace {
namespace {
constexpr uint64_t RecordLimit = 1'500'000;
constexpr size_t RingCapacity = 8192;
SourceTrace::Detail::Ring<RingCapacity, Event> g_ring;
SourceTrace::Detail::RecordBudget<RecordLimit> g_budget;
uint64_t g_drained = 0, g_last_report_ns = 0;
bool g_exhaustion_reported = false;
}

void Initialize(bool reporter_enabled) {
	const char* value = std::getenv("KYTY_GT7_EVENT_TRACE");
	if (!reporter_enabled || !SourceTrace::ChainEnabled() || value == nullptr ||
	    std::string_view(value) != "1") return;
	g_last_report_ns = NowNs();
	Detail::g_enabled.store(true, std::memory_order_release);
	Log::WriteToConsoleAndLog(fmt::format(
	    "GT7 event trace: version=1 clock=steady_ns slots={} budget={} drain_ms=250 "
	    "tail_incomplete=1 separate_source_budget=1 ids=opaque_process_local "
	    "tick_scope=captured_priority_callbacks_only callback_scope=all_priority_callbacks "
	    "unselected_tick_is_drop=0\n", RingCapacity, RecordLimit));
}

void Record(Event event) noexcept {
	if (!Enabled()) return;
	if (event.time_ns == 0) event.time_ns = NowNs();
	const auto arm_ns = SourceTrace::ArmTimestamp();
	if (event.time_ns < arm_ns || (event.begin_ns != 0 && event.begin_ns < arm_ns)) return;
	if (event.tid == 0) event.tid = ThreadId();
	if (g_budget.TryAdmit()) (void)g_ring.TryPush(event);
}

void Drain() {
	if (!Enabled()) return;
	const auto begin_ns = NowNs();
	std::string batch;
	const auto drained = g_ring.Drain([&](const Event& event) {
		batch += fmt::format(
		    "GT7 event trace event: kind={} flags={} tid={} filter={} result={} count={} index={} "
		    "fflags={} event_flags={} mode={} time_ns={} begin_ns={} id={} parent_id={} queue_id={} "
		    "registration_id={} scheduler_id={} tick={} submission_id={} request_id={} ident={} data={} extra={}\n",
		    static_cast<uint32_t>(event.kind), event.flags, event.tid, event.filter, event.result,
		    event.count, event.index, event.fflags, event.event_flags, event.mode, event.time_ns,
		    event.begin_ns, event.id, event.parent_id, event.queue_id, event.registration_id,
		    event.scheduler_id, event.tick, event.submission_id, event.request_id,
		    event.ident, event.data, event.extra);
	});
	g_drained += drained;
	const auto now = NowNs();
	const auto captured = g_ring.Captured(), dropped = g_ring.Dropped();
	const auto attempted = g_budget.Attempted();
	const auto skipped = attempted > RecordLimit ? attempted - RecordLimit : 0;
	const bool exhausted = attempted >= RecordLimit;
	if (now - g_last_report_ns >= 5'000'000'000ull || (exhausted && !g_exhaustion_reported)) {
		batch += fmt::format(
		    "GT7 event trace summary: steady_ns={} attempted={} captured={} drained={} dropped={} "
		    "budget_skipped={} exhausted={} arm_ns={} unselected_tick={} tail_incomplete=1\n",
		    now, attempted, captured, g_drained, dropped, skipped,
		    exhausted ? 1 : 0, SourceTrace::ArmTimestamp(),
		    Detail::g_unselected_ticks.load(std::memory_order_relaxed));
		g_last_report_ns = now;
		g_exhaustion_reported = exhausted;
	}
	if (!batch.empty()) Log::WriteToConsoleAndLog(batch);
	const auto end_ns = NowNs();
	Log::WriteToConsoleAndLog(fmt::format(
	    "GT7 event trace report: begin_ns={} end_ns={} wall_ns={} events={}\n",
	    begin_ns, end_ns, end_ns - begin_ns, drained));
}
} // namespace Common::EventTrace
