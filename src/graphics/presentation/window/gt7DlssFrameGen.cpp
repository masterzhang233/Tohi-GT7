#include "graphics/presentation/window/gt7DlssFrameGen.h"
#include "graphics/presentation/window/gt7DlssFrameGenCamera.h"
#include "common/emulatorConfig.h"
#include "graphics/host_gpu/renderer/dlss.h"
#include "graphics/host_gpu/renderer/gt7FrameGenLifecycle.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_vulkan.h>

#if defined(_WIN32) && defined(KYTY_GT7_FRAMEGEN_DLSS)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
// Do not define VK_USE_PLATFORM_WIN32_KHR for just this translation unit: that
// would change Vulkan-Hpp's shared dynamic dispatcher layout. Only the raw C
// surface structures/function pointer are needed for the interposer call.
#include <vulkan/vulkan_win32.h>
#include <filesystem>
#include <map>
#include <string>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include "graphics/presentation/window/gt7DlssFrameGenPolicy.h"
#define GT7_HAS_DLSSG_RUNTIME 1
#endif

namespace Libs::Graphics::Gt7DlssFrameGen {
namespace {

std::atomic<StatusReason> g_reason {StatusReason::Off};
std::atomic<bool> g_ready {false};
std::atomic<uint32_t> g_discontinuity {Gt7FrameGenLifecycle::Initial};
std::atomic<uint64_t> g_sdk_presented {0}, g_application_presents {0}, g_generated {0};
std::atomic<uint64_t> g_present_attempts {0};
std::atomic<uint32_t> g_sdk_status {0};
std::atomic<uint32_t> g_last_presented_count {0};
std::atomic<uint32_t> g_max_generated_frames {0};
std::atomic<int64_t> g_last_generated {0};

int64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

#ifdef GT7_HAS_DLSSG_RUNTIME
using namespace Gt7DlssFrameGenCamera;
constexpr sl::Feature Features[] {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
const sl::ViewportHandle Viewport {0u};

uint64_t NowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Timing {
    uint64_t count = 0, total_ns = 0, max_ns = 0, over_100ms = 0;
    void Add(uint64_t duration) {
        ++count;
        total_ns += duration;
        max_ns = std::max(max_ns, duration);
        over_100ms += duration >= 100000000u;
    }
    double MeanMs() const { return count ? double(total_ns)/double(count)/1.0e6 : 0.0; }
    double MaxMs() const { return double(max_ns)/1.0e6; }
};

struct Measure {
    Timing& timing;
    uint64_t started = NowNs();
    ~Measure() { timing.Add(NowNs()-started); }
};

struct State {
    std::mutex mutex;
    bool tried = false, initialized = false, options_on = false, frame_valid = false, frame_reset = true;
    bool generation_was_enabled = false;
    bool frame_pending_prepare = false;
    bool swapchain_reflex_ready = false;
    HMODULE module = nullptr;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
    PFun_slInit* init = nullptr;
    PFun_slShutdown* shutdown = nullptr;
    PFun_slIsFeatureSupported* is_supported = nullptr;
    PFun_slGetFeatureRequirements* get_requirements = nullptr;
    PFun_slGetFeatureFunction* feature_function = nullptr;
    PFun_slGetNewFrameToken* new_token = nullptr;
    PFun_slSetConstants* set_constants = nullptr;
    PFun_slSetTagForFrame* set_tags = nullptr;
    PFun_slFreeResources* free_resources = nullptr;
    PFun_slDLSSGSetOptions* set_options = nullptr;
    PFun_slDLSSGGetState* get_state = nullptr;
    PFun_slReflexSetOptions* reflex_options = nullptr;
    PFun_slReflexSleep* reflex_sleep = nullptr;
    PFun_slPCLSetMarker* marker = nullptr;
    sl::FrameToken* frame = nullptr;
    uint32_t frame_index = 0;
    uint64_t previous_guest = 0;
    Matrix previous_view_projection {};
    vk::Extent2D previous_render {}, previous_display {};
    bool previous_valid = false;
    std::wstring runtime_directory, log_directory;
    const wchar_t* plugin_path = nullptr;
    std::vector<std::string> instance_extensions, device_extensions;
    // A FramePool owns the images; views exist only until its explicit drain.
    std::map<VkImage, vk::ImageView> views;
    vk::Device device = nullptr;
    int64_t next_report = 0;
    int64_t next_timing_report = 0;
    uint64_t present_begin_ns = 0, previous_present_begin_ns = 0;
    Timing sleep_timing, prepare_timing, present_timing, interval_timing;
    Timing options_timing, tags_timing, clear_timing, query_timing;
    uint64_t enabled_count = 0, disabled_count = 0, reset_count = 0, drain_count = 0;
    Gt7FrameGenLifecycle::ResetDiagnostics reset_diagnostics;
};
State g;

void ReportTiming(bool force = false) {
    if ((!force && Now() < g.next_timing_report) || !g.present_timing.count) return;
    g.next_timing_report = Now()+5000;
    // Aggregate CPU wall time only. Present interval includes acquisition,
    // producer work and pacing; it is not a GPU execution-time measurement.
    std::printf("GT7 DLSS FG timing: samples=%llu ms(avg/max) sleep=%.3f/%.3f prepare=%.3f/%.3f "
                "present=%.3f/%.3f interval=%.3f/%.3f options=%.3f/%.3f tags=%.3f/%.3f "
                "clear=%.3f/%.3f state=%.3f/%.3f over100ms(sleep/prepare/present/interval)=%llu/%llu/%llu/%llu "
                "on=%llu off=%llu resets=%llu drains=%llu\n",
        static_cast<unsigned long long>(g.present_timing.count),
        g.sleep_timing.MeanMs(), g.sleep_timing.MaxMs(), g.prepare_timing.MeanMs(), g.prepare_timing.MaxMs(),
        g.present_timing.MeanMs(), g.present_timing.MaxMs(), g.interval_timing.MeanMs(), g.interval_timing.MaxMs(),
        g.options_timing.MeanMs(), g.options_timing.MaxMs(), g.tags_timing.MeanMs(), g.tags_timing.MaxMs(),
        g.clear_timing.MeanMs(), g.clear_timing.MaxMs(), g.query_timing.MeanMs(), g.query_timing.MaxMs(),
        static_cast<unsigned long long>(g.sleep_timing.over_100ms),
        static_cast<unsigned long long>(g.prepare_timing.over_100ms),
        static_cast<unsigned long long>(g.present_timing.over_100ms),
        static_cast<unsigned long long>(g.interval_timing.over_100ms),
        static_cast<unsigned long long>(g.enabled_count), static_cast<unsigned long long>(g.disabled_count),
        static_cast<unsigned long long>(g.reset_count), static_cast<unsigned long long>(g.drain_count));
    g.sleep_timing = {}; g.prepare_timing = {}; g.present_timing = {}; g.interval_timing = {};
    g.options_timing = {}; g.tags_timing = {}; g.clear_timing = {}; g.query_timing = {};
    g.enabled_count = g.disabled_count = g.reset_count = g.drain_count = 0;
}

void Log(sl::LogType type, const char* message) {
    if (type == sl::LogType::eError || type == sl::LogType::eWarn)
        std::printf("GT7 DLSS FG Streamline: %s\n", message ? message : "");
}

bool TrustedNvidiaDll(const std::filesystem::path& path) {
    WINTRUST_FILE_INFO file {};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA trust {};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = WinVerifyTrust(nullptr, &policy, &trust);
    bool nvidia = false;
    if (result == ERROR_SUCCESS) {
        const auto* data = WTHelperProvDataFromStateData(trust.hWVTStateData);
        const auto* signer = data ? WTHelperGetProvSignerFromChain(
            const_cast<CRYPT_PROVIDER_DATA*>(data), 0, FALSE, 0) : nullptr;
        if (signer && signer->csCertChain && signer->pasCertChain[0].pCert) {
            wchar_t subject[256] {};
            CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE,
                               0, nullptr, subject, 256);
            nvidia = std::wstring_view(subject) == L"NVIDIA Corporation";
        }
    }
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &trust);
    if (!nvidia) std::printf("GT7 DLSS FG: rejected unsigned/non-NVIDIA runtime %ls (trust=0x%08lx)\n",
                             path.filename().c_str(), static_cast<unsigned long>(result));
    return nvidia;
}

template <typename Function>
Function* Feature(sl::Feature feature, const char* name) {
    void* address = nullptr;
    if (!g.feature_function || g.feature_function(feature, name, address) != sl::Result::eOk) return nullptr;
    return reinterpret_cast<Function*>(address);
}

void Report(const char* operation, sl::Result result) {
    g_reason.store(StatusReason::RuntimeError);
    g_discontinuity.fetch_or(Gt7FrameGenLifecycle::RuntimeFailure);
    if (Now() >= g.next_report) {
        g.next_report = Now() + 5000;
        std::printf("GT7 DLSS FG: %s failed (Streamline result %d); native generation suspended\n",
                    operation, static_cast<int>(result));
    }
}

bool SetMode(bool enabled) {
    if (!g.set_options) return false;
    if (enabled && !Gt7DlssFrameGenPolicy::SupportsFixed2x(g_max_generated_frames.load())) {
        g_reason.store(StatusReason::Fixed2xUnsupported);
        return false;
    }
    if (g.options_on == enabled) return true;
    const auto options = Gt7DlssFrameGenPolicy::Options(enabled);
    const auto begin = NowNs();
    const auto result = g.set_options(Viewport, options);
    g.options_timing.Add(NowNs()-begin);
    if (result != sl::Result::eOk) { Report("slDLSSGSetOptions", result); return false; }
    g.options_on = enabled;
    if (enabled) {
        ++g.enabled_count;
        g.generation_was_enabled = true;
    }
    else ++g.disabled_count;
    return true;
}

void ClearTags() {
    if (!g.frame || !g.set_tags) return;
    const Measure measure {g.clear_timing};
    sl::ResourceTag tags[] {
        {nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent},
        {nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent},
        {nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent},
        {nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent},
    };
    const auto result = g.set_tags(*g.frame, Viewport, tags, static_cast<uint32_t>(std::size(tags)), nullptr);
    if (result != sl::Result::eOk) Report("clear tags", result);
}

void Suspend(StatusReason reason) {
    g.frame_valid = false;
    g.previous_valid = false;
    g_discontinuity.fetch_or(Gt7FrameGenLifecycle::Suspended);
    const bool disabled = SetMode(false);
    ClearTags();
    if (disabled) g_reason.store(reason);
}

void Marker(sl::PCLMarker marker) {
    if (g.marker && g.frame) {
        const auto result = g.marker(marker, *g.frame);
        if (result != sl::Result::eOk) Report("Reflex marker", result);
    }
}

vk::ImageView View(vk::Image image, vk::Format format) {
    auto found = g.views.find(static_cast<VkImage>(image));
    if (found != g.views.end()) return found->second;
    const vk::ImageViewCreateInfo info {
        .image = image, .viewType = vk::ImageViewType::e2D, .format = format,
        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1},
    };
    const auto created = g.device.createImageView(info);
    if (created.result != vk::Result::eSuccess) return nullptr;
    g.views.emplace(static_cast<VkImage>(image), created.value);
    return created.value;
}

