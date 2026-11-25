#include "graphics/presentation/videoOut.h"

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/perfStats.h"
#include "common/profiler.h"
#include "common/sourceTrace.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/timer.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/gpuPhases.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/timeline.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/renderDoc.h"
#include "graphics/presentation/window/gt7DlssFrameGen.h"
#include "graphics/vr/openvrBridge.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/hmd2.h"
#include "libs/libs.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <fmt/format.h>
#include <list>
#include <string>
#include <thread>
#include <vector>

namespace Libs::Graphics {
struct GraphicContext;
} // namespace Libs::Graphics

namespace Libs::VideoOut {

LIB_NAME("VideoOut", "VideoOut");

namespace EventQueue = LibKernel::EventQueue;

constexpr int      VIDEO_OUT_EVENT_FLIP                                 = 0;
constexpr int      VIDEO_OUT_EVENT_VBLANK                               = 1;
constexpr int      VIDEO_OUT_EVENT_PRE_VBLANK_START                     = 2;
constexpr int      VIDEO_OUT_EVENT_SET_MODE                             = 8;
constexpr int      VIDEO_OUT_TRUE                                       = 1;
constexpr int      VIDEO_OUT_FALSE                                      = 0;
constexpr int      VIDEO_OUT_BUS_TYPE_MAIN                              = 0;
constexpr int      VIDEO_OUT_BUS_TYPE_OVERLAY                           = 1;
constexpr int      VIDEO_OUT_BUS_TYPE_SUB                               = 2;
constexpr int      VIDEO_OUT_BUS_TYPE_HMD                               = Hmd2::VIDEO_OUT_BUS;
constexpr int      VIDEO_OUT_FLIP_MODE_VSYNC                            = 1;
constexpr int      VIDEO_OUT_FLIP_MODE_VSYNC_MULTI                      = 4;
constexpr int      VIDEO_OUT_FLIP_MODE_SLAVE                            = 8;
constexpr int      VIDEO_OUT_FLIP_MODE_MASTER                           = 9;
constexpr int      VIDEO_OUT_BUFFER_INDEX_BLACK                         = -2;
constexpr int      VIDEO_OUT_BUFFER_INDEX_BLANK                         = -1;
constexpr int      VIDEO_OUT_BUFFER_NUM_MAX                             = 16;
constexpr size_t   VIDEO_OUT_FLIP_QUEUE_CAPACITY                        = 16;
constexpr int      VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX                   = 4;
constexpr uint64_t VIDEO_OUT_OUTPUT_MODE_DEFAULT                        = 0x0000000000000001ULL;
constexpr uint64_t VIDEO_OUT_OUTPUT_MODE_119_88HZ                       = 0x000000000000000FULL;
constexpr uint64_t VIDEO_OUT_REFRESH_RATE_59_94HZ                       = 3;
constexpr uint64_t VIDEO_OUT_REFRESH_RATE_119_88HZ                      = 13;
constexpr int      VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_UNCOMPRESSED     = 0;
constexpr int      VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_COMPRESSED       = 1;
constexpr int      VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_HMD_STEREO       = 0x10;
constexpr uint64_t VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_STRICT_COLORIMETRY = 8;
constexpr uint64_t VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_ALPHA_PREMULTIPLIED = 32;

enum class VideoOutEventKind : uintptr_t {
	Flip           = VIDEO_OUT_EVENT_FLIP,
	Vblank         = VIDEO_OUT_EVENT_VBLANK,
	PreVblankStart = VIDEO_OUT_EVENT_PRE_VBLANK_START,
	OutputMode     = VIDEO_OUT_EVENT_SET_MODE,
};

enum class FlipRequestSource { Cpu, GpuEop };

static bool PerformanceDiagnostics() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GT7_PERF_DIAGNOSTICS");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

static bool NativeAsyncPresentationRequested() {
	const char* value = std::getenv("KYTY_GT7_NATIVE_ASYNC_PRESENT");
	return value != nullptr && value[0] == '1' && value[1] == '\0' && !Config::VrEnabled() &&
	       Graphics::Gt7DlssFrameGen::Requested();
}

static uint64_t DiagnosticNowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Declare before the port lock so reporting happens after all existing reservation locks have
// been released. The queue only writes timestamps through this synchronous stack-owned record.
// Per-thread summaries avoid introducing a shared diagnostic lock into the submit path.
struct ReserveWaitDiagnostic {
	struct Summary {
		uint64_t begin_ns = 0, count = 0, slow = 0;
		std::array<uint64_t, 2> sources {};
		std::array<uint64_t, 4> sums {}, maxima {}; // driver Get, port wait, queue wait, total
	};

	const bool enabled;
	const FlipRequestSource source;
	const int index;
	const uint64_t& request_id;
	uint64_t begin_ns = 0, get_done_ns = 0, port_begin_ns = 0, port_acquired_ns = 0;
	uint64_t queue_begin_ns = 0, queue_acquired_ns = 0;

	ReserveWaitDiagnostic(FlipRequestSource request_source, int buffer_index, const uint64_t& id)
	    : enabled(PerformanceDiagnostics()), source(request_source), index(buffer_index), request_id(id) {
		if (enabled) begin_ns = DiagnosticNowNs();
	}

	~ReserveWaitDiagnostic() {
		if (!enabled) return;
		const uint64_t end_ns = DiagnosticNowNs();
		const auto elapsed = [](uint64_t begin, uint64_t end) {
			return begin != 0 && end >= begin ? end - begin : uint64_t {0};
		};
		const std::array<uint64_t, 4> durations {
		    elapsed(begin_ns, get_done_ns), elapsed(port_begin_ns, port_acquired_ns),
		    elapsed(queue_begin_ns, queue_acquired_ns), elapsed(begin_ns, end_ns)};
		static thread_local Summary summary;
		if (summary.begin_ns == 0) summary.begin_ns = begin_ns;
		++summary.count;
		++summary.sources[source == FlipRequestSource::Cpu ? 0 : 1];
		for (size_t i = 0; i < durations.size(); ++i) {
			summary.sums[i] += durations[i];
			summary.maxima[i] = std::max(summary.maxima[i], durations[i]);
		}
		const bool slow = durations[3] >= 5'000'000ull;
		summary.slow += slow;
		const auto ms = [](uint64_t ns) { return double(ns) / 1e6; };
		if (slow) {
			static std::atomic<uint64_t> slow_records {0};
			const auto ordinal = slow_records.fetch_add(1, std::memory_order_relaxed);
			if (ordinal < 256) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "GT7 perf flip reserve: sample={}/256 tid={} request={} source={} index={} "
				    "epoch_us(begin/get_done/port_begin/port_acquired/queue_begin/queue_acquired/end)="
				    "{}/{}/{}/{}/{}/{}/{} ms(get/port_wait/queue_wait/total)={:.3f}/{:.3f}/{:.3f}/{:.3f}\n",
				    ordinal + 1, Common::PerfStats::CurrentOsThreadId(), request_id,
				    source == FlipRequestSource::Cpu ? "cpu" : "gpu_eop", index,
				    begin_ns / 1000, get_done_ns / 1000, port_begin_ns / 1000,
				    port_acquired_ns / 1000, queue_begin_ns / 1000, queue_acquired_ns / 1000,
				    end_ns / 1000, ms(durations[0]), ms(durations[1]), ms(durations[2]), ms(durations[3])));
			}
		}
		if (end_ns - summary.begin_ns >= 5'000'000'000ull) {
			Log::WriteToConsoleAndLog(fmt::format(
			    "GT7 perf flip reserve summary: tid={} begin_epoch_us={} end_epoch_us={} "
			    "requests={} cpu={} gpu_eop={} slow_ge5ms={} last_request={} last_source={} last_index={} "
			    "avg_ms(get/port_wait/queue_wait/total)={:.3f}/{:.3f}/{:.3f}/{:.3f} "
			    "max_ms(get/port_wait/queue_wait/total)={:.3f}/{:.3f}/{:.3f}/{:.3f}\n",
			    Common::PerfStats::CurrentOsThreadId(), summary.begin_ns / 1000, end_ns / 1000,
			    summary.count, summary.sources[0], summary.sources[1], summary.slow,
			    request_id, source == FlipRequestSource::Cpu ? "cpu" : "gpu_eop", index,
			    ms(summary.sums[0]) / summary.count, ms(summary.sums[1]) / summary.count,
			    ms(summary.sums[2]) / summary.count, ms(summary.sums[3]) / summary.count,
			    ms(summary.maxima[0]), ms(summary.maxima[1]), ms(summary.maxima[2]), ms(summary.maxima[3])));
			summary = {};
		}
	}
};

// Only the present thread writes/reads this snapshot. Request timestamps are copied while
// holding the existing queue lock; diagnostics never retain a request pointer or take a new lock.
struct FlipDiagnostics {
	uint64_t calls = 0, no_owner = 0, not_ready = 0, not_due = 0, held = 0;
	uint64_t head_id = 0, reserved_ns = 0, preparing_ns = 0, prepared_ns = 0, ready_ns = 0;
	int head_state = -1;
	size_t queued = 0, cpu_queued = 0;
	uint64_t queue_lock_max_ns = 0, port_lock_max_ns = 0;
};

// Fixed storage, present-thread only. Bounds are inclusive; the final bin is overflow.
// No formatting, allocation or clock reads occur while accumulating a sample.
struct PacingDurationHistogram {
	static constexpr std::array<uint64_t, 14> upper_ns {
	    100'000, 250'000, 500'000, 1'000'000, 2'000'000, 4'000'000, 8'000'000,
	    12'000'000, 16'000'000, 20'000'000, 25'000'000, 33'333'000, 50'000'000, 100'000'000};
	static constexpr const char* bounds_text =
	    "100000,250000,500000,1000000,2000000,4000000,8000000,12000000,16000000,"
	    "20000000,25000000,33333000,50000000,100000000,inf";
	uint64_t count = 0, sum_ns = 0, max_ns = 0;
	std::array<uint64_t, upper_ns.size() + 1> bins {};

	void Add(uint64_t ns) {
		++count;
		sum_ns += ns;
		max_ns = std::max(max_ns, ns);
		++bins[static_cast<size_t>(std::lower_bound(upper_ns.begin(), upper_ns.end(), ns) - upper_ns.begin())];
	}

	std::string Describe() const {
		std::string counts;
		for (size_t i = 0; i < bins.size(); ++i) {
			if (i != 0) counts += ',';
			counts += fmt::format("{}", bins[i]);
		}
		return fmt::format("n:{};sum_ns:{};max_ns:{};bins:[{}]", count, sum_ns, max_ns, counts);
	}
};

// These windows classify admitted groups by host Present completion, not by guest simulation
// time or physical display time. Earliest request/interval endpoints are retained so an analyst
// can exclude a window crossing an active/standby boundary. All records are value copies.
struct FlipPacingDiagnostics {
	uint64_t begin_ns = 0, groups = 0, requests = 0, held = 0;
	uint64_t earliest_ready_ns = 0, earliest_prepared_ns = 0, interval_begin_ns = 0;
	uint64_t missing_ready = 0, missing_prepared = 0, previous_present_begin_ns = 0;
	std::array<uint64_t, 2> sources {};
	PacingDurationHistogram ready_to_admit, prepared_to_ready, host_present, host_present_interval;

	void RecordRequest(uint64_t prepared_ns, uint64_t ready_ns, uint64_t admit_ns,
	                   FlipRequestSource source, bool hold) {
		if (begin_ns == 0) begin_ns = admit_ns;
		++requests;
		++sources[source == FlipRequestSource::Cpu ? 0 : 1];
		held += hold;
		if (ready_ns != 0 && ready_ns <= admit_ns) {
			ready_to_admit.Add(admit_ns - ready_ns);
			if (earliest_ready_ns == 0 || ready_ns < earliest_ready_ns) earliest_ready_ns = ready_ns;
		} else {
			++missing_ready;
		}
		if (prepared_ns != 0 && ready_ns >= prepared_ns) {
			prepared_to_ready.Add(ready_ns - prepared_ns);
			if (earliest_prepared_ns == 0 || prepared_ns < earliest_prepared_ns) earliest_prepared_ns = prepared_ns;
		} else {
			++missing_prepared;
		}
	}

	// Called only after all queue/port locks are released. Present may already have released a
	// held incoming frame; deliberately take no Frame/Request/port pointer here.
	void RecordPresent(uint64_t group, uint64_t present_begin_ns, uint64_t present_end_ns) {
		if (begin_ns == 0) begin_ns = present_begin_ns;
		++groups;
		host_present.Add(present_end_ns - present_begin_ns);
		if (previous_present_begin_ns != 0) {
			host_present_interval.Add(present_begin_ns - previous_present_begin_ns);
			if (interval_begin_ns == 0) interval_begin_ns = previous_present_begin_ns;
		}
		previous_present_begin_ns = present_begin_ns;
		if (present_end_ns - begin_ns < 5'000'000'000ull) return;
		Log::WriteToConsoleAndLog(fmt::format(
		    "GT7 perf flip pacing: clock=steady_qpc window_begin_epoch_ns={} window_end_epoch_ns={} "
		    "qpc_frequency={} groups={} requests={} cpu={} gpu_eop={} held={} last_group={} "
		    "earliest_ready_epoch_ns={} earliest_prepared_epoch_ns={} interval_begin_epoch_ns={} "
		    "last_present_begin_epoch_ns={} missing_ready={} missing_prepared={} bin_upper_ns=[{}] "
		    "ready_to_admit={{{}}} prepared_to_ready={{{}}} host_present_call={{{}}} "
		    "host_present_begin_interval={{{}}}\n",
		    begin_ns, present_end_ns, Common::Timer::QueryPerformanceFrequency(), groups, requests,
		    sources[0], sources[1], held, group, earliest_ready_ns, earliest_prepared_ns, interval_begin_ns,
		    present_begin_ns, missing_ready, missing_prepared, PacingDurationHistogram::bounds_text,
		    ready_to_admit.Describe(), prepared_to_ready.Describe(), host_present.Describe(),
		    host_present_interval.Describe()));
		// Keep interval continuity across reports, but not samples/counts from the prior window.
		*this = {};
		begin_ns = present_end_ns;
		previous_present_begin_ns = present_begin_ns;
	}
};

struct VblankPacingDiagnostics {
	uint64_t begin_ns = 0, begin_qpc = 0, loops = 0, presented_loops = 0;
	uint64_t late_loops = 0, debt_loops = 0, no_sleep_loops = 0;
	uint64_t previous_end_ns = 0, spacing_begin_ns = 0, min_period_qpc = 0, max_period_qpc = 0;
	PacingDurationHistogram end_spacing, wake_lateness, end_debt;

