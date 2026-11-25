#include "graphics/host_gpu/renderer/dlss.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/pssr.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/gt7DlssDevice.h"
#include "graphics/shader/shaderBindings.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::Dlss {
struct Feature {
    KytyDlssPssrFeature* native {};
    mutable std::chrono::steady_clock::time_point previous {};
};
namespace {
constexpr uint32_t FixedFlags=Pssr::kReverseDepth|Pssr::kAutoExposure;
std::mutex mutex;
Parameter parameters {};
VkDevice device {};
uint32_t owners {};
std::atomic<SessionState> session_state {SessionState::NotStarted};
void Warn(const char* operation,const KytyDlssError& error) {
    // A rejected request may repeat every frame; do not turn safe fallback into
    // a synchronous disk-log flush on every native dispatch.
    static std::mutex warnings_mutex;
    static std::unordered_map<std::string,uint64_t> warnings;
    uint64_t count;
    {
        std::lock_guard lock(warnings_mutex);
        count=++warnings[std::string(operation)+": "+error.message];
    }
    if (count<=3 || (count&(count-1))==0)
        Log::WriteToConsoleAndLog(fmt::format("PSSR DLSS: {}: {}; native request uses linear fallback (occurrences={}).\n",operation,error.message,count));
}
KytyDlssPssrImage Image(const Resource& r) { return {r.image,r.view,r.range,r.format,r.width,r.height}; }
}
bool Accepts(const Pssr::Request& request) {
    ShaderTextureResource depth {},motion {};
    std::copy_n(request.depth,8,depth.fields); std::copy_n(request.motion,8,motion.fields);
    return !(request.flags&Pssr::kSpatialOnly) && !depth.IsNull() && !motion.IsNull() &&
        std::isfinite(request.jitter[0]) && std::isfinite(request.jitter[1]) &&
        std::abs(request.jitter[0])<=.5f && std::abs(request.jitter[1])<=.5f &&
        std::isfinite(request.motion_scale[0]) && std::isfinite(request.motion_scale[1]) &&
        std::isfinite(request.pre_exposure) && request.pre_exposure>0;
}
bool Takes(const Creation& c,const Pssr::Request& r,const Inputs& i) {
    return c.output_width==i.output.width && c.output_height==i.output.height &&
        c.flags==(r.flags&FixedFlags) && r.width>=c.min_width && r.height>=c.min_height &&
        r.width<=c.max_width && r.height<=c.max_height;
}
std::optional<Creation> Choose(Parameter& p,const Pssr::Request& r,const Inputs& i) {
    if (!p.api || !p.session) return std::nullopt;
    std::optional<Creation> best;
    uint64_t best_distance=0;
    for (uint32_t mode=0;mode<=5;++mode) {
        KytyDlssOptimalSettings range {sizeof(range)}; KytyDlssError error {};
        if (p.api->query_optimal(p.session,i.output.width,i.output.height,mode,&range,&error)!=KYTY_DLSS_OK ||
            !range.optimal_width || !range.optimal_height) continue;
        Creation c {i.output.width,i.output.height,r.flags&FixedFlags,static_cast<int32_t>(mode),
            range.min_width,range.min_height,range.max_width,range.max_height};
        const uint64_t distance=uint64_t(std::max(range.optimal_width,r.width)-std::min(range.optimal_width,r.width))+
            uint64_t(std::max(range.optimal_height,r.height)-std::min(range.optimal_height,r.height));
        if (Takes(c,r,i) && (!best || distance<best_distance)) { best=c; best_distance=distance; }
    }
    return best;
}
void Init(const GraphicContext& g,PFN_vkGetInstanceProcAddr gipa,PFN_vkGetDeviceProcAddr gdpa) {
    std::lock_guard lock(mutex);
    if (owners++) {
        if (device && device!=static_cast<VkDevice>(g.device)) {
            EXIT("PSSR DLSS: concurrent different-device renderers are unsupported.\n");
            std::abort();
        }
        return;
    }
    parameters.api=Gt7DlssHost::ApiPssr(); device=static_cast<VkDevice>(g.device);
    if (!parameters.api || !g.gt7_dlss_device_enabled) {
        session_state.store(SessionState::Failed, std::memory_order_release);
        return;
    }
    std::vector<const char*> extensions;
    for (const auto& name:g.gt7_dlss_device_extensions) extensions.push_back(name.c_str());
    KytyDlssCreateInfo create {};
    create.struct_size=sizeof(create); create.paths=Gt7DlssHost::Paths();
    create.instance=static_cast<VkInstance>(g.instance); create.physical_device=static_cast<VkPhysicalDevice>(g.physical_device);
    create.device=device; create.get_instance_proc_addr=gipa; create.get_device_proc_addr=gdpa;
    create.queue_family_index=g.queue_family; create.enabled_feature_flags=g.gt7_dlss_feature_flags;
    create.enabled_device_extension_count=static_cast<uint32_t>(extensions.size()); create.enabled_device_extensions=extensions.data();
    KytyDlssError error {};
    if (parameters.api->create_session(&create,&parameters.session,&error)!=KYTY_DLSS_OK) {
        session_state.store(SessionState::Failed, std::memory_order_release);
        Warn("session initialization failed",error); return;
    }
    if (!parameters.session) {
        session_state.store(SessionState::Failed, std::memory_order_release);
        Log::WriteToConsoleAndLog("PSSR DLSS: backend returned no session; native requests use linear fallback.\n");
        return;
    }
    session_state.store(SessionState::Ready, std::memory_order_release);
    Log::WriteToConsoleAndLog("PSSR DLSS: shared existing backend/runtime initialized; awaiting native PSSR requests.\n");
}
void Shutdown() {
    std::lock_guard lock(mutex);
    if (!owners || --owners) return;
    if (parameters.session) {
        KytyDlssError error {};
        if (parameters.api->destroy_session(parameters.session,1,&error)!=KYTY_DLSS_OK) {
            EXIT("PSSR DLSS: cannot safely release session: %s\n",error.message);
            std::abort();
        }
    }
    parameters={}; device={};
    session_state.store(SessionState::NotStarted, std::memory_order_release);
}
SessionState GetSessionState() { return session_state.load(std::memory_order_acquire); }
Parameter* Capabilities() { return parameters.session?&parameters:nullptr; }
Feature* CreateFeature(vk::CommandBuffer,const Creation& c) {
    if (!parameters.session) return nullptr;
    auto feature=std::make_unique<Feature>();
    KytyDlssPssrCreation creation {sizeof(creation),c.output_width,c.output_height,c.flags,static_cast<uint32_t>(c.mode),
        c.min_width,c.min_height,c.max_width,c.max_height};
    KytyDlssError error {};
    if (parameters.api->create_feature(parameters.session,&creation,&feature->native,&error)!=KYTY_DLSS_OK) {
        Warn("CPU feature allocation failed",error); return nullptr;
    }
    return feature.release();
}
bool Evaluate(vk::CommandBuffer command,const Feature* feature,const Pssr::Request& r,Inputs& i,bool reset) {
    KytyDlssPssrEvaluateInfo evaluate {};
    evaluate.struct_size=sizeof(evaluate); evaluate.command_buffer=static_cast<VkCommandBuffer>(command);
    evaluate.color=Image(i.color); evaluate.depth=Image(i.depth); evaluate.motion=Image(i.motion); evaluate.output=Image(i.output);
    KytyDlssPssrImage exposure {};
    if (i.exposure) { exposure=Image(*i.exposure); evaluate.exposure=&exposure; }
    evaluate.x=r.x; evaluate.y=r.y; evaluate.width=r.width; evaluate.height=r.height;
    evaluate.jitter_x=r.jitter[0]; evaluate.jitter_y=r.jitter[1];
    evaluate.motion_scale_x=r.motion_scale[0]; evaluate.motion_scale_y=r.motion_scale[1];
    evaluate.pre_exposure=r.pre_exposure; evaluate.exposure_scale=1;
    const auto now=std::chrono::steady_clock::now();
    evaluate.frame_time_ms=feature->previous.time_since_epoch().count()==0?16.6667f:
        std::clamp(std::chrono::duration<float,std::milli>(now-feature->previous).count(),.01f,1000.f);
    evaluate.reset_history=reset || (r.flags&Pssr::kReset);
    KytyDlssRecordResult result {sizeof(result)}; KytyDlssError error {};
    const auto status=parameters.api->record_evaluate(feature->native,&evaluate,&result,&error);
    if (status==KYTY_DLSS_INVALID_ARGUMENT) { Warn("native input rejected before recording",error); return false; }
    if (status!=KYTY_DLSS_OK) {
        EXIT("PSSR DLSS: recording failed (%u, NGX 0x%x): %s. Shared command buffer may be partially recorded; terminating before submission.\n",
            unsigned(status),error.ngx_result,error.message);
        std::abort(); // No diagnostic handler may turn a poisoned buffer into a fallback.
    }
    feature->previous=now;
    return true;
}
void ReleaseFeature(Feature* feature) {
    if (!feature) return;
    KytyDlssError error {};
    if (parameters.api->destroy_feature(feature->native,1,&error)!=KYTY_DLSS_OK) {
        EXIT("PSSR DLSS: unsafe feature release: %s\n",error.message);
        std::abort();
    }
    delete feature;
}
}