sl::Resource Resource(const VulkanImage& image, vk::ImageView view) {
    sl::Resource resource {sl::ResourceType::eTex2d, static_cast<VkImage>(image.image), nullptr,
                           static_cast<VkImageView>(view), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    resource.width = image.extent.width;
    resource.height = image.extent.height;
    resource.nativeFormat = static_cast<uint32_t>(image.format);
    resource.mipLevels = image.mip_levels;
    resource.arrayLayers = image.layers;
    resource.flags = static_cast<VkImageCreateFlags>(image.flags);
    resource.usage = static_cast<VkImageUsageFlags>(image.usage);
    return resource;
}

bool ValidImage(const VulkanImage& image, vk::Extent2D size, vk::Format format) {
    return image.image && image.format == format && image.extent.width == size.width &&
           image.extent.height == size.height && image.extent.depth == 1 &&
           image.layers == 1 && image.mip_levels == 1 && image.samples == 1 &&
           (image.usage & vk::ImageUsageFlagBits::eSampled) &&
           (image.usage & vk::ImageUsageFlagBits::eTransferSrc);
}

sl::float4x4 SlMatrix(const Matrix& matrix) {
    sl::float4x4 result;
    for (uint32_t row = 0; row < 4; ++row)
        result.setRow(row, sl::float4(float(matrix[row*4]), float(matrix[row*4+1]),
                                      float(matrix[row*4+2]), float(matrix[row*4+3])));
    return result;
}

bool Constants(const Gt7FrameGen::FrameInputs& inputs, bool reset, sl::Constants& constants,
               Matrix& current_vp) {
    Camera camera;
    for (size_t i = 0; i < 3; ++i) {
        camera.position[i] = inputs.camera_position[i]; camera.right[i] = inputs.camera_right[i];
        camera.up[i] = inputs.camera_up[i]; camera.forward[i] = inputs.camera_forward[i];
    }
    camera.near_plane = inputs.near_plane; camera.far_plane = inputs.far_plane;
    camera.fov_y = inputs.fov_y; camera.infinite = inputs.depth_infinite;
    camera.aspect = float(inputs.render_extent.width)/float(inputs.render_extent.height);
    Matrix view, projection, inverse_projection, inverse_current_vp;
    if (!Build(camera, view, projection) || !Inverse(projection, inverse_projection)) return false;
    current_vp = Multiply(view, projection);
    if (!Inverse(current_vp, inverse_current_vp)) return false;
    const auto to_previous = reset ? Identity() : Multiply(inverse_current_vp, g.previous_view_projection);
    Matrix to_current;
    if (!Inverse(to_previous, to_current)) return false;
    constants.cameraViewToClip = SlMatrix(projection);
    constants.clipToCameraView = SlMatrix(inverse_projection);
    constants.clipToLensClip = SlMatrix(Identity());
    constants.clipToPrevClip = SlMatrix(to_previous);
    constants.prevClipToClip = SlMatrix(to_current);
    constants.jitterOffset = {inputs.jitter_x, inputs.jitter_y};
    constants.mvecScale = Gt7DlssFrameGenPolicy::MotionScale(inputs.render_extent.width, inputs.render_extent.height);
    constants.cameraPinholeOffset = {0, 0};
    constants.cameraPos = {camera.position[0], camera.position[1], camera.position[2]};
    constants.cameraRight = {camera.right[0], camera.right[1], camera.right[2]};
    constants.cameraUp = {camera.up[0], camera.up[1], camera.up[2]};
    constants.cameraFwd = {camera.forward[0], camera.forward[1], camera.forward[2]};
    constants.cameraNear = camera.near_plane;
    constants.cameraFar = camera.infinite ? std::numeric_limits<float>::max() : camera.far_plane;
    constants.cameraFOV = camera.fov_y;
    constants.cameraAspectRatio = camera.aspect;
    constants.depthInverted = sl::Boolean::eTrue;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    return true;
}

void QueryState() {
    if (!g.get_state) return;
    sl::DLSSGState state {};
    const auto begin = NowNs();
    const auto result = g.get_state(Viewport, state, nullptr);
    g.query_timing.Add(NowNs()-begin);
    if (result != sl::Result::eOk) {
        Report("slDLSSGGetState", result);
        Suspend(StatusReason::RuntimeError);
        return;
    }
    g_sdk_status.store(static_cast<uint32_t>(state.status));
    g_max_generated_frames.store(state.numFramesToGenerateMax);
    g_last_presented_count.store(state.numFramesActuallyPresented);
    const auto total = g_sdk_presented.fetch_add(state.numFramesActuallyPresented) + state.numFramesActuallyPresented;
    const auto real = g_application_presents.load();
    const auto attempts = g_present_attempts.load();
    const auto extra = Gt7DlssFrameGenPolicy::GeneratedPresentationsLowerBound(
        total, attempts, g.generation_was_enabled);
    const auto previous = g_generated.load();
    if (extra > previous) {
        g_generated.store(extra);
    }
    // The pacer is asynchronous: one poll can include multiple older real
    // frames. Only a growing cumulative excess proves generated presentations;
    // a sample of 2 by itself does not. Reset frames must never appear active.
    if (!Gt7DlssFrameGenPolicy::SupportsFixed2x(state.numFramesToGenerateMax)) {
        Suspend(StatusReason::Fixed2xUnsupported);
    }
    if (g.frame_valid && !g.frame_reset && g.options_on &&
        state.status == sl::DLSSGStatus::eOk && extra > previous) {
        g_last_generated.store(Now());
        g_reason.store(StatusReason::Active);
    }
    if (state.status != sl::DLSSGStatus::eOk && g.options_on) {
        Suspend(StatusReason::RuntimeError);
    }
    if (Now() >= g.next_report) {
        g.next_report = Now()+5000;
        std::printf("GT7 DLSS FG: reason=%s sdk_status=0x%x last_presented=%u presented=%llu application=%llu attempts=%llu generated_min=%llu max_generated=%u requested_generated=%u mode=fixed2x\n",
            StatusReasonText(g_reason.load()), static_cast<unsigned>(state.status),
            state.numFramesActuallyPresented,
            static_cast<unsigned long long>(total), static_cast<unsigned long long>(real),
            static_cast<unsigned long long>(attempts),
            static_cast<unsigned long long>(g_generated.load()), state.numFramesToGenerateMax,
            Gt7DlssFrameGenPolicy::GeneratedFrames);
    }
}

#endif // GT7_HAS_DLSSG_RUNTIME
} // namespace