	// The debt is the unchanged scheduler accumulator, not an inferred GPU/display deadline.
	// Record runs after VblankEnd has released every port lock. Paused loops are excluded.
	void Record(uint64_t loop_begin_ns, uint64_t loop_begin_qpc, uint64_t end_ns, uint64_t end_qpc,
	            uint64_t frequency, uint64_t period, int64_t wake_balance, int64_t end_balance,
	            bool no_sleep, bool presented) {
		if (begin_ns == 0) {
			begin_ns = loop_begin_ns;
			begin_qpc = loop_begin_qpc;
		}
		++loops;
		presented_loops += presented;
		no_sleep_loops += no_sleep;
		late_loops += wake_balance < 0;
		debt_loops += end_balance < 0;
		if (min_period_qpc == 0 || period < min_period_qpc) min_period_qpc = period;
		max_period_qpc = std::max(max_period_qpc, period);
		const auto debt_ns = [frequency](int64_t balance) -> uint64_t {
			if (balance >= 0) return uint64_t {0};
			const uint64_t ticks = static_cast<uint64_t>(-(balance + 1)) + 1;
			return (ticks / frequency) * 1'000'000'000ull +
			       ((ticks % frequency) * 1'000'000'000ull) / frequency;
		};
		wake_lateness.Add(debt_ns(wake_balance));
		end_debt.Add(debt_ns(end_balance));
		if (previous_end_ns != 0) {
			end_spacing.Add(end_ns - previous_end_ns);
			if (spacing_begin_ns == 0) spacing_begin_ns = previous_end_ns;
		}
		previous_end_ns = end_ns;
		if (end_ns - begin_ns < 5'000'000'000ull) return;
		Log::WriteToConsoleAndLog(fmt::format(
		    "GT7 perf vblank pacing: clock=steady_qpc window_begin_epoch_ns={} window_end_epoch_ns={} "
		    "window_begin_qpc={} window_end_qpc={} qpc_frequency={} spacing_begin_epoch_ns={} "
		    "loops={} presented_loops={} late_wake_loops={} end_debt_loops={} no_sleep_loops={} "
		    "period_qpc_min={} period_qpc_max={} bin_upper_ns=[{}] vblank_end_spacing={{{}}} "
		    "wake_lateness={{{}}} end_deadline_debt={{{}}}\n",
		    begin_ns, end_ns, begin_qpc, end_qpc, frequency, spacing_begin_ns, loops, presented_loops,
		    late_loops, debt_loops, no_sleep_loops, min_period_qpc, max_period_qpc,
		    PacingDurationHistogram::bounds_text, end_spacing.Describe(), wake_lateness.Describe(), end_debt.Describe()));
		*this = {};
		begin_ns = end_ns;
		begin_qpc = end_qpc;
		previous_end_ns = end_ns;
	}
};

struct PreparationDiagnostic {
	uint64_t request = 0, group = 0, generation = 0;
	uint64_t begin_ns = 0, end_ns = 0, address = 0, size = 0;
	FlipRequestSource source = FlipRequestSource::Cpu;
	int bus = 0, index = 0;
	uint32_t width = 0, height = 0, format = 0;
	bool special = false, hmd = false, hold = false, has_frame = false;
};

// Only Prepare(), on the serial guest GPU lane, accesses these counters. Capture values while
// their existing locks are held; never retain a request, port, image or frame pointer here.
// Full intervals let the renderer's snapshot_epoch_us identify its exact originating flip,
// including rejected FG inputs, without changing FrameInputs or presentation ownership.
struct PreparationDiagnostics {
	static constexpr uint64_t detail_limit = 20'000;
	static constexpr uint64_t extra_anomaly_limit = 128;
	uint64_t total = 0, details = 0, extra_anomalies = 0, last_report_ns = 0;
	std::array<std::array<uint64_t, 2>, 5> buses {}; // main, overlay, sub, hmd, other; CPU/GPU-EOP
	uint64_t special = 0, held = 0, no_frame = 0, dropped = 0;

	void Record(const PreparationDiagnostic& d) {
		if (last_report_ns == 0) last_report_ns = d.begin_ns;
		++total;
		const size_t bus_slot = d.bus == VIDEO_OUT_BUS_TYPE_MAIN ? 0 :
		                        d.bus == VIDEO_OUT_BUS_TYPE_OVERLAY ? 1 :
		                        d.bus == VIDEO_OUT_BUS_TYPE_SUB ? 2 :
		                        d.bus == VIDEO_OUT_BUS_TYPE_HMD ? 3 : 4;
		++buses[bus_slot][d.source == FlipRequestSource::Cpu ? 0 : 1];
		special += d.special;
		held += d.hold;
		no_frame += !d.has_frame;
		const bool anomaly = d.bus != VIDEO_OUT_BUS_TYPE_MAIN || d.special || !d.has_frame;
		const bool full_detail = details < detail_limit;
		if (full_detail || (anomaly && extra_anomalies < extra_anomaly_limit)) {
			if (full_detail) ++details;
			else ++extra_anomalies;
			const char* route = d.hmd ? "hmd" : !d.special ? "frame" :
			                    d.index == VIDEO_OUT_BUFFER_INDEX_BLACK ? "black" : "blank";
			Log::WriteToConsoleAndLog(fmt::format(
			    "GT7 perf flip prepare: seq={} request={} group_at_prepare={} generation={} "
			    "source={} bus={} index={} route={} begin_epoch_us={} end_epoch_us={} "
			    "address=0x{:x} size={} format={} extent={}x{} hold={} frame={}\n",
			    total, d.request, d.group, d.generation,
			    d.source == FlipRequestSource::Cpu ? "cpu" : "gpu_eop", d.bus, d.index, route,
			    d.begin_ns / 1000, d.end_ns / 1000, d.address, d.size, d.format,
			    d.width, d.height, d.hold ? 1 : 0, d.has_frame ? 1 : 0));
			if (full_detail && details == detail_limit) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "GT7 perf flip prepare: budget_exhausted=1 detail_limit={}; only up to {} additional "
				    "non-main/blank/frameless records and periodic counts remain\n",
				    detail_limit, extra_anomaly_limit));
			}
		} else {
			++dropped;
		}
		if (d.end_ns - last_report_ns >= 5'000'000'000ull) {
			Log::WriteToConsoleAndLog(fmt::format(
			    "GT7 perf flip prepare summary: begin_epoch_us={} end_epoch_us={} total={} "
			    "cpu/gpu_eop(main/overlay/sub/hmd/other)={}/{},{}/{},{}/{},{}/{},{}/{} "
			    "special={} held={} no_frame={} dropped={} details={}/{} extra_anomalies={}/{}\n",
			    last_report_ns / 1000, d.end_ns / 1000, total,
			    buses[0][0], buses[0][1], buses[1][0], buses[1][1], buses[2][0], buses[2][1],
			    buses[3][0], buses[3][1], buses[4][0], buses[4][1], special, held, no_frame, dropped,
			    details, detail_limit, extra_anomalies, extra_anomaly_limit));
			last_report_ns = d.end_ns;
			buses = {};
			special = held = no_frame = dropped = 0;
		}
	}
};

struct VideoOutEventState;

struct VideoOutEventRegistration {
	EventQueue::KernelEqueue            handle = EventQueue::KERNEL_EQUEUE_INVALID;
	std::shared_ptr<VideoOutEventState> state;
	uint64_t                            generation = 0;
	VideoOutEventKind                   kind       = VideoOutEventKind::Flip;
};

using VideoOutEventRegistrationRef = std::shared_ptr<VideoOutEventRegistration>;
using VideoOutEventQueues          = std::vector<VideoOutEventRegistrationRef>;

struct VideoOutEventState {
	Common::Mutex       mutex;
	VideoOutEventQueues flip;
	VideoOutEventQueues pre_vblank;
	VideoOutEventQueues vblank;
	VideoOutEventQueues output_mode;
};

struct VideoOutBufferAttribute2 {
	uint32_t reserved0;
	uint32_t tiling_mode;
	uint32_t aspect_ratio;
	uint32_t width;
	uint32_t height;
	uint32_t pitch_in_pixel;
	uint64_t option;
	uint64_t pixel_format;
	uint64_t dcc_cb_register_clear_color;
	uint32_t dcc_control;
	uint32_t pad0;
	uint64_t reserved1[3];
};

// PS5 layout
struct VideoOutFlipStatus {
	uint64_t count                    = 0;
	uint64_t processTime              = 0;
	uint64_t reserved0                = 0;
	int64_t  flipArg                  = 0;
	uint64_t reserved1                = 0;
	uint64_t processTimeCounter       = 0;
	int32_t  gcQueueNum               = 0;
	int32_t  flipPendingNum           = 0;
	int32_t  currentBuffer            = 0;
	uint32_t reserved2                = 0;
	uint64_t submitProcessTimeCounter = 0;
	uint64_t reserved3[7]             = {};
};

// PS5 layout
struct VideoOutVblankStatus {
	uint64_t count              = 0;
	uint64_t processTime        = 0;
	uint64_t reserved           = 0;
	uint64_t processTimeCounter = 0;
	uint8_t  flags              = 0;
	uint8_t  phase              = 0;
	uint8_t  pad1[6]            = {};
};

struct VideoOutOutputStatus {
	uint32_t resolution   = 0;
	uint32_t dynamicRange = 0;
	uint64_t refreshRate  = 0;
	uint64_t flags        = 0;
	uint64_t reserved[3]  = {};
};

struct VideoOutOutputOptions {
	uint32_t internalData[16] = {};
};

struct VideoOutColorSettings {
	float    gamma       = 1.0f;
	uint32_t reserved[3] = {};
};

struct VideoOutBuffers {
	const void* data;
	const void* metadata;
	const void* reserved[2];
};

struct VideoOutBuffer {
	int      group_index      = -1;
	uint64_t data_address     = 0;
	uint64_t metadata_address = 0;

	[[nodiscard]] bool Occupied() const noexcept { return group_index >= 0; }
};

struct BufferAttributeGroup {
	VideoOutBufferAttribute2 attribute {};
	int                      category = VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_UNCOMPRESSED;
	bool                     occupied = false;

	[[nodiscard]] Graphics::ImageInfo ImageInfo(const VideoOutBuffer& buffer) const;
};

struct VideoOutConfig {
	Common::Mutex                       mutex;
	Common::CondVar                     vblank_cond;
	std::shared_ptr<VideoOutEventState> events      = std::make_shared<VideoOutEventState>();
	uint32_t                            width       = 0;
	uint32_t                            height      = 0;
	uint64_t                            generation  = 0;
	bool                                opened      = false;
	bool                                closing     = false;
	int                                 bus         = 0;
	VideoOutConfig*                     master      = nullptr;
	uint32_t                            slave_count = 0;
	int                                 flip_rate   = 0;
	// Experimental host admission and guest publication are distinct, generation-scoped epochs.
	// Only access while holding this port's mutex; Open resets all three together.
	uint64_t                            async_epoch_generation = 0;
	uint64_t                            async_admitted_epoch = 0;
	uint64_t                            async_published_epoch = 0;
	uint64_t                            output_mode = VIDEO_OUT_OUTPUT_MODE_DEFAULT;
	float                               gamma       = 1.0f;
	VideoOutFlipStatus                  flip_status;
	VideoOutVblankStatus                pre_vblank_status;
	VideoOutVblankStatus                vblank_status;
	std::array<VideoOutBuffer, VIDEO_OUT_BUFFER_NUM_MAX>                 buffers;
	std::array<BufferAttributeGroup, VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX> groups;
};

class FlipQueue {
public:
	explicit FlipQueue(Graphics::Presenter& presenter): m_presenter(presenter) {
		EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	}
	~FlipQueue();
	KYTY_CLASS_NO_COPY(FlipQueue);

	bool Reserve(VideoOutConfig& cfg, int index, int64_t flip_arg, FlipRequestSource source,
	             uint64_t& request_id, ReserveWaitDiagnostic* diagnostic);
	void Cancel(VideoOutConfig& cfg);
	void Prepare(uint64_t request_id, Graphics::CommandBuffer& buffer);
	void Complete(uint64_t request_id);
	void WaitForSubmitSlot(VideoOutConfig& cfg);
	bool Flip(uint32_t micros, uint64_t* presented_buses = nullptr, bool async_host = false);
	void BeginAsyncTick(bool paused);
	void EndAsyncTick(bool paused, uint32_t width, uint32_t height, bool any_open);
	bool PublishCompleted();
	uint64_t AsyncRevision();
	void WaitAsyncEvent(uint64_t revision, std::stop_token token);
	void WakeAsync();
	bool TakeAsyncRefresh(uint32_t& width, uint32_t& height, bool& paused, bool& any_open);
	void GetFlipStatus(VideoOutConfig& cfg, VideoOutFlipStatus& out);
	void Wait(VideoOutConfig& cfg, int index);
	const FlipDiagnostics& Diagnostics() const { return m_diagnostics; }
	void ResetDiagnostics() { m_diagnostics = {}; }

private:
	enum class RequestState { Reserved, Recording, Ready, Presenting, CompletedAwaitVblank };

	struct Request {
		uint64_t                    id;
		uint64_t                    group;
		VideoOutConfig*             cfg;
		VideoOutConfig*             master;
		uint64_t                    generation;
		int                         bus; // Immutable identity captured under cfg.mutex in Reserve.
		int                         index;
		int64_t                     flip_arg;
		uint64_t                    submit_ptc;
		FlipRequestSource           source;
		RequestState                state;
		Graphics::Presenter::Frame* frame;
		bool                        premultiplied_alpha;
		// Round 133: the main display keeps the frame it shows (GpuPhases::TakeFrameHold).
		bool hold = false;
		bool                        hmd_output;
		uint64_t diagnostic_reserved_ns = 0, diagnostic_preparing_ns = 0;
		uint64_t diagnostic_prepared_ns = 0, diagnostic_ready_ns = 0;
	};

	Graphics::Presenter& m_presenter;
	Common::Mutex        m_mutex;
	Common::CondVar      m_submit_cond_var;
	Common::CondVar      m_submit_slot_cond_var;
	Common::CondVar      m_done_cond_var;
	std::list<Request>   m_requests;
	std::list<Request>   m_cpu_requests;
	std::list<Request>   m_cancelled_requests;
	bool                 m_processing      = false;
	uint64_t             m_next_request_id = 1;
	FlipDiagnostics      m_diagnostics;
	FlipPacingDiagnostics m_pacing_diagnostics;
	PreparationDiagnostics m_preparation_diagnostics;
	// Queue mutex protects the revision, complete tick snapshot and refresh mailbox. The host
	// worker never waits for publication; Close can cancel completed metadata without the clock.
	uint64_t m_async_revision = 0, m_async_epoch = 0, m_async_last_host_epoch = 0;
	bool m_async_tick_in_progress = true, m_async_paused = true, m_async_refresh = false;
	bool m_async_enabled = false;
	bool m_async_any_open = false;
	uint32_t m_async_width = 0, m_async_height = 0;
};

struct VideoOutDriver::Impl {
public:
	static constexpr std::array VIDEO_OUT_BUSES {
	    VIDEO_OUT_BUS_TYPE_MAIN, VIDEO_OUT_BUS_TYPE_OVERLAY, VIDEO_OUT_BUS_TYPE_SUB,
	    VIDEO_OUT_BUS_TYPE_HMD};
	static constexpr int VIDEO_OUT_NUM_MAX = VIDEO_OUT_BUSES.size() + 1;

	Impl(uint32_t width, uint32_t height, Graphics::Presenter& presenter)
	    : m_renderer(presenter.Renderer()), m_presenter(presenter), m_flip_queue(presenter) {
		EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
		Init(width, height);
		m_async_present = NativeAsyncPresentationRequested();
		if (m_async_present) {
			Log::WriteToConsoleAndLog("GT7 experimental native presentation: Ready-driven host worker; independent guest Vblank clock\n");
		}
		// Start the bounded-wait clock first. If creation of the host thread throws, member
		// destruction can stop/join this clock without the Impl destructor's wake path.
		m_present_thread = std::jthread([this](std::stop_token token) {
			PresentThread(token);
			m_present_thread_exited.store(true, std::memory_order_release);
		});
		if (m_async_present) {
			m_async_host_thread_exited.store(false, std::memory_order_relaxed);
			m_async_host_thread = std::jthread([this](std::stop_token token) {
				AsyncHostThread(token);
				m_async_host_thread_exited.store(true, std::memory_order_release);
			});
		}
	}
	~Impl();
	KYTY_CLASS_NO_COPY(Impl);

	int             Open(int bus_type, int index);
	bool            Close(int handle);
	VideoOutConfig* Get(int handle);
	VideoOutConfig* Get(int handle, uint64_t& generation);
	bool            IsOpened(int handle);
	int             SetFlipMaster(int slave_handle, int master_handle);

	void                     Init(uint32_t width, uint32_t height);
	FlipQueue&               GetFlipQueue() { return m_flip_queue; }
	Graphics::RenderContext& Renderer() const noexcept { return m_renderer; }

	void VblankBegin();
	void VblankEnd();
	void PresentThread(std::stop_token token);
	void AsyncClockThread(std::stop_token token);
	bool AsyncClockTick(bool paused);
	void AsyncHostThread(std::stop_token token);

private:
	Common::Mutex            m_mutex;
	VideoOutConfig           m_video_out_ctx[VIDEO_OUT_NUM_MAX];
	Graphics::RenderContext& m_renderer;
	Graphics::Presenter&     m_presenter;
	FlipQueue                m_flip_queue;
	std::atomic<bool>        m_present_thread_exited {false};
	bool                    m_async_present = false;
	Common::Mutex           m_async_clock_mutex;
	Common::CondVar         m_async_clock_wake;
	std::atomic<bool>        m_async_host_thread_exited {true};
	std::jthread            m_async_host_thread;
	std::jthread             m_present_thread;
};

