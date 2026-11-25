struct KytyDlssPssrSession {
    KytyDlssContext* owner {};
    uint32_t features {};
};
struct KytyDlssPssrFeature {
    KytyDlssPssrSession* session {};
    KytyDlssPssrCreation creation {};
    NVSDK_NGX_Parameter* parameters {};
    NVSDK_NGX_Handle* handle {};
    bool poisoned {};
    uint64_t evaluations {};
    ~KytyDlssPssrFeature() {
        if (handle) NVSDK_NGX_VULKAN_ReleaseFeature(handle);
        if (parameters) NVSDK_NGX_VULKAN_DestroyParameters(parameters);
    }
};
namespace {
KytyDlssStatus PssrSessionCreate(const KytyDlssCreateInfo* i, KytyDlssPssrSession** out, KytyDlssError* e) {
    if (out) *out = nullptr;
    return Safe(e, [&] {
        Require(i && i->struct_size == sizeof(*i) && out, "Native session struct/output mismatch");
        auto session = std::make_unique<KytyDlssPssrSession>();
        auto owner_info = *i;
        owner_info.input_width = owner_info.output_width = 64;
        owner_info.input_height = owner_info.output_height = 64;
        owner_info.quality = KYTY_DLSS_DLAA;
        KytyDlssError inner {};
        const auto status = Create(&owner_info, &session->owner, &inner);
        if (status != KYTY_DLSS_OK) throw Failure(status, inner.message, inner.ngx_result);
        *out = session.release();
    });
}
void PssrOptimalLocked(KytyDlssPssrSession* s, uint32_t w, uint32_t h, uint32_t mode,
                       KytyDlssOptimalSettings* out) {
    Require(s && s->owner && out && out->struct_size == sizeof(*out), "Native optimal session/struct mismatch");
    Require(w >= 64 && h >= 64 && w <= 16384 && h <= 16384 && mode <= 5, "Invalid native output extent/mode");
    *out = {}; out->struct_size = sizeof(*out);
    float sharpness {};
    Ngx(NGX_DLSS_GET_OPTIMAL_SETTINGS(s->owner->capabilities, w, h,
        static_cast<NVSDK_NGX_PerfQuality_Value>(mode), &out->optimal_width, &out->optimal_height,
        &out->max_width, &out->max_height, &out->min_width, &out->min_height, &sharpness), "Native optimal settings");
    // Some runtimes omit dynamic ranges. Then only the exact optimal size is accepted.
    if (!out->min_width) out->min_width = out->optimal_width;
    if (!out->min_height) out->min_height = out->optimal_height;
    if (!out->max_width) out->max_width = out->optimal_width;
    if (!out->max_height) out->max_height = out->optimal_height;
    if (mode == 5) { out->min_width = out->max_width = w; out->min_height = out->max_height = h; }
}
KytyDlssStatus PssrOptimal(KytyDlssPssrSession* s, uint32_t w, uint32_t h, uint32_t mode,
                          KytyDlssOptimalSettings* out, KytyDlssError* e) {
    return Safe(e, [&] { std::lock_guard lock(global_mutex); PssrOptimalLocked(s,w,h,mode,out); });
}
KytyDlssStatus PssrFeatureCreate(KytyDlssPssrSession* s, const KytyDlssPssrCreation* c,
                                KytyDlssPssrFeature** out, KytyDlssError* e) {
    if (out) *out = nullptr;
    return Safe(e, [&] {
        std::lock_guard lock(global_mutex);
        Require(s && s->owner && c && c->struct_size == sizeof(*c) && out, "Native feature struct/session mismatch");
        Require(!(c->flags & ~3u), "Unknown native feature flags");
        KytyDlssOptimalSettings range {sizeof(range)};
        PssrOptimalLocked(s,c->output_width,c->output_height,c->mode,&range);
        Require(c->min_width >= range.min_width && c->min_height >= range.min_height &&
                c->max_width <= range.max_width && c->max_height <= range.max_height &&
                c->min_width >= 1 && c->min_height >= 1 &&
                c->min_width <= c->max_width && c->min_height <= c->max_height,
                "Native feature dimensions outside runtime-supported range");
        auto feature = std::make_unique<KytyDlssPssrFeature>();
        feature->session=s; feature->creation=*c;
        Ngx(NVSDK_NGX_VULKAN_AllocateParameters(&feature->parameters), "Allocate native feature parameters");
        const char* keys[] {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality};
        for (const char* key: keys) NVSDK_NGX_Parameter_SetUI(feature->parameters,key,NgxPreset(s->owner->preset));
        *out = feature.release(); ++s->features;
    });
}
bool PssrColorFormat(VkFormat f) {
    return f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
}
bool PssrDepthFormat(VkFormat f, VkImageAspectFlags aspect) {
    if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) return f == VK_FORMAT_R32_SFLOAT || f == VK_FORMAT_R16_SFLOAT;
    return aspect == VK_IMAGE_ASPECT_DEPTH_BIT && (f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D16_UNORM ||
        f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32);
}
void PssrImage(const KytyDlssPssrImage& i) {
    Require(i.image && i.view && i.width && i.height && i.width <= 16384 && i.height <= 16384 &&
            i.range.levelCount == 1 && i.range.layerCount == 1 && i.range.baseMipLevel < 32 &&
            (i.range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT || i.range.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT),
            "Invalid native image handle, extent or subresource range");
}
void PssrRect(const KytyDlssPssrImage& i, const KytyDlssPssrEvaluateInfo& e) {
    Require(e.x <= i.width && e.y <= i.height && e.width <= i.width-e.x && e.height <= i.height-e.y,
            "Native input subrect exceeds its view extent");
}
NVSDK_NGX_Resource_VK PssrResource(const KytyDlssPssrImage& i, bool writable) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(i.view,i.image,i.range,i.format,i.width,i.height,writable);
}
KytyDlssStatus PssrEvaluate(KytyDlssPssrFeature* f, const KytyDlssPssrEvaluateInfo* i,
                           KytyDlssRecordResult* out, KytyDlssError* e) {
    return Safe(e,[&] {
        std::lock_guard lock(global_mutex);
        Require(f && i && i->struct_size == sizeof(*i) && out && out->struct_size == sizeof(*out), "Native evaluation struct/feature mismatch");
        *out={}; out->struct_size=sizeof(*out);
        if (f->poisoned) throw Failure(KYTY_DLSS_POISONED,"Native feature poisoned; discard command buffer and drain before destruction");
        Require(i->command_buffer,"Missing native command buffer");
        const auto& c=f->creation;
        Require(i->width >= c.min_width && i->height >= c.min_height && i->width <= c.max_width && i->height <= c.max_height,
                "Native dynamic render extent outside feature range");
        PssrImage(i->color); PssrImage(i->depth); PssrImage(i->motion); PssrImage(i->output);
        PssrRect(i->color,*i); PssrRect(i->depth,*i); PssrRect(i->motion,*i);
        Require(PssrColorFormat(i->color.format) && PssrColorFormat(i->output.format) &&
                i->color.range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && i->output.range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
                PssrDepthFormat(i->depth.format,i->depth.range.aspectMask) && i->motion.range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
                (i->motion.format == VK_FORMAT_R32G32_SFLOAT || i->motion.format == VK_FORMAT_R16G16_SFLOAT ||
                 i->motion.format == VK_FORMAT_R16G16_SNORM), "Unsupported native image format/aspect");
        Require(i->output.width == c.output_width && i->output.height == c.output_height,"Native output extent mismatch");
        Require(i->output.image != i->color.image && i->output.image != i->depth.image && i->output.image != i->motion.image,
                "Native output must not alias inputs");
        if (i->exposure) {
            PssrImage(*i->exposure);
            Require(i->exposure->width == 1 && i->exposure->height == 1 && i->exposure->range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
                    (i->exposure->format == VK_FORMAT_R32_SFLOAT || i->exposure->format == VK_FORMAT_R16_SFLOAT) &&
                    i->exposure->image != i->output.image,"Invalid native exposure resource");
        }
        Require(std::isfinite(i->jitter_x) && std::isfinite(i->jitter_y) && std::abs(i->jitter_x)<=.5f && std::abs(i->jitter_y)<=.5f &&
                std::isfinite(i->motion_scale_x) && std::isfinite(i->motion_scale_y) &&
                std::isfinite(i->pre_exposure) && i->pre_exposure>0 && std::isfinite(i->exposure_scale) && i->exposure_scale>0 &&
                std::isfinite(i->frame_time_ms) && i->frame_time_ms>0 && i->frame_time_ms<=1000,"Invalid native motion/jitter/exposure/timing");
        try {
            if (!f->handle) {
                NVSDK_NGX_DLSS_Create_Params p {};
                p.Feature.InWidth=c.max_width; p.Feature.InHeight=c.max_height;
                p.Feature.InTargetWidth=c.output_width; p.Feature.InTargetHeight=c.output_height;
                p.Feature.InPerfQualityValue=static_cast<NVSDK_NGX_PerfQuality_Value>(c.mode);
                p.InFeatureCreateFlags=NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
                if (c.flags & KYTY_DLSS_PSSR_REVERSE_DEPTH) p.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
                if (c.flags & KYTY_DLSS_PSSR_AUTO_EXPOSURE) p.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
                Ngx(NGX_VULKAN_CREATE_DLSS_EXT1(f->session->owner->device,i->command_buffer,1,1,&f->handle,f->parameters,&p),"Create native DLSS feature");
                f->session->owner->discovery->VerifyLoadedRuntime(true);
                out->feature_created=1;
            }
            auto color=PssrResource(i->color,false), depth=PssrResource(i->depth,false), motion=PssrResource(i->motion,false), output=PssrResource(i->output,true);
            NVSDK_NGX_Resource_VK exposure {};
            if (i->exposure) exposure=PssrResource(*i->exposure,false);
            NVSDK_NGX_VK_DLSS_Eval_Params p {};
            p.Feature.pInColor=&color; p.Feature.pInOutput=&output; p.pInDepth=&depth; p.pInMotionVectors=&motion;
            p.pInExposureTexture=i->exposure?&exposure:nullptr;
            p.InRenderSubrectDimensions={i->width,i->height};
            p.InColorSubrectBase=p.InDepthSubrectBase=p.InMVSubrectBase={i->x,i->y};
            p.InJitterOffsetX=i->jitter_x; p.InJitterOffsetY=i->jitter_y;
            p.InMVScaleX=i->motion_scale_x; p.InMVScaleY=i->motion_scale_y;
            p.InPreExposure=i->pre_exposure; p.InExposureScale=i->exposure_scale; p.InFrameTimeDeltaInMsec=i->frame_time_ms;
            p.InReset=!f->evaluations || i->reset_history;
            Ngx(NGX_VULKAN_EVALUATE_DLSS_EXT(i->command_buffer,f->handle,f->parameters,&p),"Evaluate native DLSS");
            out->reset_used=p.InReset; out->evaluations_recorded=++f->evaluations;
            if (f->evaluations == 1) std::fprintf(stderr,"PSSR DLSS: native feature recorded, preset %c, render %ux%u output %ux%u; effective model unverified\n",
                f->session->owner->preset.letter,i->width,i->height,c.output_width,c.output_height);
        } catch (const Failure& failure) {
            f->poisoned=true;
            throw Failure(failure.status == KYTY_DLSS_INVALID_ARGUMENT ? KYTY_DLSS_INTERNAL_ERROR : failure.status,failure.what(),failure.ngx);
        } catch (...) { f->poisoned=true; throw; }
    });
}
KytyDlssStatus PssrFeatureDestroy(KytyDlssPssrFeature* f,uint32_t completed,KytyDlssError* e) {
    return Safe(e,[&] { std::lock_guard lock(global_mutex);
        Require(f && completed == 1,"Native feature destroy requires host-confirmed GPU completion");
        --f->session->features; delete f;
    });
}
KytyDlssStatus PssrSessionDestroy(KytyDlssPssrSession* s,uint32_t completed,KytyDlssError* e) {
    return Safe(e,[&] { std::lock_guard lock(global_mutex);
        Require(s && completed == 1,"Native session destroy requires host-confirmed GPU completion");
        if (s->features) throw Failure(KYTY_DLSS_BUSY,"Release all native feature histories before shutting down NGX");
        delete s->owner; delete s; context_active=false;
    });
}
}
extern "C" __declspec(dllexport) KytyDlssStatus KytyDlssGetPssrApi(uint32_t version,KytyDlssPssrApi* api) {
    if (version != KYTY_DLSS_PSSR_ABI_VERSION || !api || api->struct_size != sizeof(*api)) return KYTY_DLSS_INVALID_ARGUMENT;
    *api={sizeof(*api),KYTY_DLSS_PSSR_ABI_VERSION,PssrSessionCreate,PssrOptimal,PssrFeatureCreate,PssrEvaluate,PssrFeatureDestroy,PssrSessionDestroy};
    return KYTY_DLSS_OK;
}