bool Requested() {
#ifndef GT7_HAS_DLSSG_RUNTIME
    // Unsupported builds must not select FG capture or presentation paths,
    // even when an inherited environment requests them.
    return false;
#else
    // Capture and RecordPrepare validate each frame's actual inputs, including
    // NativePSSR. The opt-in alone never marks interpolation as active.
    static const bool requested = [] {
        const char* value = std::getenv("KYTY_GT7_DLSS_FG");
        return value && std::string_view(value) == "1";
    }();
    return requested;
#endif
}

PFN_vkGetInstanceProcAddr Initialize(PFN_vkGetInstanceProcAddr real_proc_addr) {
    if (!Requested()) return real_proc_addr;
#ifdef GT7_HAS_DLSSG_RUNTIME
    std::scoped_lock lock(g.mutex);
    if (g.tried) return g.get_instance_proc_addr ? g.get_instance_proc_addr : real_proc_addr;
    g.tried = true;
    g_reason.store(StatusReason::Loading);
    try {
        wchar_t executable[32768] {};
        const auto length = GetModuleFileNameW(nullptr, executable, 32768);
        if (!length || length >= 32768) { g_reason.store(StatusReason::RuntimeMissing); return real_proc_addr; }
        const auto directory = std::filesystem::path(executable).parent_path();
        wchar_t configured[32768] {};
        const auto configured_length = GetEnvironmentVariableW(L"KYTY_GT7_DLSS_FG_RUNTIME", configured, 32768);
        auto runtime = configured_length && configured_length < 32768
            ? std::filesystem::path(configured) : directory/L"streamline";
        if (!runtime.is_absolute() || !std::filesystem::is_directory(runtime)) {
            std::printf("GT7 DLSS FG: runtime must be an existing absolute directory\n");
            g_reason.store(StatusReason::RuntimeMissing); return real_proc_addr;
        }
        runtime = std::filesystem::canonical(runtime);
        constexpr const wchar_t* required[] {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss_g.dll",
            L"sl.reflex.dll", L"sl.pcl.dll", L"nvngx_dlssg.dll", L"NvLowLatencyVk.dll"};
        for (const auto* file : required) {
            if (!std::filesystem::is_regular_file(runtime/file)) {
                std::printf("GT7 DLSS FG: runtime file missing: %ls\n", file);
                g_reason.store(StatusReason::RuntimeMissing); return real_proc_addr;
            }
            if (!TrustedNvidiaDll(runtime/file)) { g_reason.store(StatusReason::RuntimeUntrusted); return real_proc_addr; }
        }
        g.runtime_directory = runtime.wstring();
        g.log_directory = (std::filesystem::current_path()/L"streamline-logs").wstring();
        std::filesystem::create_directories(g.log_directory);
        g.plugin_path = g.runtime_directory.c_str();
        g.module = LoadLibraryExW((runtime/L"sl.interposer.dll").c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!g.module) {
            std::printf("GT7 DLSS FG: interposer load failed (Windows %lu)\n", GetLastError());
            g_reason.store(StatusReason::RuntimeMissing); return real_proc_addr;
        }
#define GT7_SL_LOAD(member, type, name) g.member = reinterpret_cast<type*>(GetProcAddress(g.module, name))
        GT7_SL_LOAD(init, PFun_slInit, "slInit");
        GT7_SL_LOAD(shutdown, PFun_slShutdown, "slShutdown");
        GT7_SL_LOAD(is_supported, PFun_slIsFeatureSupported, "slIsFeatureSupported");
        GT7_SL_LOAD(get_requirements, PFun_slGetFeatureRequirements, "slGetFeatureRequirements");
        GT7_SL_LOAD(feature_function, PFun_slGetFeatureFunction, "slGetFeatureFunction");
        GT7_SL_LOAD(new_token, PFun_slGetNewFrameToken, "slGetNewFrameToken");
        GT7_SL_LOAD(set_constants, PFun_slSetConstants, "slSetConstants");
        GT7_SL_LOAD(set_tags, PFun_slSetTagForFrame, "slSetTagForFrame");
        GT7_SL_LOAD(free_resources, PFun_slFreeResources, "slFreeResources");
#undef GT7_SL_LOAD
        const auto proxy = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(g.module, "vkGetInstanceProcAddr"));
        if (!g.init || !g.shutdown || !g.is_supported || !g.get_requirements || !g.feature_function ||
            !g.new_token || !g.set_constants || !g.set_tags || !g.free_resources || !proxy) {
            g_reason.store(StatusReason::RuntimeError); return real_proc_addr;
        }
        sl::Preferences preferences {};
        preferences.showConsole = false;
        preferences.logLevel = sl::LogLevel::eDefault;
        preferences.logMessageCallback = Log;
        preferences.pathsToPlugins = &g.plugin_path;
        preferences.numPathsToPlugins = 1;
        preferences.pathToLogsAndData = g.log_directory.c_str();
        preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
            sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
        preferences.featuresToLoad = Features;
        preferences.numFeaturesToLoad = static_cast<uint32_t>(std::size(Features));
        preferences.engine = sl::EngineType::eCustom;
        preferences.engineVersion = "Tohi-GT7";
        preferences.projectId = "c367b01c-d827-4ef1-8527-d3ad9fd65d21";
        preferences.renderAPI = sl::RenderAPI::eVulkan;
        const auto result = g.init(preferences, sl::kSDKVersion);
        if (result != sl::Result::eOk) { Report("slInit", result); return real_proc_addr; }
        g.initialized = true;
        g.get_instance_proc_addr = proxy;
        for (const auto feature : Features) {
            sl::FeatureRequirements requirements {};
            if (g.get_requirements(feature, requirements) != sl::Result::eOk) continue;
            for (uint32_t i = 0; i < requirements.vkNumInstanceExtensions; ++i)
                g.instance_extensions.emplace_back(requirements.vkInstanceExtensions[i]);
            for (uint32_t i = 0; i < requirements.vkNumDeviceExtensions; ++i)
                g.device_extensions.emplace_back(requirements.vkDeviceExtensions[i]);
        }
        g_reason.store(StatusReason::WaitingSwapchain);
        std::printf("GT7 DLSS FG: Streamline initialized before Vulkan; native fixed 2x requested; runtime=%ls\n",
                    g.runtime_directory.c_str());
        return proxy;
    } catch (const std::exception& error) {
        std::printf("GT7 DLSS FG: initialization failed: %s\n", error.what());
        g_reason.store(StatusReason::RuntimeError);
        return g.get_instance_proc_addr ? g.get_instance_proc_addr : real_proc_addr;
    }