static std::unique_ptr<VideoOutDriver> g_video_out_driver;

static VideoOutDriver::Impl& DriverState() {
	EXIT_IF(g_video_out_driver == nullptr);
	return g_video_out_driver->State();
}

static uintptr_t VideoOutEventId(VideoOutEventKind kind) {
	return static_cast<uintptr_t>(kind);
}

static VideoOutEventQueues& VideoOutEventQueuesFor(VideoOutEventState& state,
                                                   VideoOutEventKind   kind) {
	switch (kind) {
		case VideoOutEventKind::Flip: return state.flip;
		case VideoOutEventKind::Vblank: return state.vblank;
		case VideoOutEventKind::PreVblankStart: return state.pre_vblank;
		case VideoOutEventKind::OutputMode: return state.output_mode;
	}
	EXIT("unsupported video-out event kind\n");
	return state.flip;
}

static intptr_t MakeVideoOutEventData(intptr_t current_data, void* trigger_data) {
	const uint64_t old_data = static_cast<uint64_t>(current_data);
	uint64_t       counter  = (old_data >> 12u) & 0xfu;
	if (counter != 0xfu) {
		counter++;
	}

	const uint64_t time    = LibKernel::KernelReadTsc() & 0xfffu;
	const uint64_t payload = static_cast<uint64_t>(reinterpret_cast<intptr_t>(trigger_data));

	return static_cast<intptr_t>(time | (counter << 12u) |
	                             ((payload & 0x0000ffffffffffffULL) << 16u));
}

static void ResetVideoOutEvent(EventQueue::KernelEqueueEvent* event) {
	EXIT_IF(event == nullptr);
	event->triggered    = false;
	event->event.fflags = 0;
	event->event.data   = 0;
}

static void TriggerVideoOutEvent(EventQueue::KernelEqueueEvent* event, void* trigger_data) {
	EXIT_IF(event == nullptr);

	auto triggered_event = event->event;
	triggered_event.fflags =
	    triggered_event.fflags < 0xfu ? triggered_event.fflags + 1u : triggered_event.fflags;
	triggered_event.data = MakeVideoOutEventData(triggered_event.data, trigger_data);
	if (event->triggered) {
		event->pending_events.push_back(triggered_event);
		return;
	}
	event->event     = triggered_event;
	event->triggered = true;
}

static void RemoveVideoOutEventQueue(EventQueue::KernelEqueue       eq,
                                     EventQueue::KernelEqueueEvent* event) {
	if (event == nullptr || event->filter.data == nullptr) {
		return;
	}

	auto* registration = static_cast<VideoOutEventRegistration*>(event->filter.data);
	auto  state        = registration->state;
	if (registration->handle != eq || !state) {
		return;
	}
	auto&             queues = VideoOutEventQueuesFor(*state, registration->kind);
	Common::LockGuard lock(state->mutex);
	const auto        entry =
	    std::find_if(queues.begin(), queues.end(), [registration](const auto& candidate) {
		    return candidate.get() == registration;
	    });
	if (entry != queues.end()) {
		queues.erase(entry);
	}
}

static void TriggerVideoOutEvents(VideoOutConfig& video_out, VideoOutEventKind kind,
                                  void* trigger_data) {
	VideoOutEventQueues queues;
	{
		Common::LockGuard lock(video_out.events->mutex);
		queues = VideoOutEventQueuesFor(*video_out.events, kind);
	}
	for (const auto& registration: queues) {
		if (!registration || registration->generation != video_out.generation) {
			continue;
		}
		const auto result =
		    EventQueue::KernelTriggerEvent(registration->handle, VideoOutEventId(kind),
		                                   EventQueue::KERNEL_EVFILT_VIDEO_OUT, trigger_data);
		EXIT_NOT_IMPLEMENTED(result != OK && result != LibKernel::KERNEL_ERROR_EBADF &&
		                     result != LibKernel::KERNEL_ERROR_ENOENT);
	}
}

static void DeleteVideoOutEvents(const VideoOutEventQueues& queues, VideoOutEventKind kind) {
	for (const auto& registration: queues) {
		if (!registration) {
			continue;
		}
		const auto result = EventQueue::KernelDeleteEvent(
		    registration->handle, VideoOutEventId(kind), EventQueue::KERNEL_EVFILT_VIDEO_OUT);
		EXIT_NOT_IMPLEMENTED(result != OK && result != LibKernel::KERNEL_ERROR_EBADF &&
		                     result != LibKernel::KERNEL_ERROR_ENOENT);
	}
}

static int RegisterVideoOutEvent(int handle, EventQueue::KernelEqueue eq, VideoOutEventKind kind,
                                 void* udata) {
	uint64_t generation = 0;
	auto*    video_out  = DriverState().Get(handle, generation);
	if (video_out == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	Common::LockGuard lock(video_out->mutex);
	if (!video_out->opened || video_out->closing || video_out->generation != generation) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	if (kind == VideoOutEventKind::OutputMode) {
		LOGF("\t eq     = 0x%016" PRIx64 "\n"
		     "\t handle = %d\n"
		     "\t udata  = 0x%016" PRIx64 "\n",
		     static_cast<uint64_t>(eq), handle, reinterpret_cast<uint64_t>(udata));
	}
	if (eq == EventQueue::KERNEL_EQUEUE_INVALID) {
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}
	if (!EventQueue::KernelPinEqueue(eq)) {
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}
	auto        event_state         = video_out->events;
	auto&       queues              = VideoOutEventQueuesFor(*event_state, kind);
	const bool  initially_triggered = kind == VideoOutEventKind::OutputMode;
	void* const initial_trigger_data =
	    initially_triggered ? reinterpret_cast<void*>(video_out->output_mode) : nullptr;

	EventQueue::KernelEqueueEvent event {};
	event.triggered    = initially_triggered;
	event.event.ident  = VideoOutEventId(kind);
	event.event.filter = EventQueue::KERNEL_EVFILT_VIDEO_OUT;
	event.event.udata  = udata;
	event.event.fflags = initially_triggered ? 1u : 0u;
	event.event.data   = initially_triggered ? MakeVideoOutEventData(0, initial_trigger_data) : 0;
	event.filter.delete_event_func = RemoveVideoOutEventQueue;
	event.filter.reset_func        = ResetVideoOutEvent;
	event.filter.trigger_func      = TriggerVideoOutEvent;

	VideoOutEventRegistrationRef registration;
	bool                         add_queue = false;
	{
		Common::LockGuard event_lock(event_state->mutex);
		const auto        existing =
		    std::find_if(queues.begin(), queues.end(), [&](const auto& candidate) {
			    return candidate->handle == eq && candidate->generation == generation;
		    });
		if (existing != queues.end()) {
			registration = *existing;
		} else {
			registration = std::make_shared<VideoOutEventRegistration>(VideoOutEventRegistration {
			    .handle = eq, .state = event_state, .generation = generation, .kind = kind});
			queues.push_back(registration);
			add_queue = true;
		}
	}
	event.filter.data  = registration.get();
	event.filter.owner = registration;
	const int result   = EventQueue::KernelAddEvent(eq, event);
	if (result != OK && add_queue) {
		Common::LockGuard event_lock(event_state->mutex);
		const auto        added = std::find(queues.begin(), queues.end(), registration);
		if (added != queues.end()) {
			queues.erase(added);
		}
	}
	return result == LibKernel::KERNEL_ERROR_EBADF ? VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE : result;
}

static int DeleteVideoOutEvent(int handle, EventQueue::KernelEqueue eq, VideoOutEventKind kind) {
	uint64_t generation = 0;
	auto*    video_out  = DriverState().Get(handle, generation);
	if (video_out == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	Common::LockGuard lock(video_out->mutex);
	if (!video_out->opened || video_out->closing || video_out->generation != generation) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	if (!EventQueue::KernelPinEqueue(eq)) {
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}
	const int result = EventQueue::KernelDeleteEvent(eq, VideoOutEventId(kind),
	                                                 EventQueue::KERNEL_EVFILT_VIDEO_OUT);
	if (result == LibKernel::KERNEL_ERROR_EBADF) {
		return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
	}
	return result == LibKernel::KERNEL_ERROR_ENOENT ? OK : result;
}

static bool IsFlipDueLocked(const VideoOutConfig& cfg, uint64_t generation) {
	if (!cfg.opened || cfg.closing || cfg.generation != generation) {
		return false;
	}
	const int interval = cfg.flip_rate + 1;

	return interval <= 1 || (cfg.vblank_status.count % static_cast<uint64_t>(interval)) == 0;
}

static bool IsValidBufferIndex(int index) {
	return index >= VIDEO_OUT_BUFFER_INDEX_BLACK && index < VIDEO_OUT_BUFFER_NUM_MAX;
}

static bool IsSpecialBufferIndex(int index) {
	return index == VIDEO_OUT_BUFFER_INDEX_BLANK || index == VIDEO_OUT_BUFFER_INDEX_BLACK;
}

static bool IsValidFlipMode(int mode) {
	return (mode >= VIDEO_OUT_FLIP_MODE_VSYNC && mode <= VIDEO_OUT_FLIP_MODE_VSYNC_MULTI) ||
	       mode == VIDEO_OUT_FLIP_MODE_SLAVE || mode == VIDEO_OUT_FLIP_MODE_MASTER;
}

static int ReserveFlipRequest(VideoOutDriver::Impl& driver, int handle, int index, int flip_mode,
                              int64_t flip_arg, FlipRequestSource source, uint64_t& request_id) {
	ReserveWaitDiagnostic diagnostic(source, index, request_id);
	auto* video_out = driver.Get(handle);
	if (diagnostic.enabled) diagnostic.get_done_ns = DiagnosticNowNs();
	if (video_out == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	if (!IsValidFlipMode(flip_mode)) {
		return VIDEO_OUT_ERROR_INVALID_FLIP_MODE;
	}
	if (!IsValidBufferIndex(index)) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}

	if (diagnostic.enabled) diagnostic.port_begin_ns = DiagnosticNowNs();
	Common::LockGuard lock(video_out->mutex);
	if (diagnostic.enabled) diagnostic.port_acquired_ns = DiagnosticNowNs();
	if ((video_out->master != nullptr) != (flip_mode == VIDEO_OUT_FLIP_MODE_SLAVE) ||
	    (video_out->slave_count != 0) != (flip_mode == VIDEO_OUT_FLIP_MODE_MASTER)) {
		return VIDEO_OUT_ERROR_INVALID_FLIP_MODE;
	}
	if (video_out->closing ||
	    (!IsSpecialBufferIndex(index) && !video_out->buffers[index].Occupied())) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}
	if (!driver.GetFlipQueue().Reserve(*video_out, index, flip_arg, source, request_id,
	                                  diagnostic.enabled ? &diagnostic : nullptr)) {
		return VIDEO_OUT_ERROR_FLIP_QUEUE_FULL;
	}
	return OK;
}

Graphics::ImageInfo BufferAttributeGroup::ImageInfo(const VideoOutBuffer& buffer) const {
	const auto compression = Graphics::ClassifyVideoOutCompression(
	    category == VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_COMPRESSED, buffer.metadata_address,
	    attribute.dcc_control, attribute.dcc_cb_register_clear_color);
	if (attribute.reserved0 != 0 || attribute.aspect_ratio != 0 || attribute.width == 0 ||
	    attribute.height == 0 || attribute.width > 16384 || attribute.height > 16384 ||
	    attribute.pitch_in_pixel != 0 ||
	    (attribute.option & ~(VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_STRICT_COLORIMETRY |
	                          VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_ALPHA_PREMULTIPLIED)) != 0 ||
	    attribute.tiling_mode != 0 || attribute.pad0 != 0 || attribute.reserved1[0] != 0 ||
	    attribute.reserved1[1] != 0 || attribute.reserved1[2] != 0 || buffer.data_address == 0 ||
	    compression == Graphics::VideoOutCompression::Unsupported) {
		// Round 147: what was not accepted, for the report.
		EXIT("unsupported or invalid video-out surface attributes: %ux%u, category %d, DCC control "
		     "0x%08x%s, data 0x%" PRIx64 ", metadata 0x%" PRIx64 "\n",
		     attribute.width, attribute.height, category, attribute.dcc_control,
		     compression == Graphics::VideoOutCompression::Unsupported ? " (not supported)" : "",
		     buffer.data_address, buffer.metadata_address);
	}
	Graphics::VideoOutPixelFormatInfo pixel_format {};
	if (!Graphics::DecodeVideoOutPixelFormat(attribute.pixel_format, pixel_format)) {
		EXIT("unsupported video-out pixel format: 0x%016" PRIx64 "\n", attribute.pixel_format);
	}
	if (attribute.pixel_format == Graphics::VIDEO_OUT_PIXEL_FORMAT_R10_G10_B10_A2_BT2100_PQ) {
		static std::atomic_flag warned = ATOMIC_FLAG_INIT;
		if (!warned.test_and_set(std::memory_order_relaxed)) {
			Log::WriteToConsoleAndLog(fmt::format(
			    "Warning: HDR-to-SDR conversion is not implemented; displaying PQ video-out "
			    "pixels unchanged (format=0x{:016x}).\n", attribute.pixel_format));
		}
	}
	const auto tile_mode = Graphics::Prospero::TileMode::kRenderTarget;
	const auto pitch =
	    Graphics::TileGetTexturePitch(pixel_format.guest_format, attribute.width, tile_mode);
	Graphics::TileSizeAlign total {};
	Graphics::TileGetTextureTotalSize(pixel_format.guest_format, attribute.width, attribute.height,
	                                  1, 1, tile_mode, false, total);
	if (total.size == 0 || total.align != 65536 ||
	    (buffer.data_address & (total.align - 1u)) != 0) {
		EXIT("invalid video-out surface footprint or alignment\n");
	}
	Graphics::ImageInfo info {};
	info.data            = {buffer.data_address, total.size};
	info.pixel_format    = pixel_format.format;
	info.guest_format    = pixel_format.guest_format;
	info.type            = Graphics::Prospero::ImageType::kColor2D;
	info.extent          = {attribute.width, attribute.height, 1};
	info.resources       = {1, 1};
	info.pitch           = pitch;
	info.bytes_per_block = pixel_format.bytes_per_element;
	info.samples         = 1;
	info.tile_mode       = tile_mode;
	info.bgra16          = pixel_format.bgra16;
	info.mip_layout[0]   = {0, total.size, pitch, attribute.height};
	if (compression != Graphics::VideoOutCompression::Uncompressed) {
		Graphics::TileSizeAlign dcc_size {};
		if (!Graphics::TileGetDccSize(attribute.width, attribute.height, 1,
		                              pixel_format.bytes_per_element, 1, tile_mode, dcc_size)) {
			EXIT("invalid video-out DCC footprint\n");
		}
		info.metadata.range       = {buffer.metadata_address, dcc_size.size};
		info.metadata.kind        = Graphics::ImageMetadataKind::Dcc;
		info.metadata.control     = attribute.dcc_control;
		info.metadata.compression = compression;
	}
	Graphics::ImageOps::Validate(info);
	if (!Graphics::IsSupportedVideoOutFormat(info)) {
		EXIT("unsupported normalized video-out format\n");
	}
	return info;
}

VideoOutDriver::VideoOutDriver(uint32_t width, uint32_t height, Graphics::Presenter& presenter)
    : m_impl(std::make_unique<Impl>(width, height, presenter)) {}

VideoOutDriver::~VideoOutDriver() = default;

VideoOutDriver::Impl& VideoOutDriver::State() noexcept {
	return *m_impl;
}

VideoOutDriver& VideoOutInit(uint32_t width, uint32_t height, Graphics::Presenter& presenter) {
	EXIT_IF(g_video_out_driver != nullptr);
	g_video_out_driver = std::make_unique<VideoOutDriver>(width, height, presenter);
	return *g_video_out_driver;
}

void VideoOutShutdown() {
	g_video_out_driver.reset();
}

VideoOutDriver::Impl::~Impl() {
	if (m_async_host_thread.joinable()) m_async_host_thread.request_stop();
	if (m_present_thread.joinable()) {
		m_present_thread.request_stop();
		if (m_async_present) {
			m_flip_queue.WakeAsync();
			Common::LockGuard clock_lock(m_async_clock_mutex);
			m_async_clock_wake.SignalAll();
		}
		// Present's title update waits for SDL_RunOnMainThread(..., true). The main
		// event loop may never have run (graphics check), or may already have ended.
		// Keep servicing its callbacks while the presenter finishes, with the window
		// and all graphics resources still alive. Do not detach or skip normal cleanup.
		if (SDL_IsMainThread()) {
			while (!m_present_thread_exited.load(std::memory_order_acquire) ||
			       !m_async_host_thread_exited.load(std::memory_order_acquire)) {
				SDL_PumpEvents();
				if (!m_present_thread_exited.load(std::memory_order_acquire) ||
				    !m_async_host_thread_exited.load(std::memory_order_acquire)) {
					Common::Thread::SleepMicro(1000);
				}
			}
		}
		m_present_thread.join();
	}
	if (m_async_host_thread.joinable()) m_async_host_thread.join();
	for (int handle = 1; handle < VIDEO_OUT_NUM_MAX; handle++) {
		if (m_video_out_ctx[handle].master != nullptr) {
			(void)Close(handle);
		}
	}
	for (int handle = 1; handle < VIDEO_OUT_NUM_MAX; handle++) {
		(void)Close(handle);
	}
}

void VideoOutDriver::Impl::Init(uint32_t width, uint32_t height) {
	for (auto& ctx: m_video_out_ctx) {
		ctx.width  = width;
		ctx.height = height;
	}
}

int VideoOutDriver::Impl::Open(int bus_type, int index) {
	const auto bus = std::find(VIDEO_OUT_BUSES.begin(), VIDEO_OUT_BUSES.end(), bus_type);
	if (bus == VIDEO_OUT_BUSES.end()) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}
	if (bus_type == VIDEO_OUT_BUS_TYPE_HMD &&
	    (!Config::VrEnabled() || !Graphics::Vr::HostBridge().Active())) {
		return VIDEO_OUT_ERROR_NO_DEVICE;
	}
	EXIT_NOT_IMPLEMENTED(index != 0);
	Common::LockGuard lock(m_mutex);

	const int handle = static_cast<int>(bus - VIDEO_OUT_BUSES.begin()) + 1;
	if (m_video_out_ctx[handle].opened) {
		return VIDEO_OUT_ERROR_RESOURCE_BUSY;
	}
	auto&             config = m_video_out_ctx[handle];
	Common::LockGuard config_lock(config.mutex);

	{
		Common::LockGuard event_lock(config.events->mutex);
		EXIT_IF(!config.events->flip.empty());
		EXIT_IF(!config.events->pre_vblank.empty());
		EXIT_IF(!config.events->vblank.empty());
		EXIT_IF(!config.events->output_mode.empty());
	}
	EXIT_IF(config.flip_rate != 0);
	for (const auto& buffer: config.buffers) {
		EXIT_IF(buffer.Occupied());
	}
	for (const auto& group: config.groups) {
		EXIT_IF(group.occupied);
	}

	config.closing = false;
	config.opened  = true;
	config.bus     = bus_type;
	if (++config.generation == 0) {
		EXIT("video-out port generation wrapped\n");
	}
	config.async_epoch_generation = config.generation;
	config.async_admitted_epoch = config.async_published_epoch = 0;
	config.output_mode               = VIDEO_OUT_OUTPUT_MODE_DEFAULT;
	config.flip_status               = VideoOutFlipStatus();
	config.flip_status.flipArg       = -1;
	config.flip_status.currentBuffer = -1;
	config.flip_status.count         = 0;
	config.pre_vblank_status         = VideoOutVblankStatus();
	config.vblank_status             = VideoOutVblankStatus();

	return handle;
}

bool VideoOutDriver::Impl::Close(int handle) {
	Common::LockGuard lock(m_mutex);

	if (handle <= 0 || handle >= VIDEO_OUT_NUM_MAX || !m_video_out_ctx[handle].opened) {
		return false;
	}

	auto&               config = m_video_out_ctx[handle];
	VideoOutEventQueues flip_events;
	VideoOutEventQueues pre_vblank_events;
	VideoOutEventQueues vblank_events;
	VideoOutEventQueues output_mode_events;
	VideoOutConfig* master = nullptr;
	{
		Common::LockGuard config_lock(config.mutex);
		if (config.closing) {
			return false;
		}
		EXIT_IF(config.slave_count != 0);
		config.opened  = false;
		config.closing = true;
		if (++config.generation == 0) {
			EXIT("video-out port generation wrapped\n");
		}
		{
			Common::LockGuard event_lock(config.events->mutex);
			flip_events        = std::move(config.events->flip);
			pre_vblank_events  = std::move(config.events->pre_vblank);
			vblank_events      = std::move(config.events->vblank);
			output_mode_events = std::move(config.events->output_mode);
		}
		config.flip_rate   = 0;
		master             = config.master;
		config.master      = nullptr;

		for (const auto& buffer: config.buffers) {
			if (buffer.Occupied() &&
			    (buffer.group_index >= VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX ||
			     buffer.data_address == 0 || !config.groups[buffer.group_index].occupied)) {
				EXIT("inconsistent registered video-out buffer state\n");
			}
		}
		for (auto& buffer: config.buffers) {
			buffer = VideoOutBuffer {};
		}
		for (auto& group: config.groups) {
			group = BufferAttributeGroup {};
		}
		config.vblank_cond.SignalAll();
	}
	if (master != nullptr) {
		Common::LockGuard master_lock(master->mutex);
		master->slave_count--;
	}
	m_flip_queue.Cancel(config);
	m_presenter.ClearLayer(config.bus);
	if (config.bus == VIDEO_OUT_BUS_TYPE_HMD) {
		Hmd2::ClearStereoFrames();
	}
	DeleteVideoOutEvents(flip_events, VideoOutEventKind::Flip);
	DeleteVideoOutEvents(pre_vblank_events, VideoOutEventKind::PreVblankStart);
	DeleteVideoOutEvents(vblank_events, VideoOutEventKind::Vblank);
	DeleteVideoOutEvents(output_mode_events, VideoOutEventKind::OutputMode);
	return true;
}

VideoOutConfig* VideoOutDriver::Impl::Get(int handle) {
	Common::LockGuard lock(m_mutex);
	if (handle <= 0 || handle >= VIDEO_OUT_NUM_MAX || !m_video_out_ctx[handle].opened) {
		return nullptr;
	}

	return m_video_out_ctx + handle;
}

VideoOutConfig* VideoOutDriver::Impl::Get(int handle, uint64_t& generation) {
	Common::LockGuard lock(m_mutex);
	if (handle <= 0 || handle >= VIDEO_OUT_NUM_MAX || !m_video_out_ctx[handle].opened) {
		return nullptr;
	}

	auto*             config = m_video_out_ctx + handle;
	Common::LockGuard config_lock(config->mutex);
	if (!config->opened || config->closing) {
		return nullptr;
	}
	generation = config->generation;
	return config;
}

bool VideoOutDriver::Impl::IsOpened(int handle) {
	Common::LockGuard lock(m_mutex);

	return handle > 0 && handle < VIDEO_OUT_NUM_MAX && m_video_out_ctx[handle].opened;
}

int VideoOutDriver::Impl::SetFlipMaster(int slave_handle, int master_handle) {
	Common::LockGuard lock(m_mutex);
	if (slave_handle <= 0 || slave_handle >= VIDEO_OUT_NUM_MAX ||
	    master_handle <= 0 || master_handle >= VIDEO_OUT_NUM_MAX ||
	    !m_video_out_ctx[slave_handle].opened || !m_video_out_ctx[master_handle].opened) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	auto& slave  = m_video_out_ctx[slave_handle];
	auto& master = m_video_out_ctx[master_handle];
	if (&slave == &master) {
		return VIDEO_OUT_ERROR_UNSUPPORTED_OPERATION;
	}
	Common::LockGuard first_lock(m_video_out_ctx[std::min(slave_handle, master_handle)].mutex);
	Common::LockGuard second_lock(m_video_out_ctx[std::max(slave_handle, master_handle)].mutex);
	if (slave.master != nullptr || slave.slave_count != 0 || master.master != nullptr) {
		return VIDEO_OUT_ERROR_UNSUPPORTED_OPERATION;
	}
	if (slave.flip_status.flipPendingNum != 0 || master.flip_status.flipPendingNum != 0) {
		return VIDEO_OUT_ERROR_RESOURCE_BUSY;
	}
	slave.master = &master;
	master.slave_count++;
	return OK;
}

void VideoOutDriver::Impl::VblankBegin() {
	Common::LockGuard lock(m_mutex);

	for (int i = 1; i < VIDEO_OUT_NUM_MAX; i++) {
		auto& ctx = m_video_out_ctx[i];
		if (ctx.opened) {
			ctx.mutex.Lock();
			ctx.pre_vblank_status.count++;
			ctx.pre_vblank_status.processTime        = LibKernel::KernelGetProcessTime();
			ctx.pre_vblank_status.reserved           = LibKernel::KernelReadTsc();
			ctx.pre_vblank_status.processTimeCounter = LibKernel::KernelGetProcessTimeCounter();

			TriggerVideoOutEvents(ctx, VideoOutEventKind::PreVblankStart,
			                      reinterpret_cast<void*>(ctx.pre_vblank_status.count));
			ctx.mutex.Unlock();
		}
	}
}

void VideoOutDriver::Impl::VblankEnd() {
	Common::LockGuard lock(m_mutex);

	for (int i = 1; i < VIDEO_OUT_NUM_MAX; i++) {
		auto& ctx = m_video_out_ctx[i];
		if (ctx.opened) {
			ctx.mutex.Lock();
			ctx.vblank_status.count++;
			ctx.vblank_status.processTime        = LibKernel::KernelGetProcessTime();
			ctx.vblank_status.reserved           = LibKernel::KernelReadTsc();
			ctx.vblank_status.processTimeCounter = LibKernel::KernelGetProcessTimeCounter();

			TriggerVideoOutEvents(ctx, VideoOutEventKind::Vblank,
			                      reinterpret_cast<void*>(ctx.vblank_status.count));
			ctx.vblank_cond.SignalAll();
			ctx.mutex.Unlock();
		}
	}
}

void VideoOutDriver::Impl::PresentThread(std::stop_token token) {
	if (m_async_present) {
		AsyncClockThread(token);
		return;
	}
	const auto frequency = Common::Timer::QueryPerformanceFrequency();
	EXIT_IF(frequency == 0);

	int64_t total_wait = 0;
	const bool diagnose = PerformanceDiagnostics();
	uint64_t last_present_ns = diagnose ? DiagnosticNowNs() : 0;
	uint64_t last_presented = 0, next_pending_report_ns = 0, next_resumed_report_ns = 0, loops = 0;
	uint64_t sleep_requested_max = 0, sleep_actual_max = 0, loop_max = 0;
	uint64_t vblank_begin_max = 0, vblank_end_max = 0, flip_max = 0;
	VblankPacingDiagnostics pacing_diagnostics;
	while (!token.stop_requested()) {
		const auto diagnostic_loop_begin = diagnose ? DiagnosticNowNs() : 0;
		const auto sleep_begin = Common::Timer::QueryPerformanceCounter();
		const bool diagnostic_no_sleep = diagnose && total_wait <= 0;
		if (total_wait > 0) {
			const auto remaining_us =
			    (static_cast<uint64_t>(total_wait) * 1000000u + frequency - 1) / frequency;
			if (diagnose) sleep_requested_max = std::max(sleep_requested_max, remaining_us * 1000u);
			Common::Thread::SleepMicro(static_cast<uint32_t>(
			    std::clamp<uint64_t>(remaining_us, 1, std::numeric_limits<uint32_t>::max())));
		}
		if (token.stop_requested()) {
			break;
		}
		const auto frame_begin = Common::Timer::QueryPerformanceCounter();
		if (diagnose) sleep_actual_max = std::max(sleep_actual_max, DiagnosticNowNs() - diagnostic_loop_begin);
		total_wait -= static_cast<int64_t>(frame_begin - sleep_begin);
		const int64_t diagnostic_wake_balance = diagnose ? total_wait : 0;

		const auto refresh = std::max(Config::GetVblankFrequency(), 1u);
		const auto period  = std::max(frequency / refresh, uint64_t {1});

		if (m_presenter.IsGuestPaused()) {
			(void)m_presenter.PresentLastFrame();
			const auto frame_end = Common::Timer::QueryPerformanceCounter();
			total_wait +=
			    static_cast<int64_t>(period) - static_cast<int64_t>(frame_end - frame_begin);
			if (diagnose) {
				last_present_ns = DiagnosticNowNs();
				last_presented = m_renderer.GetGraphics().presented_frames.load(std::memory_order_relaxed);
				loops = sleep_requested_max = sleep_actual_max = loop_max = 0;
				vblank_begin_max = vblank_end_max = flip_max = 0;
				m_flip_queue.ResetDiagnostics();
				pacing_diagnostics = {}; // Do not count a paused interval as guest Vblank spacing.
			}
			continue;
		}

		const auto before_vblank = diagnose ? DiagnosticNowNs() : 0;
		VblankBegin();
		const auto before_flip = diagnose ? DiagnosticNowNs() : 0;
		if (diagnose) vblank_begin_max = std::max(vblank_begin_max, before_flip - before_vblank);
		bool presented = false;
		if (Config::VrEnabled()) {
			// Independent desktop and HMD groups can be ready at the same refresh.
			// Drain them without forcing each port to consume a separate 60 Hz tick.
			// Flip preserves FIFO and permits each port at most once this Vblank.
			uint64_t presented_buses = 0;
			for (size_t group = 0; group < VIDEO_OUT_BUSES.size(); ++group) {
				if (!m_flip_queue.Flip(0, &presented_buses)) { break; }
				presented = true;
			}
		} else {
			presented = m_flip_queue.Flip(0);
		}
		if (diagnose) flip_max = std::max(flip_max, DiagnosticNowNs() - before_flip);
		if (!presented && m_presenter.NeedsSystemOverlayRefresh()) {
			if (m_presenter.PresentLastFrame()) {
				presented = true;
			} else {
				uint32_t width  = 0;
				uint32_t height = 0;
				{
					Common::LockGuard lock(m_mutex);
					width  = m_video_out_ctx[0].width;
					height = m_video_out_ctx[0].height;
				}
				auto& blank = m_presenter.PrepareBlankFrame(width, height, true);
				m_presenter.Present(blank);
				presented = true;
			}
		}
		if (!presented && total_wait < 0) {
			bool     any_open = false;
			uint32_t width    = 0;
			uint32_t height   = 0;
			{
				Common::LockGuard lock(m_mutex);
				width  = m_video_out_ctx[0].width;
				height = m_video_out_ctx[0].height;
				for (int handle = 1; handle < VIDEO_OUT_NUM_MAX; handle++) {
					any_open |= m_video_out_ctx[handle].opened;
				}
			}
			if (!any_open) {
				auto& blank = m_presenter.PrepareBlankFrame(width, height, true);
				m_presenter.Present(blank);
			}
		}
		const auto before_end = diagnose ? DiagnosticNowNs() : 0;
		VblankEnd();

		const auto frame_end = Common::Timer::QueryPerformanceCounter();
		total_wait += static_cast<int64_t>(period) - static_cast<int64_t>(frame_end - frame_begin);
		if (diagnose) {
			const auto now = DiagnosticNowNs();
			pacing_diagnostics.Record(diagnostic_loop_begin, sleep_begin, now, frame_end,
			                          frequency, period, diagnostic_wake_balance, total_wait,
			                          diagnostic_no_sleep, presented);
			loops++;
			loop_max = std::max(loop_max, now - diagnostic_loop_begin);
			vblank_end_max = std::max(vblank_end_max, now - before_end);
			const auto frame = m_renderer.GetGraphics().presented_frames.load(std::memory_order_relaxed);
			const bool advanced = frame != last_presented;
			const auto gap = now - last_present_ns;
			// A pending report must not suppress the eventual full stall duration on resume.
			auto& next_report_ns = advanced ? next_resumed_report_ns : next_pending_report_ns;
			if (gap >= 100'000'000u && now >= next_report_ns) {
				next_report_ns = now + 2'000'000'000u;
				const auto& d = m_flip_queue.Diagnostics();
				const auto ms = [](uint64_t ns) { return double(ns) / 1e6; };
				Log::WriteToConsoleAndLog(fmt::format(
				    "GT7 perf flip queue: steady_ms={:.3f} frame={} resumed={} gap_ms={:.3f} "
				    "loops={} flip_calls={} no_owner={} not_ready={} not_due={} held={} "
				    "head={} state={} queued={}/{} request_ms(reserve/prepare_begin/prepare_end/ready)="
				    "{:.3f}/{:.3f}/{:.3f}/{:.3f} max_ms(loop/sleep_requested/sleep_actual/vblank_begin/flip/"
				    "vblank_end/queue_lock/port_lock)={:.3f}/{:.3f}/{:.3f}/{:.3f}/{:.3f}/{:.3f}/{:.3f}/{:.3f}\n",
				    ms(now), frame, advanced ? 1 : 0, ms(gap), loops, d.calls, d.no_owner, d.not_ready,
				    d.not_due, d.held, d.head_id, d.head_state, d.queued, d.cpu_queued,
				    ms(d.reserved_ns), ms(d.preparing_ns), ms(d.prepared_ns), ms(d.ready_ns),
				    ms(loop_max), ms(sleep_requested_max), ms(sleep_actual_max), ms(vblank_begin_max),
				    ms(flip_max), ms(vblank_end_max), ms(d.queue_lock_max_ns), ms(d.port_lock_max_ns)));
			}
			if (advanced) {
				last_present_ns = now;
				last_presented = frame;
				loops = sleep_requested_max = sleep_actual_max = loop_max = 0;
				vblank_begin_max = vblank_end_max = flip_max = 0;
				m_flip_queue.ResetDiagnostics();
			}
		}
	}
}