#else
    g_reason.store(StatusReason::BuildUnsupported);
    std::printf("GT7 DLSS FG: this build has no Streamline support\n");
    return real_proc_addr;
#endif
}

void AppendInstanceExtensions(std::vector<const char*>& extensions) {
#ifdef GT7_HAS_DLSSG_RUNTIME
    std::scoped_lock lock(g.mutex);
    for (const auto& extension : g.instance_extensions)
        if (std::none_of(extensions.begin(), extensions.end(), [&](const char* value) { return extension == value; }))
            extensions.push_back(extension.c_str());
#endif
}

bool CreateSurface(SDL_Window* window, vk::Instance instance, vk::SurfaceKHR* surface) {
    if (!window || !instance || !surface) return SDL_SetError("Invalid Vulkan surface arguments");
    *surface = nullptr;
#ifdef GT7_HAS_DLSSG_RUNTIME
    {
        std::scoped_lock lock(g.mutex);
        if (g.initialized && g.get_instance_proc_addr) {
            const auto properties = SDL_GetWindowProperties(window);
            const auto hwnd = static_cast<HWND>(SDL_GetPointerProperty(
                properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
            if (!hwnd) return SDL_SetError("DLSS FG requires the SDL Win32 HWND property");
            const auto create_surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
                g.get_instance_proc_addr(static_cast<VkInstance>(instance), "vkCreateWin32SurfaceKHR"));
            if (!create_surface) return SDL_SetError("Streamline vkCreateWin32SurfaceKHR unavailable");
            VkWin32SurfaceCreateInfoKHR info {};
            info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
            info.hwnd = hwnd;
            info.hinstance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
            if (!info.hinstance) return SDL_SetError("SDL window instance handle unavailable");
            VkSurfaceKHR native_surface = VK_NULL_HANDLE;
            const auto result = create_surface(static_cast<VkInstance>(instance), &info, nullptr, &native_surface);
            if (result != VK_SUCCESS) {
                std::printf("GT7 DLSS FG: interposer Win32 surface creation failed (%d)\n", static_cast<int>(result));
                return SDL_SetError("Streamline Vulkan surface creation failed (%d)", static_cast<int>(result));
            }
            *surface = native_surface;
            std::printf("GT7 DLSS FG: Win32 surface registered through Streamline\n");
            return true;
        }
    }
#endif
    VkSurfaceKHR native_surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, static_cast<VkInstance>(instance), nullptr, &native_surface)) return false;
    *surface = native_surface;
    return true;
}