bool VideoOutDriver::Impl::AsyncClockTick(bool paused) {
	// Close the host admission gate before any cfg counter changes. The worker sees the next
	// epoch only after the full guest-visible transaction, including all ports, has finished.
	m_flip_queue.BeginAsyncTick(paused);
	bool published = false;
	if (!paused) {
		VblankBegin();
		published = m_flip_queue.PublishCompleted();
		VblankEnd();
	}
	uint32_t width = 0, height = 0;
	bool any_open = false;
	{
		Common::LockGuard lock(m_mutex);
		width = m_video_out_ctx[0].width;
		height = m_video_out_ctx[0].height;
		for (int i = 1; i < VIDEO_OUT_NUM_MAX; ++i) any_open |= m_video_out_ctx[i].opened;
	}
	m_flip_queue.EndAsyncTick(paused, width, height, any_open);
	return published;
}

void VideoOutDriver::Impl::AsyncClockThread(std::stop_token token) {
	const uint64_t frequency = Common::Timer::QueryPerformanceFrequency();
	EXIT_IF(frequency == 0);
	uint64_t deadline = Common::Timer::QueryPerformanceCounter();
	bool was_paused = false;
	const bool diagnose = PerformanceDiagnostics();
	VblankPacingDiagnostics diagnostics;
	while (!token.stop_requested()) {
		const auto loop_begin_ns = diagnose ? DiagnosticNowNs() : 0;
		const auto sleep_begin = Common::Timer::QueryPerformanceCounter();
		const bool no_sleep = sleep_begin >= deadline;
		{
			Common::LockGuard lock(m_async_clock_mutex);
			while (!token.stop_requested()) {
				const auto now = Common::Timer::QueryPerformanceCounter();
				if (now >= deadline) break;
				const uint64_t remaining_us = ((deadline - now) * 1'000'000ull + frequency - 1) / frequency;
				m_async_clock_wake.WaitFor(&m_async_clock_mutex, static_cast<uint32_t>(
				    std::clamp<uint64_t>(remaining_us, 1, std::numeric_limits<uint32_t>::max())));
			}
		}
		if (token.stop_requested()) break;
		const auto frame_begin = Common::Timer::QueryPerformanceCounter();
		const int64_t wake_balance = static_cast<int64_t>(deadline) - static_cast<int64_t>(frame_begin);
		const uint64_t period = std::max(frequency / std::max(Config::GetVblankFrequency(), 1u), uint64_t {1});
		const bool paused = m_presenter.IsGuestPaused();
		const bool published = AsyncClockTick(paused);
		const auto frame_end = Common::Timer::QueryPerformanceCounter();
		// A pause freezes the guest clock. Resuming starts a fresh period, never catches up
		// ticks accumulated while paused. Ordinary OS scheduling lateness retains its deadline.
		deadline = paused || was_paused ? frame_end + period : deadline + period;
		was_paused = paused;
		if (diagnose) {
			if (paused) {
				diagnostics = {};
			} else {
				diagnostics.Record(loop_begin_ns, sleep_begin, DiagnosticNowNs(), frame_end, frequency,
				                   period, wake_balance,
				                   static_cast<int64_t>(deadline) - static_cast<int64_t>(frame_end),
				                   no_sleep, published);
			}
		}
	}
}

void VideoOutDriver::Impl::AsyncHostThread(std::stop_token token) {
	while (!token.stop_requested()) {
		// Snapshot before attempting work, then wait on the same mutex-protected revision.
		// Ready/epoch/Cancel arriving during the attempt cannot be lost before sleeping.
		const auto revision = m_flip_queue.AsyncRevision();
		if (!m_presenter.IsGuestPaused() && m_flip_queue.Flip(0, nullptr, true)) continue;
		uint32_t width = 0, height = 0;
		bool paused = false, any_open = false;
		if (!token.stop_requested() && m_flip_queue.TakeAsyncRefresh(width, height, paused, any_open)) {
			// All host presentation, including pause/system UI/no-port fallback, stays on this
			// single worker. No driver, cfg or queue mutex is held during any Presenter call.
			if (paused || m_presenter.IsGuestPaused()) {
				(void)m_presenter.PresentLastFrame();
			} else if (m_presenter.NeedsSystemOverlayRefresh()) {
				if (!m_presenter.PresentLastFrame()) {
					auto& blank = m_presenter.PrepareBlankFrame(width, height, true);
					m_presenter.Present(blank);
				}
			} else if (!any_open) {
				auto& blank = m_presenter.PrepareBlankFrame(width, height, true);
				m_presenter.Present(blank);
			}
		}
		m_flip_queue.WaitAsyncEvent(revision, token);
	}
}

bool FlipQueue::Reserve(VideoOutConfig& cfg, int index, int64_t flip_arg, FlipRequestSource source,
                        uint64_t& request_id, ReserveWaitDiagnostic* diagnostic) {
	if (diagnostic != nullptr) diagnostic->queue_begin_ns = DiagnosticNowNs();
	Common::LockGuard lock(m_mutex);
	if (diagnostic != nullptr) diagnostic->queue_acquired_ns = DiagnosticNowNs();

	// A newer slave request replaces the one not yet captured by a master flip.
	if (cfg.master != nullptr) {
		for (auto* queue: {&m_requests, &m_cpu_requests}) {
			for (auto it = queue->begin(); it != queue->end();) {
				if (it->cfg != &cfg || it->group != 0) {
					++it;
					continue;
				}
				cfg.flip_status.flipPendingNum--;
				cfg.flip_status.gcQueueNum -= it->source == FlipRequestSource::GpuEop;
				if (it->state == RequestState::Ready) {
					if (it->frame != nullptr) {
						m_presenter.Discard(*it->frame);
					}
					it = queue->erase(it);
				} else {
					auto cancelled = it++;
					m_cancelled_requests.splice(m_cancelled_requests.end(), *queue, cancelled);
				}
			}
		}
		m_done_cond_var.SignalAll();
	}
	if (cfg.flip_status.flipPendingNum >= VIDEO_OUT_FLIP_QUEUE_CAPACITY) {
		return false;
	}
	auto& pending = source == FlipRequestSource::GpuEop ? m_requests : m_cpu_requests;

	Request r {};
	r.id         = m_next_request_id++;
	r.group      = cfg.master == nullptr ? r.id : 0;
	r.cfg        = &cfg;
	r.master     = cfg.master;
	r.generation = cfg.generation;
	r.bus        = cfg.bus;
	r.index      = index;
	r.flip_arg   = flip_arg;
	r.submit_ptc = LibKernel::KernelGetProcessTimeCounter();
	r.source     = source;
	r.state      = RequestState::Reserved;
	if (PerformanceDiagnostics()) r.diagnostic_reserved_ns = DiagnosticNowNs();
	r.hmd_output = cfg.bus == VIDEO_OUT_BUS_TYPE_HMD;
	if (cfg.slave_count != 0) {
		for (auto* queue: {&m_requests, &m_cpu_requests}) {
			for (auto& slave: *queue) {
				if (slave.master == &cfg && slave.group == 0) {
					slave.group = r.id;
				}
			}
		}
	}

	pending.push_back(r);
	request_id = r.id;

	cfg.flip_status.flipPendingNum++;
	cfg.flip_status.submitProcessTimeCounter = r.submit_ptc;
	if (source == FlipRequestSource::GpuEop) {
		cfg.flip_status.gcQueueNum++;
	}

	return true;
}

FlipQueue::~FlipQueue() {
	for (auto* queue: {&m_requests, &m_cpu_requests, &m_cancelled_requests}) {
		for (auto& request: *queue) {
			if (request.frame != nullptr) {
				m_presenter.Discard(*request.frame);
			}
		}
	}
}

void FlipQueue::Cancel(VideoOutConfig& cfg) {
	std::vector<Graphics::Presenter::Frame*> frames;
	m_mutex.Lock();
	while (m_processing && std::any_of(m_requests.begin(), m_requests.end(), [&cfg](const auto& r) {
		       return r.cfg == &cfg && r.state == RequestState::Presenting;
	       })) {
		m_done_cond_var.Wait(&m_mutex);
	}
	for (auto* queue: {&m_requests, &m_cpu_requests}) {
		for (auto it = queue->begin(); it != queue->end();) {
			if (it->cfg != &cfg) {
				++it;
				continue;
			}
			if (it->state == RequestState::Reserved || it->state == RequestState::Recording) {
				auto cancelled = it++;
				m_cancelled_requests.splice(m_cancelled_requests.end(), *queue, cancelled);
				continue;
			}
			if (it->state == RequestState::Presenting) {
				EXIT("video-out cancellation retained a presenting request\n");
			}
			if (it->frame != nullptr) {
				frames.push_back(it->frame);
			}
			it = queue->erase(it);
		}
	}
	m_done_cond_var.SignalAll();
	m_submit_slot_cond_var.SignalAll();
	m_submit_cond_var.SignalAll();
	if (m_async_enabled) ++m_async_revision;
	m_mutex.Unlock();
	for (auto* frame: frames) {
		m_presenter.Discard(*frame);
	}
	Common::LockGuard lock(cfg.mutex);
	cfg.flip_status.flipPendingNum = 0;
	cfg.flip_status.gcQueueNum     = 0;
}

void FlipQueue::Prepare(uint64_t request_id, Graphics::CommandBuffer& buffer) {
	const bool      source_trace = Common::SourceTrace::Enabled();
	const bool      diagnose   = PerformanceDiagnostics() || source_trace;
	PreparationDiagnostic diagnostic;
	VideoOutConfig* cfg        = nullptr;
	uint64_t        generation = 0;
	int             index      = 0;
	bool            hmd_output = false;
	{
		Common::LockGuard lock(m_mutex);
		auto request = std::find_if(m_requests.begin(), m_requests.end(),
		                            [request_id](const auto& r) { return r.id == request_id; });
		if (request == m_requests.end()) {
			auto pending = std::find_if(m_cpu_requests.begin(), m_cpu_requests.end(),
			                            [request_id](const auto& r) { return r.id == request_id; });
			if (pending == m_cpu_requests.end()) {
				auto cancelled =
				    std::find_if(m_cancelled_requests.begin(), m_cancelled_requests.end(),
				                 [request_id](const auto& r) { return r.id == request_id; });
				if (cancelled == m_cancelled_requests.end() ||
				    cancelled->state != RequestState::Reserved) {
					EXIT("cannot prepare video-out request id=%" PRIu64 "\n", request_id);
				}
				cancelled->state = RequestState::Recording;
				return;
			}
			request = m_requests.insert(m_requests.end(), *pending);
			m_cpu_requests.erase(pending);
		}
		if (request->state != RequestState::Reserved) {
			EXIT("cannot prepare video-out request id=%" PRIu64 "\n", request_id);
		}
		request->state = RequestState::Recording;
		if (PerformanceDiagnostics()) request->diagnostic_preparing_ns = DiagnosticNowNs();
		cfg            = request->cfg;
		generation     = request->generation;
		index          = request->index;
		hmd_output     = request->hmd_output;
		if (diagnose) {
			diagnostic.request = request->id;
			diagnostic.group = request->group;
			diagnostic.generation = request->generation;
			diagnostic.source = request->source;
			diagnostic.index = request->index;
			diagnostic.hmd = request->hmd_output;
		}
	}

	const bool          special = IsSpecialBufferIndex(index);
	Graphics::ImageInfo source_info;
	uint32_t            width   = 0;
	uint32_t            height  = 0;
	bool                current = false;
	bool                premultiplied_alpha = false;
	{
		Common::LockGuard lock(cfg->mutex);
		current = cfg->opened && !cfg->closing && cfg->generation == generation;
		if (current) {
			if (diagnose) diagnostic.bus = cfg->bus;
			if (special) {
				width  = cfg->width;
				height = cfg->height;
			} else {
				const auto& surface = cfg->buffers[index];
				if (!surface.Occupied()) {
					EXIT("cannot prepare flip from an unregistered surface, id=%" PRIu64
					     " index=%d\n",
					     request_id, index);
				}
				if (surface.group_index >= VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX ||
				    !cfg->groups[surface.group_index].occupied) {
					EXIT("video-out surface references an unavailable attribute group, id=%" PRIu64
					     " index=%d group=%d\n",
					     request_id, index, surface.group_index);
				}
				if (!hmd_output) {
					source_info = cfg->groups[surface.group_index].ImageInfo(surface);
				}
				premultiplied_alpha =
				    (cfg->groups[surface.group_index].attribute.option &
				     VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_ALPHA_PREMULTIPLIED) != 0;
			}
		}
	}
	if (!current) {
		Common::LockGuard lock(m_mutex);
		const auto        request =
		    std::find_if(m_requests.begin(), m_requests.end(),
		                 [request_id](const auto& r) { return r.id == request_id; });
		if (request != m_requests.end()) {
			m_cancelled_requests.splice(m_cancelled_requests.end(), m_requests, request);
		}
		return;
	}
	Graphics::Presenter::Frame* frame = nullptr;
	// Round 133: a frame of the main display that left out many draws while their pipelines were
	// looked up is not shown (the GPU thread, after the frame's draws).
	const bool hold =
	    !special && cfg->bus == VIDEO_OUT_BUS_TYPE_MAIN && Graphics::GpuPhases::TakeFrameHold();
	if (diagnose) {
		diagnostic.special = special;
		diagnostic.hold = hold;
		if (!hmd_output && !special) {
			diagnostic.address = source_info.data.address;
			diagnostic.size = source_info.data.size;
			diagnostic.format = static_cast<uint32_t>(source_info.pixel_format);
			diagnostic.width = source_info.extent.width;
			diagnostic.height = source_info.extent.height;
		} else {
			diagnostic.width = width;
			diagnostic.height = height;
		}
		diagnostic.begin_ns = DiagnosticNowNs();
	}
	if (hmd_output) {
		Hmd2::StereoFrameSnapshot stereo;
		if (special) {
			Hmd2::ClearStereoFrames();
		} else if (Hmd2::TakeStereoFrame(index, stereo)) {
			Graphics::Vr::Pose render_pose;
			const bool known_pose = Graphics::Vr::MakeRenderPose(stereo.guest_position.data(),
			    stereo.guest_orientation.data(), render_pose);
			if (known_pose) {
				frame = m_presenter.PrepareVrFrame(buffer, stereo.eyes, &render_pose);
			} else {
				static std::atomic<uint32_t> bad_pose_reports {0};
				if (bad_pose_reports.fetch_add(1, std::memory_order_relaxed) < 8) {
					LOGF("Hmd2: skipping stereo buffer=%d token=%" PRIu64 " with invalid submitted head pose\n",
					     index, stereo.frame_token);
				}
			}
		}
		// Missing/unsupported metadata completes this flip with no eye submission.
		// In particular, a type-0x12 mono layer is never copied to both eyes.
	} else if (special) {
		frame = &m_presenter.PrepareBlankFrame(width, height, index == VIDEO_OUT_BUFFER_INDEX_BLACK,
		                                       &buffer);
	} else {
		frame = &m_presenter.PrepareFrame(buffer, source_info);
	}
	if (diagnose) {
		diagnostic.end_ns = DiagnosticNowNs();
		diagnostic.has_frame = frame != nullptr;
		if (source_trace) {
			// Source capture replaces the synchronous per-frame detail/summary output. The
			// reporter drains scalar records; no request or presentation pointer escapes.
			Common::SourceTrace::Event event {};
			event.kind = Common::SourceTrace::Kind::FlipPrepare;
			event.flags = (diagnostic.source == FlipRequestSource::GpuEop ? 1u : 0u) |
			    (diagnostic.has_frame ? 1u << 8u : 0u) | (diagnostic.hold ? 1u << 9u : 0u) |
			    (diagnostic.special ? 1u << 10u : 0u);
			event.begin_ns = diagnostic.begin_ns;
			event.end_ns = event.work_end_ns = diagnostic.end_ns;
			event.submission_id = Common::SourceTrace::t_context.submission_id;
			event.request_id = request_id;
			event.arg0 = static_cast<uint64_t>(diagnostic.bus);
			event.arg1 = static_cast<uint64_t>(diagnostic.index);
			Common::SourceTrace::Record(event);
		} else {
			// Log outside the queue, port and renderer locks. The interval excludes diagnostic I/O.
			m_preparation_diagnostics.Record(diagnostic);
		}
	}

	Common::LockGuard lock(m_mutex);
	Request*          prepared = nullptr;
	if (const auto request =
	        std::find_if(m_requests.begin(), m_requests.end(),
	                     [request_id](const auto& r) { return r.id == request_id; });
	    request != m_requests.end()) {
		prepared = &*request;
	} else if (const auto cancelled =
	               std::find_if(m_cancelled_requests.begin(), m_cancelled_requests.end(),
	                            [request_id](const auto& r) { return r.id == request_id; });
	           cancelled != m_cancelled_requests.end()) {
		prepared = &*cancelled;
	}
	if (prepared == nullptr || prepared->state != RequestState::Recording ||
	    prepared->frame != nullptr) {
		EXIT("video-out request changed while recording, id=%" PRIu64 "\n", request_id);
	}
	prepared->frame = frame;
	prepared->premultiplied_alpha = premultiplied_alpha;
	prepared->hold                = hold;
	if (PerformanceDiagnostics()) prepared->diagnostic_prepared_ns = DiagnosticNowNs();
}

void FlipQueue::Complete(uint64_t request_id) {
	const bool source_trace = Common::SourceTrace::Enabled();
	Common::SourceTrace::Event source_event {};
	m_mutex.Lock();
	auto request = std::find_if(m_requests.begin(), m_requests.end(),
	                            [request_id](const auto& r) { return r.id == request_id; });
	if (request != m_requests.end()) {
		if (request->state != RequestState::Recording ||
		    (request->frame == nullptr && !request->hmd_output)) {
			m_mutex.Unlock();
			EXIT("completed GPU flip has no prepared recording, id=%" PRIu64 "\n", request_id);
		}
		request->state = RequestState::Ready;
		if (PerformanceDiagnostics()) request->diagnostic_ready_ns = DiagnosticNowNs();
		if (source_trace) {
			source_event.kind = Common::SourceTrace::Kind::FlipReady;
			source_event.flags = request->source == FlipRequestSource::GpuEop ? 1u : 0u;
			source_event.begin_ns = Common::SourceTrace::NowNs();
			source_event.end_ns = source_event.work_end_ns = source_event.begin_ns;
			source_event.request_id = request_id;
			source_event.arg0 = static_cast<uint64_t>(request->bus);
			source_event.arg1 = static_cast<uint64_t>(request->index);
		}
		if (m_async_enabled) ++m_async_revision;
		m_submit_cond_var.Signal();
		m_mutex.Unlock();
		if (source_trace) Common::SourceTrace::Record(source_event);
		return;
	}
	auto cancelled = std::find_if(m_cancelled_requests.begin(), m_cancelled_requests.end(),
	                              [request_id](const auto& r) { return r.id == request_id; });
	if (cancelled == m_cancelled_requests.end() || cancelled->state != RequestState::Recording) {
		m_mutex.Unlock();
		EXIT("completed GPU flip has no prepared recording, id=%" PRIu64 "\n", request_id);
	}
	auto* frame = cancelled->frame;
	m_cancelled_requests.erase(cancelled);
	m_done_cond_var.SignalAll();
	m_mutex.Unlock();
	if (frame != nullptr) {
		m_presenter.Discard(*frame);
	}
}

void FlipQueue::WaitForSubmitSlot(VideoOutConfig& cfg) {
	Common::LockGuard lock(m_mutex);
	auto belongs_to_port = [&cfg](const auto& request) { return request.cfg == &cfg; };
	while (std::count_if(m_requests.begin(), m_requests.end(), belongs_to_port) +
	           std::count_if(m_cpu_requests.begin(), m_cpu_requests.end(), belongs_to_port) >=
	       VIDEO_OUT_FLIP_QUEUE_CAPACITY) {
		if (m_requests.empty()) {
			EXIT("video-out queue is saturated by CPU flips queued behind the current EOP\n");
		}
		m_submit_slot_cond_var.Wait(&m_mutex);
	}
}

void FlipQueue::Wait(VideoOutConfig& cfg, int index) {
	Common::LockGuard lock(m_mutex);

	auto has_request = [this, &cfg, index] {
		auto matches = [&cfg, index](const auto& r) { return r.cfg == &cfg && r.index == index; };
		return std::any_of(m_requests.begin(), m_requests.end(), matches) ||
		       std::any_of(m_cpu_requests.begin(), m_cpu_requests.end(), matches);
	};
	while (has_request()) {
		m_done_cond_var.Wait(&m_mutex);
	}
}

void FlipQueue::BeginAsyncTick(bool paused) {
	Common::LockGuard lock(m_mutex);
	m_async_enabled = true;
	m_async_tick_in_progress = true;
	m_async_paused = paused;
}

void FlipQueue::EndAsyncTick(bool paused, uint32_t width, uint32_t height, bool any_open) {
	Common::LockGuard lock(m_mutex);
	if (!paused && ++m_async_epoch == 0) EXIT("video-out async epoch wrapped\n");
	m_async_width = width;
	m_async_height = height;
	m_async_any_open = any_open;
	m_async_paused = paused;
	m_async_refresh = true;
	m_async_tick_in_progress = false;
	++m_async_revision;
	m_submit_cond_var.SignalAll();
}

uint64_t FlipQueue::AsyncRevision() {
	Common::LockGuard lock(m_mutex);
	return m_async_revision;
}

void FlipQueue::WaitAsyncEvent(uint64_t revision, std::stop_token token) {
	Common::LockGuard lock(m_mutex);
	while (!token.stop_requested() && revision == m_async_revision) {
		m_submit_cond_var.Wait(&m_mutex);
	}
}

void FlipQueue::WakeAsync() {
	Common::LockGuard lock(m_mutex);
	++m_async_revision;
	m_submit_cond_var.SignalAll();
}

bool FlipQueue::TakeAsyncRefresh(uint32_t& width, uint32_t& height, bool& paused, bool& any_open) {
	Common::LockGuard lock(m_mutex);
	if (!m_async_refresh || m_async_tick_in_progress) return false;
	m_async_refresh = false;
	if (!m_async_paused && m_async_last_host_epoch == m_async_epoch) return false;
	width = m_async_width;
	height = m_async_height;
	paused = m_async_paused;
	any_open = m_async_any_open;
	return true;
}

bool FlipQueue::PublishCompleted() {
	// Only the clock calls this, between VblankBegin and VblankEnd. Snapshot queue metadata,
	// then acquire cfg -> queue in the same order as Reserve/host completion. Close can erase a
	// completed record without waiting for us; no request or consumed Frame pointer is retained.
	bool published = false;
	for (;;) {
		std::array<Request, VideoOutDriver::Impl::VIDEO_OUT_BUSES.size()> requests {};
		size_t count = 0;
		uint64_t group = 0, publication_epoch = 0;
		{
			Common::LockGuard lock(m_mutex);
			EXIT_IF(!m_async_tick_in_progress);
			publication_epoch = m_async_epoch + 1;
			EXIT_IF(publication_epoch == 0);
			const auto owner = std::find_if(m_requests.begin(), m_requests.end(), [](const auto& r) {
				return r.group == r.id && r.state == RequestState::CompletedAwaitVblank;
			});
			if (owner == m_requests.end()) return published;
			group = owner->id;
			for (const auto& r: m_requests) {
				if (r.group != group) continue;
				EXIT_IF(r.state != RequestState::CompletedAwaitVblank || r.frame != nullptr || count == requests.size());
				requests[count++] = r;
			}
		}
		// Completed records can be cancelled and their port reopened after this snapshot.
		// Sort the copied identity, never read mutable cfg fields before taking their locks.
		std::sort(requests.begin(), requests.begin() + count,
		          [](const auto& a, const auto& b) { return a.bus < b.bus; });
		for (size_t i = 0; i < count; ++i) requests[i].cfg->mutex.Lock();
		m_mutex.Lock();
		bool blocked = false;
		for (const auto& r: m_requests) {
			if (r.group != group) continue;
			EXIT_IF(r.state != RequestState::CompletedAwaitVblank || r.frame != nullptr);
			// A slow host call can cross guest ticks. Retain the owner's guest notification
			// phase as well as its admission rate; slaves follow their master, as before.
			if (r.id == group && r.cfg->opened && !r.cfg->closing && r.cfg->generation == r.generation &&
			    !IsFlipDueLocked(*r.cfg, r.generation)) blocked = true;
			if (r.cfg->opened && !r.cfg->closing && r.cfg->generation == r.generation &&
			    r.cfg->async_epoch_generation == r.generation &&
			    r.cfg->async_published_epoch == publication_epoch) blocked = true;
		}
		if (!blocked) {
			for (auto it = m_requests.begin(); it != m_requests.end();) {
				if (it->group != group) { ++it; continue; }
				auto& r = *it;
				if (r.cfg->opened && !r.cfg->closing && r.cfg->generation == r.generation) {
					EXIT_IF(r.cfg->async_epoch_generation != r.generation);
					r.cfg->async_published_epoch = publication_epoch;
					published = true;
					r.cfg->flip_status.count++;
					r.cfg->flip_status.processTime = LibKernel::KernelGetProcessTime();
					r.cfg->flip_status.processTimeCounter = LibKernel::KernelGetProcessTimeCounter();
					r.cfg->flip_status.submitProcessTimeCounter = r.submit_ptc;
					r.cfg->flip_status.flipArg = r.flip_arg;
					r.cfg->flip_status.currentBuffer = r.index;
					r.cfg->flip_status.flipPendingNum--;
					r.cfg->flip_status.gcQueueNum -= r.source == FlipRequestSource::GpuEop;
					TriggerVideoOutEvents(*r.cfg, VideoOutEventKind::Flip, reinterpret_cast<void*>(r.flip_arg));
				}
				it = m_requests.erase(it);
			}
			m_done_cond_var.SignalAll();
			m_submit_slot_cond_var.SignalAll();
		}
		m_mutex.Unlock();
		for (size_t i = count; i != 0; --i) requests[i - 1].cfg->mutex.Unlock();
		if (blocked) return published; // Preserve publication FIFO when a prior group used this epoch.
	}
}