void AppendDeviceExtensions(std::vector<const char*>& extensions) {
#ifdef GT7_HAS_DLSSG_RUNTIME
    std::scoped_lock lock(g.mutex);
    if (!g.initialized) return;
    for (const auto& extension : g.device_extensions)
        if (std::none_of(extensions.begin(), extensions.end(), [&](const char* value) { return extension == value; }))
            extensions.push_back(extension.c_str());
    // Streamline 2.14.16 enables NV low-latency2 but omits this required
    // extension dependency. The host also enables the available presentId
    // feature so the runtime can use IDs in its private presentation path.
    if (std::none_of(extensions.begin(), extensions.end(), [](const char* value) {
            return std::strcmp(value, VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0;
        })) extensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
#endif
}

bool OnDevice(GraphicContext& graphics, uint32_t graphics_queue_index) {
#ifdef GT7_HAS_DLSSG_RUNTIME
    std::scoped_lock lock(g.mutex);
    if (!g.initialized || !g.get_instance_proc_addr) return false;
    // vkCreateDevice proxy has already installed queues, features, device and
    // instance. slSetVulkanInfo here would overwrite its private queue mapping.
    (void)graphics_queue_index;
    sl::AdapterInfo adapter {};
    adapter.vkPhysicalDevice = static_cast<VkPhysicalDevice>(graphics.physical_device);
    const auto supported = g.is_supported(sl::kFeatureDLSS_G, adapter);
    if (supported != sl::Result::eOk) {
        std::printf("GT7 DLSS FG: adapter unsupported (result %d); check GPU, driver and hardware GPU scheduling\n",
                    static_cast<int>(supported));
        g_reason.store(StatusReason::Unsupported); return false;
    }
    g.device = graphics.device;
    g.set_options = Feature<PFun_slDLSSGSetOptions>(sl::kFeatureDLSS_G, "slDLSSGSetOptions");
    g.get_state = Feature<PFun_slDLSSGGetState>(sl::kFeatureDLSS_G, "slDLSSGGetState");
    g.reflex_options = Feature<PFun_slReflexSetOptions>(sl::kFeatureReflex, "slReflexSetOptions");
    g.reflex_sleep = Feature<PFun_slReflexSleep>(sl::kFeatureReflex, "slReflexSleep");
    g.marker = Feature<PFun_slPCLSetMarker>(sl::kFeaturePCL, "slPCLSetMarker");
    if (!g.set_options || !g.get_state || !g.reflex_options || !g.reflex_sleep || !g.marker) {
        g_reason.store(StatusReason::RuntimeError); return false;
    }
    // Query before the first application present. GetState consumes presented
    // counters, so subsequent capability updates share the one post-present
    // query instead of adding another poll when FG is enabled or suspended.
    sl::DLSSGState capability {};
    const auto capability_result = g.get_state(Viewport, capability, nullptr);
    if (capability_result != sl::Result::eOk) {
        Report("slDLSSGGetState capabilities", capability_result);
        return false;
    }
    g_max_generated_frames.store(capability.numFramesToGenerateMax);
    std::printf("GT7 DLSS FG: requested_generated=%u multiplier=2 mode=fixed max_generated=%u\n",
                Gt7DlssFrameGenPolicy::GeneratedFrames, capability.numFramesToGenerateMax);
    if (!Gt7DlssFrameGenPolicy::SupportsFixed2x(capability.numFramesToGenerateMax)) {
        g_reason.store(StatusReason::Fixed2xUnsupported);
        return false;
    }
    sl::ReflexOptions reflex {};
    reflex.mode = sl::ReflexMode::eLowLatency;
    reflex.frameLimitUs = 0;
    const auto result = g.reflex_options(reflex);
    if (result != sl::Result::eOk) { Report("slReflexSetOptions", result); return false; }
    // This initializes the plugin's constants, but LL2 has no swapchain yet
    // and returns success without applying them to the driver.
    g.swapchain_reflex_ready = false;
    g_ready.store(true);
    g_reason.store(StatusReason::WaitingInputs);
    std::printf("GT7 DLSS FG: NVIDIA Vulkan device ready; waiting for real camera/depth/motion/HUD-less inputs\n");
    return true;
#else
    return false;
#endif
}

bool DeviceReady() { return g_ready.load(); }
StatusSnapshot GetStatus() {
    auto reason = g_reason.load();
    if (reason == StatusReason::Active && Now()-g_last_generated.load() >= 2000) reason = StatusReason::WaitingPresent;
    return {reason, g_sdk_presented.load(), g_application_presents.load(), g_generated.load(),
            g_sdk_status.load(), g_last_presented_count.load(), g_max_generated_frames.load()};
}
bool IsGenerating() { return GetStatus().reason == StatusReason::Active; }

void OnSwapchainCreated() {
#ifdef GT7_HAS_DLSSG_RUNTIME
    if (!DeviceReady()) return;
    std::scoped_lock lock(g.mutex);
    if (!g.initialized || !g.reflex_options) return;
    g.swapchain_reflex_ready = false;
    // Streamline 2.14.16's LL2 create hook only stores the new swapchain;
    // it does not replay the options supplied before that swapchain existed.
    // Reapply once per creation, including recreation. No additional FPS cap.
    sl::ReflexOptions reflex {};
    reflex.mode = sl::ReflexMode::eLowLatency;
    reflex.frameLimitUs = 0;
    const auto result = g.reflex_options(reflex);
    if (result != sl::Result::eOk) {
        Report("slReflexSetOptions after swapchain create", result);
        Suspend(StatusReason::RuntimeError);
        return;
    }
    g.swapchain_reflex_ready = true;
    g_reason.store(StatusReason::WaitingInputs);
    std::printf("GT7 DLSS FG: Reflex low latency options applied after swapchain creation; frame_limit_us=0\n");
#endif
}

void BeginFrame() {
#ifdef GT7_HAS_DLSSG_RUNTIME
    if (!DeviceReady()) return;
    std::scoped_lock lock(g.mutex);
    g.frame_valid = false;
    g.frame = nullptr;
    g.frame_pending_prepare = false;
    if (!g.swapchain_reflex_ready) return;
    ++g.frame_index;
    auto result = g.new_token(g.frame, &g.frame_index);
    if (result != sl::Result::eOk || !g.frame) {
        Report("slGetNewFrameToken", result); SetMode(false); return;
    }
    // The SDK's Vulkan Reflex sleep waits on a CPU semaphore. Keep this
    // outside the shared renderer mutex so producers can continue recording.
    {
        const Measure measure {g.sleep_timing};
        result = g.reflex_sleep(*g.frame);
    }
    if (result != sl::Result::eOk) {
        Report("slReflexSleep", result);
        Suspend(StatusReason::RuntimeError);
        g.frame = nullptr;
        return;
    }
    // These describe the presenter stage, not guest simulation latency.
    Marker(sl::PCLMarker::eSimulationStart);
    Marker(sl::PCLMarker::eSimulationEnd);
    g.frame_pending_prepare = true;
#endif
}

void RecordPrepare(GraphicContext& graphics, vk::CommandBuffer command,
                   const Gt7FrameGen::FrameInputs* inputs, vk::Image backbuffer, vk::ImageView backbuffer_view,
                   vk::Extent2D display, vk::Format format, vk::Rect2D region, const VulkanImage* hudless) {
#ifdef GT7_HAS_DLSSG_RUNTIME
    if (!DeviceReady()) return;
    std::scoped_lock lock(g.mutex);
    const Measure measure {g.prepare_timing};
    (void)graphics; (void)backbuffer_view;
    g.frame_valid = false;
    // BeginFrame failed: it already disabled FG for this presentation.
    if (!g.frame || !g.frame_pending_prepare) return;
    g.frame_pending_prepare = false;
    if (Config::PssrDlssEnabled() && Dlss::GetSessionState() == Dlss::SessionState::Failed) {
        Suspend(StatusReason::SuperResolutionUnavailable); return;
    }
    if (!Gt7DlssFrameGenPolicy::SupportsFixed2x(g_max_generated_frames.load())) {
        Suspend(StatusReason::Fixed2xUnsupported); return;
    }
    if (!inputs) { Suspend(StatusReason::WaitingInputs); return; }
    if (!inputs->valid) {
        switch (inputs->rejection_reason) {
        case Gt7FrameGen::StatusReason::SceneUnverified: Suspend(StatusReason::SceneUnverified); break;
        case Gt7FrameGen::StatusReason::CameraUnverified: Suspend(StatusReason::CameraUnverified); break;
        case Gt7FrameGen::StatusReason::HudlessUnverified: Suspend(StatusReason::HudlessUnverified); break;
        case Gt7FrameGen::StatusReason::DuplicateFrame: Suspend(StatusReason::DuplicateFrame); break;
        case Gt7FrameGen::StatusReason::InputUnavailable:
        case Gt7FrameGen::StatusReason::InvalidInput: Suspend(StatusReason::InvalidInputs); break;
        default: Suspend(StatusReason::WaitingInputs); break;
        }
        return;
    }
    if (!inputs->guest_frame || inputs->guest_frame == g.previous_guest) { Suspend(StatusReason::DuplicateFrame); return; }
    if (!inputs->camera_valid || !inputs->camera_pose_valid) { Suspend(StatusReason::CameraUnverified); return; }
    if (!inputs->hudless_valid || !inputs->hudless_color.image || !hudless ||
        !ValidImage(*hudless, display, format)) { Suspend(StatusReason::HudlessUnverified); return; }
    if (!command || !backbuffer || !display.width || !display.height ||
        !inputs->render_extent.width || !inputs->render_extent.height ||
        !std::isfinite(inputs->jitter_x) || !std::isfinite(inputs->jitter_y) ||
        !ValidImage(inputs->depth, inputs->render_extent, vk::Format::eR32Sfloat) ||
        !ValidImage(inputs->motion, inputs->render_extent, vk::Format::eR32G32Sfloat) ||
        region.offset.x != 0 || region.offset.y != 0 || region.extent != display ||
        (format != vk::Format::eB8G8R8A8Unorm && format != vk::Format::eR8G8B8A8Unorm &&
         format != vk::Format::eB8G8R8A8Srgb && format != vk::Format::eR8G8B8A8Srgb)) {
        Suspend(StatusReason::InvalidInputs); return;
    }
    const auto discontinuity_sources = g_discontinuity.exchange(0);
    const auto reset_reasons = Gt7FrameGenLifecycle::ConsumerResetReasons(
        discontinuity_sources != 0, inputs->reset, g.previous_valid, inputs->guest_frame, g.previous_guest,
        g.previous_render != inputs->render_extent, g.previous_display != display);
    const bool reset = reset_reasons != 0;
    sl::Constants constants {};
    Matrix current_vp;
    if (!Constants(*inputs, reset, constants, current_vp)) { Suspend(StatusReason::CameraUnverified); return; }
    const auto depth_view = View(inputs->depth.image, inputs->depth.format);
    const auto motion_view = View(inputs->motion.image, inputs->motion.format);
    const auto hudless_view = View(hudless->image, hudless->format);
    if (!depth_view || !motion_view || !hudless_view) { Suspend(StatusReason::RuntimeError); return; }
    // Enable before volatile tagging: SL copies only for an ACTIVE feature.
    if (!SetMode(true)) { Suspend(StatusReason::RuntimeError); return; }
    Marker(sl::PCLMarker::eRenderSubmitStart);
    auto result = g.set_constants(constants, *g.frame, Viewport);
    if (result != sl::Result::eOk) { Report("slSetConstants", result); Suspend(StatusReason::RuntimeError); return; }
    auto depth = Resource(inputs->depth, depth_view);
    auto motion = Resource(inputs->motion, motion_view);
    auto hud = Resource(*hudless, hudless_view);
    const sl::Extent input_extent {0, 0, inputs->render_extent.width, inputs->render_extent.height};
    const sl::Extent output_extent {0, 0, display.width, display.height};
    sl::ResourceTag tags[] {
        // Streamline obtains the actual backbuffer from intercepted present.
        // The documented null-resource backbuffer tag supplies only its rect.
        {nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &output_extent},
        {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &input_extent},
        {&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &input_extent},
        {&hud, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, &output_extent},
    };
    auto* buffer = reinterpret_cast<sl::CommandBuffer*>(static_cast<VkCommandBuffer>(command));
    {
        const Measure tags_measure {g.tags_timing};
        result = g.set_tags(*g.frame, Viewport, tags, static_cast<uint32_t>(std::size(tags)), buffer);
    }
    if (result != sl::Result::eOk) { Report("slSetTagForFrame", result); Suspend(StatusReason::RuntimeError); return; }
    Marker(sl::PCLMarker::eRenderSubmitEnd);
    if (g.reset_diagnostics.Report(reset_reasons, Now())) {
        std::printf("GT7 DLSS FG reset: token=%u scene=%llu previous_scene=%llu mask=0x%x"
                    " discontinuity_sources=0x%x input_reset=%u previous_valid=%u interval_ms=%.6f"
                    " render=%ux%u previous_render=%ux%u display=%ux%u previous_display=%ux%u reset=%u\n",
            g.frame_index, static_cast<unsigned long long>(inputs->guest_frame),
            static_cast<unsigned long long>(g.previous_guest), reset_reasons, discontinuity_sources,
            inputs->reset ? 1u : 0u, g.previous_valid ? 1u : 0u, inputs->frame_time_ms,
            inputs->render_extent.width, inputs->render_extent.height, g.previous_render.width, g.previous_render.height,
            display.width, display.height, g.previous_display.width, g.previous_display.height, reset ? 1u : 0u);
    }
    g.previous_guest = inputs->guest_frame;
    g.previous_view_projection = current_vp;
    g.previous_valid = true;
    g.previous_render = inputs->render_extent;
    g.previous_display = display;
    g.frame_valid = true;
    g.frame_reset = reset;
    if (reset) { ++g.reset_count; g_reason.store(StatusReason::HistoryReset); }
    else if (!IsGenerating()) g_reason.store(StatusReason::WaitingPresent);
#endif
}

void BeforePresent() {
#ifdef GT7_HAS_DLSSG_RUNTIME
    if (!DeviceReady()) return;
    std::scoped_lock lock(g.mutex);
    g.present_begin_ns = NowNs();
    if (g.previous_present_begin_ns) g.interval_timing.Add(g.present_begin_ns-g.previous_present_begin_ns);
    g.previous_present_begin_ns = g.present_begin_ns;
    Marker(sl::PCLMarker::ePresentStart);
#endif
}
void AfterPresent(vk::Result result) {
#ifdef GT7_HAS_DLSSG_RUNTIME
    if (!DeviceReady()) return;
    std::scoped_lock lock(g.mutex);
    if (g.present_begin_ns) g.present_timing.Add(NowNs()-g.present_begin_ns);
    g.present_begin_ns = 0;
    Marker(sl::PCLMarker::ePresentEnd);
    g_present_attempts.fetch_add(1);
    if (result == vk::Result::eSuccess || result == vk::Result::eSuboptimalKHR)
        g_application_presents.fetch_add(1);
    else {
        g.frame_valid = false;
        g.previous_valid = false;
        g_discontinuity.fetch_or(Gt7FrameGenLifecycle::PresentFailure);
        g_reason.store(StatusReason::RuntimeError);
    }
    QueryState(); // Exactly once per application present, on the presenting thread.
    ReportTiming();
    // Keep the token for explicit tag clearing during drain, but never prepare it twice.
    g.frame_pending_prepare = false;
#endif
}
void NoteDroppedFrame() {
    g_discontinuity.fetch_or(Gt7FrameGenLifecycle::Dropped);
    if (DeviceReady()) g_reason.store(StatusReason::DroppedFrame);
}
void WaitForPresents(GraphicContext& graphics) {
#ifdef GT7_HAS_DLSSG_RUNTIME
    std::scoped_lock lock(g.mutex);
    if (!g.initialized || !g.device) return;
    ++g.drain_count;
    SetMode(false);
    ClearTags();
    {
        Common::LockGuard queue_lock(graphics.queue_mutex);
        const auto result = graphics.device.waitIdle();
        if (result != vk::Result::eSuccess) {
            std::printf("GT7 DLSS FG: waitIdle failed (%d) while draining presentation\n", static_cast<int>(result));
            g_reason.store(StatusReason::RuntimeError);
        }
    }
    // Explicit resize/shutdown drain, not a transient missing-input fallback.
    // Release FG's NGX handles before the separately owned NGX SR context can
    // be shut down. Streamline remains initialized for swapchain recreation.
    if (g.free_resources && g.set_options) {
        const auto result = g.free_resources(sl::kFeatureDLSS_G, Viewport);
        if (result != sl::Result::eOk) Report("slFreeResources", result);
    }
    for (const auto& [image, view] : g.views) g.device.destroyImageView(view);
    g.views.clear();
    g.previous_valid = false;
    g_discontinuity.fetch_or(Gt7FrameGenLifecycle::Drained);
#endif
}
void OnSwapchainDestroyed() {
#ifdef GT7_HAS_DLSSG_RUNTIME
    std::scoped_lock lock(g.mutex);
    if (!g.initialized) return;
    SetMode(false);
    ClearTags();
    g.frame = nullptr;
    g.frame_pending_prepare = false;
    g.swapchain_reflex_ready = false;
    g.frame_valid = false;
    g.previous_valid = false;
    g.previous_guest = 0;
    g.present_begin_ns = g.previous_present_begin_ns = 0;
    g_discontinuity.fetch_or(Gt7FrameGenLifecycle::SwapchainDestroyed);
    if (DeviceReady()) g_reason.store(StatusReason::WaitingSwapchain);
#endif
}
void Shutdown() {
#ifdef GT7_HAS_DLSSG_RUNTIME
    std::scoped_lock lock(g.mutex);
    g_ready.store(false);
    if (!g.initialized) return;
    SetMode(false);
    ClearTags();
    ReportTiming(true);
    const auto result = g.shutdown();
    if (result != sl::Result::eOk) Report("slShutdown", result);
    for (const auto& [image, view] : g.views) if (g.device) g.device.destroyImageView(view);
    g.views.clear();
    g.initialized = false;
    g.swapchain_reflex_ready = false;
    g.device = nullptr;
    g.frame = nullptr;
    // Do not FreeLibrary: vkDestroyDevice/Instance still dispatch via this DLL.
#endif
}

const char* StatusReasonText(StatusReason reason) {
    switch (reason) {
    case StatusReason::Off: return "disabled";
    case StatusReason::Loading: return "loading NVIDIA runtime";
    case StatusReason::BuildUnsupported: return "build has no native DLSS FG";
    case StatusReason::RuntimeMissing: return "NVIDIA runtime missing";
    case StatusReason::RuntimeUntrusted: return "NVIDIA runtime signature rejected";
    case StatusReason::Unsupported: return "GPU/driver/HAGS unsupported";
    case StatusReason::Fixed2xUnsupported: return "runtime does not support fixed 2x generation";
    case StatusReason::SuperResolutionUnavailable: return "DLSS SR unavailable; linear fallback";
    case StatusReason::WaitingSwapchain: return "waiting for swapchain";
    case StatusReason::WaitingInputs: return "waiting for matched scene inputs";
    case StatusReason::InvalidInputs: return "invalid input images or presentation region";
    case StatusReason::SceneUnverified: return "DLSS scene not verified";
    case StatusReason::CameraUnverified: return "camera pose/projection unverified";
    case StatusReason::HudlessUnverified: return "HUD-less display image unavailable";
    case StatusReason::DuplicateFrame: return "duplicate guest frame";
    case StatusReason::HistoryReset: return "resetting interpolation history";
    case StatusReason::WaitingPresent: return "waiting for SDK generated presentation";
    case StatusReason::Active: return "native 2x generated presentations observed";
    case StatusReason::DroppedFrame: return "presenter dropped a frame";
    case StatusReason::RuntimeError: return "NVIDIA runtime error (see log)";
    }
    return "unknown";
}

} // namespace Libs::Graphics::Gt7DlssFrameGen