bool FlipQueue::Flip(uint32_t micros, uint64_t* presented_buses, bool async_host) {
	KYTY_PROFILER_BLOCK("FlipQueue::Flip");

	const bool diagnose = PerformanceDiagnostics();
	const auto lock_begin = diagnose ? DiagnosticNowNs() : 0;
	m_mutex.Lock();
	if (async_host && (m_async_paused || m_async_tick_in_progress || m_async_epoch == 0)) {
		m_mutex.Unlock();
		return false;
	}
	const uint64_t admission_epoch = async_host ? m_async_epoch : 0;
	if (diagnose) {
		m_diagnostics.calls++;
		m_diagnostics.queue_lock_max_ns = std::max(m_diagnostics.queue_lock_max_ns,
		                                         DiagnosticNowNs() - lock_begin);
	}
	if (m_requests.empty()) {
		m_submit_cond_var.WaitFor(&m_mutex, micros);
	}
	EXIT_IF(m_processing);
	const auto owner = std::find_if(m_requests.begin(), m_requests.end(), [async_host](const auto& r) {
		return r.group == r.id && (!async_host || r.state != RequestState::CompletedAwaitVblank);
	});
	if (diagnose) {
		m_diagnostics.queued = m_requests.size();
		m_diagnostics.cpu_queued = m_cpu_requests.size();
		// State is 0=reserved, 1=recording, 2=ready, 3=presenting; -1 means no owner.
		m_diagnostics.head_id = 0;
		m_diagnostics.head_state = -1;
		m_diagnostics.reserved_ns = m_diagnostics.preparing_ns = 0;
		m_diagnostics.prepared_ns = m_diagnostics.ready_ns = 0;
		if (owner != m_requests.end()) {
			m_diagnostics.head_id = owner->id;
			m_diagnostics.head_state = static_cast<int>(owner->state);
			m_diagnostics.reserved_ns = owner->diagnostic_reserved_ns;
			m_diagnostics.preparing_ns = owner->diagnostic_preparing_ns;
			m_diagnostics.prepared_ns = owner->diagnostic_prepared_ns;
			m_diagnostics.ready_ns = owner->diagnostic_ready_ns;
		}
	}
	if (owner == m_requests.end()) {
		if (diagnose) m_diagnostics.no_owner++;
		m_mutex.Unlock();
		return false;
	}
	const uint64_t group = owner->id;
	if (std::any_of(m_cpu_requests.begin(), m_cpu_requests.end(),
	                [group](const auto& r) { return r.group == group; }) ||
	    std::any_of(m_requests.begin(), m_requests.end(), [group](const auto& r) {
		    return r.group == group && r.state != RequestState::Ready;
	    })) {
		if (diagnose) m_diagnostics.not_ready++;
		m_mutex.Unlock();
		return false;
	}
	uint64_t group_buses = 0;
	if (presented_buses != nullptr) {
		for (const auto& request: m_requests) {
			if (request.group != group) { continue; }
			EXIT_IF(request.cfg->bus < 0 || request.cfg->bus >= 64);
			group_buses |= uint64_t {1} << request.cfg->bus;
		}
		if ((*presented_buses & group_buses) != 0) {
			m_mutex.Unlock();
			return false;
		}
	}
	std::array<Request, VideoOutDriver::Impl::VIDEO_OUT_BUSES.size()> requests {};
	size_t count = 0;
	for (auto& request: m_requests) {
		if (request.group == group) {
			EXIT_IF(count == requests.size());
			request.state = RequestState::Presenting;
			requests[count++] = request;
		}
	}
	m_processing = true;
	m_mutex.Unlock();

	// Snapshot the admitted group under its port locks, always acquired in bus order.
	// Presenting keeps its prepared frames owned until completion; Cancel waits for this state.
	std::sort(requests.begin(), requests.begin() + count,
	          [](const auto& a, const auto& b) { return a.cfg->bus < b.cfg->bus; });
	bool due = true;
	uint64_t diagnostic_present_begin_ns = 0, diagnostic_present_end_ns = 0;
	std::array<Graphics::Presenter::Layer, VideoOutDriver::Impl::VIDEO_OUT_BUSES.size()> layers {};
	for (size_t i = 0; i < count; i++) {
		auto& r = requests[i];
		const auto port_lock_begin = diagnose ? DiagnosticNowNs() : 0;
		r.cfg->mutex.Lock();
		if (diagnose) {
			m_diagnostics.port_lock_max_ns = std::max(m_diagnostics.port_lock_max_ns,
			                                        DiagnosticNowNs() - port_lock_begin);
			if (r.hold) m_diagnostics.held++;
		}
		due &= r.cfg->opened && !r.cfg->closing && r.cfg->generation == r.generation;
		if (r.id == group) {
			due &= IsFlipDueLocked(*r.cfg, r.generation);
		}
		layers[i] = {r.frame, r.cfg->bus, r.premultiplied_alpha, r.hold};
	}
	if (async_host) {
		// Clock closes this gate before touching any port and publishes a new epoch only after
		// the complete Begin -> guest flip events -> End transaction. Validate again under both
		// cfg and queue locks so a selection racing the tick cannot mix epoch/count snapshots.
		Common::LockGuard queue_lock(m_mutex);
		due &= !m_async_tick_in_progress && !m_async_paused && m_async_epoch == admission_epoch;
		for (size_t i = 0; i < count; ++i) {
			const auto& r = requests[i];
			due &= r.cfg->async_epoch_generation == r.generation &&
			       r.cfg->async_admitted_epoch != admission_epoch;
		}
		if (due) {
			for (size_t i = 0; i < count; ++i) requests[i].cfg->async_admitted_epoch = admission_epoch;
		}
	}
	if (due) {
		// Host acquire/Reflex/present can block. Neither the captured layers nor their prepared
		// images depend on mutable port registrations, so let the guest submit/prepare its next
		// frame while the host presents this one. Keep m_processing/Presenting throughout.
		for (size_t i = count; i != 0; i--) {
			requests[i - 1].cfg->mutex.Unlock();
		}
		if (diagnose) diagnostic_present_begin_ns = DiagnosticNowNs();
		m_presenter.Present(std::span(layers.data(), count));
		if (diagnose) diagnostic_present_end_ns = DiagnosticNowNs();
		// Reacquire ports before the queue, matching Reserve's cfg -> queue lock order. Close
		// can have invalidated a port while Present ran, but cannot reopen it or ClearLayer
		// until its Cancel has observed this group's completion.
		for (size_t i = 0; i < count; i++) {
			const auto port_lock_begin = diagnose ? DiagnosticNowNs() : 0;
			requests[i].cfg->mutex.Lock();
			if (diagnose) {
				m_diagnostics.port_lock_max_ns = std::max(m_diagnostics.port_lock_max_ns,
				                                        DiagnosticNowNs() - port_lock_begin);
			}
		}
		Profiler::MarkFrame();
		if (presented_buses != nullptr) { *presented_buses |= group_buses; }
	}
	if (diagnose && !due) m_diagnostics.not_due++;

	const auto completion_lock_begin = diagnose ? DiagnosticNowNs() : 0;
	m_mutex.Lock();
	if (diagnose) m_diagnostics.queue_lock_max_ns = std::max(m_diagnostics.queue_lock_max_ns,
	                                                       DiagnosticNowNs() - completion_lock_begin);
	for (auto it = m_requests.begin(); it != m_requests.end();) {
		if (it->group != group) {
			++it;
			continue;
		}
		if (!due) {
			it++->state = RequestState::Ready;
			continue;
		}
		auto& r = *it;
		if (async_host) {
			// Host Present consumed this frame, possibly releasing a held incoming layer. Keep
			// only the guest notification metadata; Cancel must never Discard it a second time.
			r.frame = nullptr;
			r.state = RequestState::CompletedAwaitVblank;
			++it;
			continue;
		}
		if (r.cfg->opened && !r.cfg->closing && r.cfg->generation == r.generation) {
			r.cfg->flip_status.count++;
			r.cfg->flip_status.processTime              = LibKernel::KernelGetProcessTime();
			r.cfg->flip_status.processTimeCounter       = LibKernel::KernelGetProcessTimeCounter();
			r.cfg->flip_status.submitProcessTimeCounter = r.submit_ptc;
			r.cfg->flip_status.flipArg                  = r.flip_arg;
			r.cfg->flip_status.currentBuffer            = r.index;
			r.cfg->flip_status.flipPendingNum--;
			r.cfg->flip_status.gcQueueNum -= r.source == FlipRequestSource::GpuEop;
			TriggerVideoOutEvents(*r.cfg, VideoOutEventKind::Flip, reinterpret_cast<void*>(r.flip_arg));
		}
		// Present already consumed the prepared frame (including a held/discarded incoming
		// layer). Retire even a now-closed generation without Discard or an old flip event.
		// Close's waiting Cancel resets that closed port's pending counts after we wake it.
		it = m_requests.erase(it);
	}
	m_processing = false;
	if (async_host && due) {
		m_async_last_host_epoch = m_async_epoch;
		++m_async_revision;
		m_submit_cond_var.SignalAll();
	}
	m_done_cond_var.SignalAll();
	m_submit_slot_cond_var.SignalAll();
	m_mutex.Unlock();
	for (size_t i = count; i != 0; i--) {
		requests[i - 1].cfg->mutex.Unlock();
	}
	if (due) {
		if (diagnose) {
			for (size_t i = 0; i < count; ++i) {
				const auto& r = requests[i];
				m_pacing_diagnostics.RecordRequest(r.diagnostic_prepared_ns, r.diagnostic_ready_ns,
				                                  diagnostic_present_begin_ns, r.source, r.hold);
			}
			m_pacing_diagnostics.RecordPresent(group, diagnostic_present_begin_ns, diagnostic_present_end_ns);
		}
		Graphics::RenderDocOnGuestFlip(m_presenter.Renderer());
		if (Config::GraphicsDebugDumpEnabled() &&
		    Config::GetPrintfDirection() != Config::LogDirection::Silent) {
			LOGF("Flip done: %d\n", requests[0].index);
		}
	}
	return due;
}

void FlipQueue::GetFlipStatus(VideoOutConfig& cfg, VideoOutFlipStatus& out) {
	Common::LockGuard lock(cfg.mutex);

	out = cfg.flip_status;
}

KYTY_SYSV_ABI int VideoOutSetFlipMaster(int slave_handle, int master_handle) {
	PRINT_NAME();
	return DriverState().SetFlipMaster(slave_handle, master_handle);
}

KYTY_SYSV_ABI void VideoOutAddBufferAttributeOption(VideoOutBufferAttribute2* attribute,
                                                   uint64_t option) {
	PRINT_NAME();
	EXIT_IF(attribute == nullptr);
	attribute->option |= option;
}

KYTY_SYSV_ABI int VideoOutOpen(int user_id, int bus_type, int index, const void* param) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(user_id != 255 && user_id != 0);
	LOGF("\t param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));

	return DriverState().Open(bus_type, index);
}

KYTY_SYSV_ABI int VideoOutClose(int handle) {
	PRINT_NAME();

	return DriverState().Close(handle) ? OK : VIDEO_OUT_ERROR_INVALID_HANDLE;
}

KYTY_SYSV_ABI void VideoOutSetBufferAttribute2(VideoOutBufferAttribute2* attribute,
                                               uint64_t pixel_format, uint32_t tiling_mode,
                                               uint32_t width, uint32_t height, uint64_t option,
                                               uint32_t dcc_control,
                                               uint64_t dcc_cb_register_clear_color) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(attribute == nullptr);

	LOGF("\t pixel_format                = %016" PRIx64 "\n"
	     "\t tiling_mode                 = %" PRIu32 "\n"
	     "\t width                       = %" PRIu32 "\n"
	     "\t height                      = %" PRIu32 "\n"
	     "\t option                      = %016" PRIx64 "\n"
	     "\t dcc_control                 = %08" PRIx32 "\n"
	     "\t dcc_cb_register_clear_color = %016" PRIx64 "\n",
	     pixel_format, tiling_mode, width, height, option, dcc_control,
	     dcc_cb_register_clear_color);

	memset(attribute, 0, sizeof(VideoOutBufferAttribute2));

	attribute->tiling_mode                 = tiling_mode;
	attribute->aspect_ratio                = 0;
	attribute->width                       = width;
	attribute->height                      = height;
	attribute->pitch_in_pixel              = 0;
	attribute->option                      = option;
	attribute->pixel_format                = pixel_format;
	attribute->dcc_cb_register_clear_color = dcc_cb_register_clear_color;
	attribute->dcc_control                 = dcc_control;
}

KYTY_SYSV_ABI int VideoOutSetFlipRate(int handle, int rate) {
	PRINT_NAME();

	LOGF("\trate = %d\n", rate);

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	if (rate < 0 || rate > 2) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	Common::LockGuard lock(ctx->mutex);
	ctx->flip_rate = rate;

	return OK;
}

KYTY_SYSV_ABI int VideoOutDeleteFlipEvent(EventQueue::KernelEqueue eq, int handle) {
	PRINT_NAME();
	return DeleteVideoOutEvent(handle, eq, VideoOutEventKind::Flip);
}

KYTY_SYSV_ABI int VideoOutAddFlipEvent(EventQueue::KernelEqueue eq, int handle, void* udata) {
	PRINT_NAME();
	return RegisterVideoOutEvent(handle, eq, VideoOutEventKind::Flip, udata);
}

KYTY_SYSV_ABI int VideoOutDeleteVblankEvent(EventQueue::KernelEqueue eq, int handle) {
	PRINT_NAME();
	return DeleteVideoOutEvent(handle, eq, VideoOutEventKind::Vblank);
}

KYTY_SYSV_ABI int VideoOutDeletePreVblankStartEvent(EventQueue::KernelEqueue eq, int handle) {
	PRINT_NAME();
	return DeleteVideoOutEvent(handle, eq, VideoOutEventKind::PreVblankStart);
}

KYTY_SYSV_ABI int VideoOutAddVblankEvent(LibKernel::EventQueue::KernelEqueue eq, int handle,
                                         void* udata) {
	PRINT_NAME();
	return RegisterVideoOutEvent(handle, eq, VideoOutEventKind::Vblank, udata);
}

KYTY_SYSV_ABI int VideoOutAddPreVblankStartEvent(LibKernel::EventQueue::KernelEqueue eq, int handle,
                                                 void* udata) {
	PRINT_NAME();
	return RegisterVideoOutEvent(handle, eq, VideoOutEventKind::PreVblankStart, udata);
}

KYTY_SYSV_ABI int VideoOutAddOutputModeEvent(LibKernel::EventQueue::KernelEqueue eq, int handle,
                                             void* udata) {
	PRINT_NAME();
	return RegisterVideoOutEvent(handle, eq, VideoOutEventKind::OutputMode, udata);
}

KYTY_SYSV_ABI int VideoOutRegisterBuffers2(int handle, int set_index, int buffer_index_start,
                                           const VideoOutBuffers* buffers, int buffer_num,
                                           const VideoOutBufferAttribute2* attribute, int category,
                                           void* option) {
	PRINT_NAME();

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	if (buffers == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (attribute == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_OPTION;
	}

	if (set_index < 0 || set_index >= VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX ||
	    buffer_index_start < 0 || buffer_index_start >= VIDEO_OUT_BUFFER_NUM_MAX ||
	    buffer_num < 1 || buffer_num > VIDEO_OUT_BUFFER_NUM_MAX ||
	    buffer_index_start + buffer_num > VIDEO_OUT_BUFFER_NUM_MAX) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	LOGF("\t start_index    = %d\n"
	     "\t buffer_num     = %d\n"
	     "\t set_index      = %d\n"
	     "\t pixel_format   = 0x%016" PRIx64 "\n"
	     "\t tiling_mode    = %" PRIu32 "\n"
	     "\t aspect_ratio   = %" PRIu32 "\n"
	     "\t width          = %" PRIu32 "\n"
	     "\t height         = %" PRIu32 "\n"
	     "\t pitch_in_pixel = %" PRIu32 "\n"
	     "\t option         = %" PRIu64 "\n"
	     "\t category       = %d\n",
	     buffer_index_start, buffer_num, set_index, attribute->pixel_format, attribute->tiling_mode,
	     attribute->aspect_ratio, attribute->width, attribute->height, attribute->pitch_in_pixel,
	     attribute->option, category);

	if (option != nullptr) {
		return VIDEO_OUT_ERROR_INVALID_OPTION;
	}
	bool hmd_output = false;
	{
		Common::LockGuard lock(ctx->mutex);
		hmd_output = ctx->bus == VIDEO_OUT_BUS_TYPE_HMD;
	}
	if ((hmd_output && category != VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_HMD_STEREO) ||
	    (!hmd_output && category != VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_UNCOMPRESSED &&
	     category != VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_COMPRESSED)) {
		return VIDEO_OUT_ERROR_INVALID_CATEGORY;
	}

	BufferAttributeGroup group {
	    .attribute = *attribute,
	    .category  = category,
	    .occupied  = true,
	};
	std::vector<VideoOutBuffer> registrations;
	registrations.reserve(static_cast<size_t>(buffer_num));

	for (int i = 0; i < buffer_num; i++) {
		LOGF("\t buffers[%d]: data=%p metadata=%p\n", i, buffers[i].data, buffers[i].metadata);
		if (buffers[i].reserved[0] != nullptr || buffers[i].reserved[1] != nullptr) {
			LOGF("\t buffers[%d]: ignoring reserved fields {%p, %p}\n", i, buffers[i].reserved[0],
			     buffers[i].reserved[1]);
		}
		auto data_address = reinterpret_cast<uint64_t>(buffers[i].data);
		const auto metadata_address = reinterpret_cast<uint64_t>(buffers[i].metadata);
		if (hmd_output) {
			std::array<Graphics::ShaderTextureResource, 2> eyes;
			if (metadata_address != 0 || !Hmd2::ReadRegisteredStereo(buffers[i].data, eyes)) {
				return VIDEO_OUT_ERROR_INVALID_VALUE;
			}
			// Category 0x10's data pointer is a transient registration list, not pixels.
			// Store only the validated resource address; each actual flip additionally
			// requires a fresh owned SetParamWithBuffer snapshot for this slot.
			data_address = eyes[0].Base40();
		}
		registrations.push_back({
		    .group_index      = set_index,
		    .data_address     = data_address,
		    .metadata_address = metadata_address,
		});
		if (!hmd_output) {
			(void)group.ImageInfo(registrations.back());
		}
	}

	Common::LockGuard lock(ctx->mutex);
	if (ctx->closing) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	if (ctx->groups[set_index].occupied) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}
	for (int i = 0; i < buffer_num; i++) {
		if (ctx->buffers[buffer_index_start + i].Occupied()) {
			return VIDEO_OUT_ERROR_SLOT_OCCUPIED;
		}
	}

	ctx->groups[set_index] = group;
	for (int i = 0; i < buffer_num; i++) {
		ctx->buffers[buffer_index_start + i] = registrations[static_cast<size_t>(i)];
		const auto& buffer                   = registrations[static_cast<size_t>(i)];
		LOGF("\tbuffers[%d] = %016" PRIx64 " metadata = %016" PRIx64 " dcc = %08" PRIx32 "\n",
		     buffer_index_start + i, buffer.data_address, buffer.metadata_address,
		     attribute->dcc_control);
	}

	return OK;
}

KYTY_SYSV_ABI int VideoOutSubmitChangeBufferAttribute2(int handle, int set_index,
                                                       const VideoOutBufferAttribute2* attribute,
                                                       void*                           option) {
	PRINT_NAME();

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	if (attribute == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_OPTION;
	}
	if (set_index < 0 || set_index >= VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}

	if (option != nullptr) {
		return VIDEO_OUT_ERROR_INVALID_OPTION;
	}

	Common::LockGuard lock(ctx->mutex);
	const auto&       current = ctx->groups[set_index];
	if (ctx->closing || !current.occupied) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}
	if (ctx->bus == VIDEO_OUT_BUS_TYPE_HMD) {
		// The observed HMD category has no ordinary framebuffer attribute layout.
		// Require re-registration rather than interpreting its zeroed attribute as TV pixels.
		return VIDEO_OUT_ERROR_INVALID_OPTION;
	}

	BufferAttributeGroup replacement {
	    .attribute = *attribute,
	    .category  = current.category,
	    .occupied  = true,
	};
	for (const auto& buffer: ctx->buffers) {
		if (buffer.group_index == set_index) {
			(void)replacement.ImageInfo(buffer);
		}
	}
	ctx->groups[set_index] = replacement;

	return OK;
}

KYTY_SYSV_ABI int VideoOutUnregisterBuffers(int handle, int set_index) {
	PRINT_NAME();

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	if (set_index < 0 || set_index >= VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}

	Common::LockGuard lock(ctx->mutex);
	if (ctx->closing) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	if (!ctx->groups[set_index].occupied) {
		return VIDEO_OUT_ERROR_INVALID_INDEX;
	}
	ctx->groups[set_index] = BufferAttributeGroup {};
	if (ctx->bus == VIDEO_OUT_BUS_TYPE_HMD) {
		Hmd2::ClearStereoFrames();
	}
	for (auto& buffer: ctx->buffers) {
		if (buffer.group_index == set_index) {
			buffer = VideoOutBuffer {};
		}
	}

	return OK;
}

KYTY_SYSV_ABI int VideoOutSubmitFlip(int handle, int index, int flip_mode, int64_t flip_arg) {
	const auto trace_begin = Common::SourceTrace::Enabled() ? Common::SourceTrace::NowNs() : 0;
	PRINT_NAME();

	uint64_t  request_id = 0;
	const int result     = ReserveFlipRequest(DriverState(), handle, index, flip_mode, flip_arg,
	                                          FlipRequestSource::Cpu, request_id);
	const auto trace_reserved = trace_begin != 0 ? Common::SourceTrace::NowNs() : 0;
	if (result == VIDEO_OUT_ERROR_INVALID_FLIP_MODE) {
		LOGF("\t unsupported flip_mode = %d\n", flip_mode);
	}
	if (result != OK) {
		return result;
	}
	Graphics::Timeline::Mark("flip-cpu", static_cast<uint64_t>(index), request_id);
	g_video_out_driver->SubmitFlipPreparation(request_id);
	if (trace_begin != 0) {
		Common::SourceTrace::Event event {};
		event.kind = Common::SourceTrace::Kind::FlipSubmit;
		event.begin_ns = trace_begin;
		event.work_end_ns = trace_reserved;
		event.end_ns = Common::SourceTrace::NowNs();
		event.request_id = request_id;
		event.arg0 = static_cast<uint64_t>(handle);
		event.arg1 = static_cast<uint64_t>(index);
		Common::SourceTrace::Record(event);
	}

	return OK;
}

int VideoOutDriver::SubmitFlipFromGpu(Graphics::CommandBuffer& buffer, int handle, int index,
                                      int flip_mode, int64_t flip_arg, uint64_t& request_id) {
	EXIT_IF(buffer.IsInvalid());

	const int result = ReserveFlipRequest(*m_impl, handle, index, flip_mode, flip_arg,
	                                      FlipRequestSource::GpuEop, request_id);
	if (result != OK) {
		return result;
	}
	Graphics::Timeline::Mark("flip-eop", static_cast<uint64_t>(index), request_id);
	m_impl->GetFlipQueue().Prepare(request_id, buffer);

	return OK;
}

void VideoOutDriver::SubmitFlipPreparation(uint64_t request_id) {
	m_impl->Renderer().GetGpu().SubmitFlipPreparation(request_id);
}

void VideoOutDriver::PrepareFlip(uint64_t request_id, Graphics::CommandBuffer& buffer) {
	m_impl->GetFlipQueue().Prepare(request_id, buffer);
}

void VideoOutDriver::CompleteFlip(uint64_t request_id) {
	m_impl->GetFlipQueue().Complete(request_id);
}

void VideoOutDriver::WaitForSubmitSlot(int handle) {
	auto* cfg = m_impl->Get(handle);
	EXIT_IF(cfg == nullptr);
	Graphics::Timeline::Mark("flip-slot-wait", static_cast<uint64_t>(handle));
	m_impl->GetFlipQueue().WaitForSubmitSlot(*cfg);
	Graphics::Timeline::Mark("flip-slot-done", static_cast<uint64_t>(handle));
}

void VideoOutDriver::WaitFlipDone(int handle, int index) {
	auto* ctx = m_impl->Get(handle);
	EXIT_IF(ctx == nullptr);

	EXIT_NOT_IMPLEMENTED(!IsValidBufferIndex(index));
	m_impl->GetFlipQueue().Wait(*ctx, index);
}

KYTY_SYSV_ABI int VideoOutGetFlipStatus(int handle, VideoOutFlipStatus* status) {
	PRINT_NAME();

	if (status == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	DriverState().GetFlipQueue().GetFlipStatus(*ctx, *status);

	LOGF("\t count = %" PRIu64 "\n"
	     "\t processTime = %" PRIu64 "\n"
	     "\t processTimeCounter = %" PRIu64 "\n"
	     "\t submitProcessTimeCounter = %" PRIu64 "\n"
	     "\t flipArg = %" PRId64 "\n"
	     "\t gcQueueNum = %d\n"
	     "\t flipPendingNum = %d\n"
	     "\t currentBuffer = %d\n",
	     status->count, status->processTime, status->processTimeCounter,
	     status->submitProcessTimeCounter, status->flipArg, status->gcQueueNum,
	     status->flipPendingNum, status->currentBuffer);

	return OK;
}

KYTY_SYSV_ABI int VideoOutIsFlipPending(int handle) {
	PRINT_NAME();

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	VideoOutFlipStatus status {};
	DriverState().GetFlipQueue().GetFlipStatus(*ctx, status);

	if (Config::GraphicsDebugDumpEnabled()) {
		LOGF("\t flipPendingNum = %d\n", status.flipPendingNum);
	}

	return status.flipPendingNum;
}

KYTY_SYSV_ABI int VideoOutGetVblankStatus(int handle, VideoOutVblankStatus* status) {
	PRINT_NAME();

	if (status == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	ctx->mutex.Lock();
	*status = ctx->vblank_status;
	ctx->mutex.Unlock();

	LOGF("\t count = %" PRIu64 "\n"
	     "\t processTime = %" PRIu64 "\n"
	     "\t processTimeCounter = %" PRIu64 "\n",
	     status->count, status->processTime, status->processTimeCounter);

	return OK;
}

KYTY_SYSV_ABI int VideoOutGetEventId(const EventQueue::KernelEvent* ev) {
	PRINT_NAME();

	if (ev == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (ev->filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT) {
		return VIDEO_OUT_ERROR_INVALID_EVENT;
	}

	switch (ev->ident) {
		case VIDEO_OUT_EVENT_FLIP:
		case VIDEO_OUT_EVENT_VBLANK:
		case VIDEO_OUT_EVENT_PRE_VBLANK_START:
		case VIDEO_OUT_EVENT_SET_MODE: return static_cast<int>(ev->ident);
		default: return VIDEO_OUT_ERROR_INVALID_EVENT;
	}
}

KYTY_SYSV_ABI int VideoOutGetEventData(const EventQueue::KernelEvent* ev, int64_t* data) {
	PRINT_NAME();

	if (ev == nullptr || data == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (ev->filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT) {
		return VIDEO_OUT_ERROR_INVALID_EVENT;
	}

	uint64_t event_data = static_cast<uint64_t>(ev->data) >> 16u;
	if (ev->ident == VIDEO_OUT_EVENT_FLIP &&
	    (static_cast<uint64_t>(ev->data) & 0x8000000000000000ULL) != 0) {
		event_data |= 0xffff000000000000ULL;
	}

	*data = static_cast<int64_t>(event_data);

	return OK;
}

KYTY_SYSV_ABI int VideoOutGetEventCount(const EventQueue::KernelEvent* ev) {
	PRINT_NAME();

	if (ev == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (ev->filter != EventQueue::KERNEL_EVFILT_VIDEO_OUT) {
		return VIDEO_OUT_ERROR_INVALID_EVENT;
	}

	return static_cast<int>((static_cast<uint64_t>(ev->data) >> 12u) & 0xfu);
}

KYTY_SYSV_ABI int VideoOutWaitVblank(int handle) {
	PRINT_NAME();

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	Common::LockGuard lock(ctx->mutex);
	const auto        count = ctx->vblank_status.count;
	while (ctx->opened && ctx->vblank_status.count == count) {
		ctx->vblank_cond.Wait(&ctx->mutex);
	}

	return OK;
}

KYTY_SYSV_ABI int VideoOutGetOutputStatus(int handle, VideoOutOutputStatus* status) {
	PRINT_NAME();

	if (status == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	int32_t attribute3 = 0;
	Loader::SystemContentParamSfoGetInt("ATTRIBUTE3", &attribute3);
	ctx->mutex.Lock();
	// Primary output reports 4K unless param.json Video-out Info enables resolution detection.
	status->resolution =
	    ((attribute3 & 4) != 0 && ctx->width < 3840 && ctx->height < 2160 ? 1u : 2u);
	status->dynamicRange = 1;
	status->refreshRate =
	    (ctx->output_mode == VIDEO_OUT_OUTPUT_MODE_119_88HZ || Config::GetVblankFrequency() >= 119
	         ? VIDEO_OUT_REFRESH_RATE_119_88HZ
	         : VIDEO_OUT_REFRESH_RATE_59_94HZ);
	status->flags       = 0;
	status->reserved[0] = 0;
	status->reserved[1] = 0;
	status->reserved[2] = 0;
	ctx->mutex.Unlock();

	return OK;
}

KYTY_SYSV_ABI int VideoOutAllowOutputResolutionWqhdDetection(int handle) {
	if (!DriverState().IsOpened(handle)) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	return OK;
}

KYTY_SYSV_ABI int VideoOutVrrPegToFixedRate(int handle, uint64_t arg1, uint64_t arg2) {
	PRINT_NAME();

	static std::atomic_bool logged {false};
	if (!logged.exchange(true, std::memory_order_relaxed)) {
		LOGF("\t handle = %d\n"
		     "\t arg1   = 0x%016" PRIx64 "\n"
		     "\t arg2   = 0x%016" PRIx64 "\n",
		     handle, arg1, arg2);
	}

	return OK;
}

KYTY_SYSV_ABI int VideoOutVrrUnpegFromFixedRate(int handle) {
	PRINT_NAME();

	static std::atomic_bool logged {false};
	if (!logged.exchange(true, std::memory_order_relaxed)) {
		LOGF("\t handle = %d\n", handle);
	}

	return OK;
}

static int ValidateOutputConfig(int handle, uint64_t mode, const VideoOutOutputOptions* options,
                                void* reserved_ptr, uint64_t reserved) {
	if (!DriverState().IsOpened(handle)) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	if (reserved_ptr != nullptr || reserved != 0) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	if (options != nullptr) {
		for (auto v: options->internalData) {
			if (v != 0) {
				return VIDEO_OUT_ERROR_INVALID_OPTION;
			}
		}
	}

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	Common::LockGuard lock(ctx->mutex);
	const bool hmd_mode = mode == Hmd2::OUTPUT_MODE_89_91HZ || mode == Hmd2::OUTPUT_MODE_119_88HZ;
	if (ctx->bus == VIDEO_OUT_BUS_TYPE_HMD ? !hmd_mode :
	    (mode != VIDEO_OUT_OUTPUT_MODE_DEFAULT && mode != VIDEO_OUT_OUTPUT_MODE_119_88HZ)) {
		return VIDEO_OUT_ERROR_UNSUPPORTED_OUTPUT_MODE;
	}

	return OK;
}

KYTY_SYSV_ABI int VideoOutInitializeOutputOptions(VideoOutOutputOptions* options) {
	PRINT_NAME();

	if (options == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	memset(options, 0, sizeof(VideoOutOutputOptions));

	return OK;
}

KYTY_SYSV_ABI int VideoOutIsOutputSupported(int handle, uint64_t mode,
                                            const VideoOutOutputOptions* options,
                                            void* reserved_ptr, uint64_t reserved) {
	PRINT_NAME();

	LOGF("\t mode = 0x%016" PRIx64 "\n", mode);

	int result = ValidateOutputConfig(handle, mode, options, reserved_ptr, reserved);
	if (result != OK) {
		return result;
	}

	if (mode == VIDEO_OUT_OUTPUT_MODE_119_88HZ) {
		return (Config::GetVblankFrequency() >= 119 ? VIDEO_OUT_TRUE : VIDEO_OUT_FALSE);
	}
	if (mode == Hmd2::OUTPUT_MODE_89_91HZ || mode == Hmd2::OUTPUT_MODE_119_88HZ) {
		// These identify the guest HMD source mode. SteamVR controls actual headset
		// scanout timing; accepting a guest mode does not force the host panel rate.
		return Config::VrEnabled() && Graphics::Vr::HostBridge().Active()
		           ? VIDEO_OUT_TRUE : VIDEO_OUT_FALSE;
	}

	return VIDEO_OUT_TRUE;
}

KYTY_SYSV_ABI int VideoOutConfigureOutput(int handle, uint64_t mode,
                                          const VideoOutOutputOptions* options, void* reserved_ptr,
                                          uint64_t reserved) {
	PRINT_NAME();

	LOGF("\t mode = 0x%016" PRIx64 "\n", mode);

	int result = VideoOutIsOutputSupported(handle, mode, options, reserved_ptr, reserved);
	if (result < 0) {
		return result;
	}
	if (result == VIDEO_OUT_FALSE) {
		return VIDEO_OUT_ERROR_UNAVAILABLE_OUTPUT_MODE;
	}

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	ctx->mutex.Lock();
	ctx->output_mode = mode;
	TriggerVideoOutEvents(*ctx, VideoOutEventKind::OutputMode,
	                      reinterpret_cast<void*>(ctx->output_mode));
	ctx->mutex.Unlock();

	return OK;
}

KYTY_SYSV_ABI int VideoOutSetWindowModeMargins(int handle, int top, int bottom) {
	PRINT_NAME();

	[[maybe_unused]] auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	LOGF("\t top    = %d\n"
	     "\t bottom = %d\n",
	     top, bottom);

	return OK;
}

KYTY_SYSV_ABI int VideoOutLatencyControlWaitBeforeInput(int handle) {
	PRINT_NAME();

	[[maybe_unused]] auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	return OK;
}

KYTY_SYSV_ABI int VideoOutLatencyMeasureSetStartPoint(int handle, uint32_t point) {
	PRINT_NAME();

	[[maybe_unused]] auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	LOGF("\t point = %" PRIu32 "\n", point);

	return OK;
}

KYTY_SYSV_ABI int VideoOutColorSettingsSetGamma(VideoOutColorSettings* settings, float gamma) {
	PRINT_NAME();

	if (settings == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (gamma < 0.1f || gamma > 2.0f) {
		return VIDEO_OUT_ERROR_INVALID_VALUE;
	}

	settings->gamma = gamma;
	return OK;
}

KYTY_SYSV_ABI int VideoOutAdjustColor(int handle, const VideoOutColorSettings* settings) {
	PRINT_NAME();

	if (settings == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_ADDRESS;
	}

	if (!DriverState().IsOpened(handle)) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}

	auto* ctx = DriverState().Get(handle);
	if (ctx == nullptr) {
		return VIDEO_OUT_ERROR_INVALID_HANDLE;
	}
	ctx->mutex.Lock();
	ctx->gamma = settings->gamma;
	ctx->mutex.Unlock();

	return OK;
}

} // namespace Libs::VideoOut
