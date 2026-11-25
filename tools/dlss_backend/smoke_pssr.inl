// Native PSSR uses genuine NGX on synthetic resources only: depth aspect,
// selected mip/layer, R11G11B10 output, exposure texture, dynamic render extents,
// two live histories and replacement of one history while the other survives.
void RunPssrSynthetic(HostContext& h,VkPhysicalDevice physical,uint32_t family,
                      const KytyDlssCreateInfo& ci,Json& report) {
    auto get=reinterpret_cast<PFN_KytyDlssGetPssrApi>(GetProcAddress(h.backend,"KytyDlssGetPssrApi"));
    if (!get) throw std::runtime_error("Native resource ABI export missing");
    h.native_api.struct_size=sizeof(h.native_api);
    if (get(KYTY_DLSS_PSSR_ABI_VERSION,&h.native_api)!=KYTY_DLSS_OK) throw std::runtime_error("Native ABI mismatch");
    KytyDlssPssrApi bad {sizeof(bad)};
    if (get(999,&bad)!=KYTY_DLSS_INVALID_ARGUMENT) throw std::runtime_error("Native ABI rejection failed");
    auto& api=h.native_api;
    KytyDlssError error {};
    Check(api.create_session(&ci,&h.native_session,&error),error,"Create native shared session");
    KytyDlssContext* conflict {};
    if (h.api.create_context(&ci,&conflict,&error)!=KYTY_DLSS_BUSY || conflict)
        throw std::runtime_error("Concurrent v1 NGX ownership was not rejected");
    SmokeGpu gpu(h,physical); gpu.GetDeviceQueue(h.device,family,0,&gpu.queue);
    VkCommandPoolCreateInfo pci {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex=family;
    CheckVk(gpu.CreateCommandPool(h.device,&pci,nullptr,&gpu.pool),"Native host command pool");
    VkCommandBufferAllocateInfo cai {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool=gpu.pool; cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount=1;
    CheckVk(gpu.AllocateCommandBuffers(h.device,&cai,&gpu.command),"Native command buffer");
    VkFenceCreateInfo fci {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CheckVk(gpu.CreateFence(h.device,&fci,nullptr,&gpu.fence),"Native fence");
    constexpr uint32_t iw=816,ih=466;
    gpu.MakeImage(gpu.images[0],iw,ih,VK_FORMAT_R16G16B16A16_SFLOAT,false,1,1);
    gpu.MakeImage(gpu.images[1],iw,ih,VK_FORMAT_D32_SFLOAT,false,1,1);
    gpu.MakeImage(gpu.images[2],iw,ih,VK_FORMAT_R32G32_SFLOAT,false,1,1);
    gpu.MakeImage(gpu.images[3],640,360,VK_FORMAT_B10G11R11_UFLOAT_PACK32,true);
    gpu.MakeImage(gpu.images[4],800,450,VK_FORMAT_B10G11R11_UFLOAT_PACK32,true);
    gpu.MakeImage(gpu.images[5],1,1,VK_FORMAT_R32_SFLOAT,false);
    gpu.MakeImage(gpu.images[6],iw,ih,VK_FORMAT_B10G11R11_UFLOAT_PACK32,false,1,1);
    const auto resource=[](const SmokeGpu::Image& image) {
        return KytyDlssPssrImage {image.image,image.view,{image.aspect,image.mip,1,image.layer,1},image.format,image.width,image.height};
    };
    const auto make_feature=[&](uint32_t ow,uint32_t oh,bool auto_exposure) {
        KytyDlssOptimalSettings optimal {sizeof(optimal)};
        Check(api.query_optimal(h.native_session,ow,oh,0,&optimal,&error),error,"Native optimal range");
        KytyDlssPssrCreation creation {sizeof(creation),ow,oh,
            KYTY_DLSS_PSSR_REVERSE_DEPTH|(auto_exposure?KYTY_DLSS_PSSR_AUTO_EXPOSURE:0u),0,
            optimal.min_width,optimal.min_height,optimal.max_width,optimal.max_height};
        KytyDlssPssrFeature* feature {};
        Check(api.create_feature(h.native_session,&creation,&feature,&error),error,"Native feature allocation");
        h.native_features.push_back(feature);
        return creation;
    };
    auto first=make_feature(640,360,true);
    auto second=make_feature(800,450,false);
    report["native_dynamic_ranges"]={{first.min_width,first.min_height,first.max_width,first.max_height},
                                     {second.min_width,second.min_height,second.max_width,second.max_height}};
    if (api.destroy_session(h.native_session,1,&error)!=KYTY_DLSS_BUSY)
        throw std::runtime_error("Native session destroyed with live histories");
    if (api.destroy_feature(h.native_features[0],0,&error)!=KYTY_DLSS_INVALID_ARGUMENT)
        throw std::runtime_error("Native unsafe release accepted");
    constexpr size_t pixels=size_t(iw)*ih;
    const std::array<VkDeviceSize,5> offsets {0,pixels*8,pixels*12,pixels*20,pixels*20+4};
    gpu.MakeBuffer(gpu.buffers[0],pixels*24+4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    gpu.MakeBuffer(gpu.buffers[1],size_t(800)*450*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    bool output_initialized[2] {};
    uint64_t counts[2] {};
    report["native_frames"]=Json::array();
    report["kind"]="native PSSR ABI synthetic GPU integration";
    // The first six frames cover ABI/lifetime behavior. The remaining frames
    // check image content, not only finiteness: a stable linear HDR ramp must
    // remain stable through dynamic extents, pre-exposure and exposure changes.
    for (uint32_t frame=0;frame<38;++frame) {
        if (frame) { CheckVk(gpu.ResetFences(h.device,1,&gpu.fence),"Reset native fence"); CheckVk(gpu.ResetCommandPool(h.device,gpu.pool,0),"Reset native pool"); }
        const uint32_t context=frame%2;
        if (frame==4) {
            // The other feature still exists and must remain usable afterwards.
            Check(api.destroy_feature(h.native_features[0],1,&error),error,"Replace completed native history");
            h.native_features.erase(h.native_features.begin());
            first=make_feature(640,360,true);
            std::swap(h.native_features[0],h.native_features[1]); counts[0]=0;
        }
        auto& target=gpu.images[3+context];
        const auto& creation=context?second:first;
        const bool photometric=frame>=6;
        const uint32_t phase=photometric?(frame-6)/2:0;
        const uint32_t render_width=photometric?(phase%2?creation.min_width:creation.max_width):(context?400:320);
        const uint32_t render_height=photometric?(phase%2?creation.min_height:creation.max_height):(context?225:180);
        if (render_width+8>iw || render_height+8>ih) throw std::runtime_error("Native photometric allocation too small for dynamic range");
        const std::array<float,4> pre_exposures {.25f,1.f,4.f,16.f};
        const float pre_exposure=photometric?pre_exposures[phase%4]:1.f;
        const float exposure_value=photometric?pre_exposures[(phase/4)%4]:1.f;
        void* mapped {};
        CheckVk(gpu.MapMemory(h.device,gpu.buffers[0].memory,0,VK_WHOLE_SIZE,0,&mapped),"Native upload mapping");
        auto* colors=static_cast<uint16_t*>(mapped);
        auto* depths=reinterpret_cast<float*>(static_cast<std::byte*>(mapped)+offsets[1]);
        auto* motions=reinterpret_cast<float*>(static_cast<std::byte*>(mapped)+offsets[2]);
        auto* packed_colors=reinterpret_cast<uint32_t*>(static_cast<std::byte*>(mapped)+offsets[4]);
        for (uint32_t y=0;y<ih;++y) for (uint32_t x=0;x<iw;++x) {
            const auto p=size_t(y)*iw+x; const float u=float(x)/iw,v=float(y)/ih;
            if (photometric) {
                const bool inside=x>=8 && y>=8 && x<8+render_width && y<8+render_height;
                // Bright pixels outside the active subrect expose accidental
                // full-allocation reads without relying on undefined pixels.
                const float su=(float(x)-8.f+.5f)/render_width,sv=(float(y)-8.f+.5f)/render_height;
                colors[p*4]=TestHalf(pre_exposure*(inside?.2f+.4f*su:32.f));
                colors[p*4+1]=TestHalf(pre_exposure*(inside?.25f+.3f*sv:32.f));
                colors[p*4+2]=TestHalf(pre_exposure*(inside?.3f:32.f));
                colors[p*4+3]=TestHalf(1);
            } else {
                colors[p*4]=TestHalf(.1f+.6f*u); colors[p*4+1]=TestHalf(.1f+.6f*v);
                colors[p*4+2]=TestHalf(.2f+.1f*std::sin(u*18)); colors[p*4+3]=TestHalf(1);
            }
            depths[p]=.5f; motions[p*2]=motions[p*2+1]=0;
            // These finite nonnegative values share the half exponent bias.
            // Truncate its mantissa to 6/6/5 bits for the native packed color.
            packed_colors[p]=(uint32_t(colors[p*4])>>4)|((uint32_t(colors[p*4+1])>>4)<<11)|
                             ((uint32_t(colors[p*4+2])>>5)<<22);
        }
        *reinterpret_cast<float*>(static_cast<std::byte*>(mapped)+offsets[3])=exposure_value;
        gpu.UnmapMemory(h.device,gpu.buffers[0].memory);
        VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CheckVk(gpu.BeginCommandBuffer(gpu.command,&begin),"Begin native commands");
        for (uint32_t n=0;n<5;++n) {
            auto& input=gpu.images[n==4?6:n==3?5:n];
            gpu.Transition(input,frame?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                frame?VK_ACCESS_SHADER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT);
            auto copy=SmokeGpu::CopyRegion(input,offsets[n]);
            gpu.CmdCopyBufferToImage(gpu.command,gpu.buffers[0].buffer,input.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            gpu.Transition(input,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
        }
        gpu.Transition(target,output_initialized[context]?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            output_initialized[context]?VK_ACCESS_TRANSFER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue unwritten {};
        for (auto& channel:unwritten.float32) channel=std::numeric_limits<float>::quiet_NaN();
        const VkImageSubresourceRange target_range {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        gpu.CmdClearColorImage(gpu.command,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&unwritten,1,&target_range);
        gpu.Transition(target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_WRITE_BIT);
        auto exposure=resource(gpu.images[5]);
        KytyDlssPssrEvaluateInfo eval {};
        eval.struct_size=sizeof(eval); eval.command_buffer=gpu.command;
        eval.color=resource(gpu.images[context?6:0]); eval.depth=resource(gpu.images[1]); eval.motion=resource(gpu.images[2]); eval.output=resource(target);
        eval.exposure=context?&exposure:nullptr;
        eval.x=8; eval.y=8;
        eval.width=render_width; eval.height=render_height;
        if (frame==2 && creation.min_width<=304 && creation.min_height<=172) { eval.width=304; eval.height=172; }
        eval.motion_scale_x=float(eval.width); eval.motion_scale_y=float(eval.height);
        eval.pre_exposure=pre_exposure; eval.exposure_scale=1; eval.frame_time_ms=16.6667f;
        eval.jitter_x=photometric?0.f:.125f; eval.jitter_y=photometric?0.f:-.125f;
        eval.reset_history=frame==3 || (photometric && phase%8==0);
        KytyDlssRecordResult result {sizeof(result)};
        if (!frame) {
            auto invalid=eval; invalid.x=UINT32_MAX;
            if (api.record_evaluate(h.native_features[context],&invalid,&result,&error)!=KYTY_DLSS_INVALID_ARGUMENT)
                throw std::runtime_error("Native overflowing subrect accepted");
            invalid=eval; invalid.depth.range.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
            if (api.record_evaluate(h.native_features[context],&invalid,&result,&error)!=KYTY_DLSS_INVALID_ARGUMENT)
                throw std::runtime_error("Native incorrect depth aspect accepted");
            invalid=eval; invalid.jitter_x=std::numeric_limits<float>::quiet_NaN();
            if (api.record_evaluate(h.native_features[context],&invalid,&result,&error)!=KYTY_DLSS_INVALID_ARGUMENT)
                throw std::runtime_error("Native nonfinite jitter accepted");
        }
        Check(api.record_evaluate(h.native_features[context],&eval,&result,&error),error,"Native NGX record");
        if (result.feature_created!=(counts[context]==0) || result.reset_used!=(!counts[context]||eval.reset_history) || result.evaluations_recorded!=++counts[context])
            throw std::runtime_error("Native per-history creation/reset/evaluation count failed");
        gpu.Transition(target,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        const auto copy=SmokeGpu::CopyRegion(target);
        gpu.CmdCopyImageToBuffer(gpu.command,target.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,gpu.buffers[1].buffer,1,&copy);
        VkMemoryBarrier barrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        gpu.CmdPipelineBarrier(gpu.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        CheckVk(gpu.EndCommandBuffer(gpu.command),"End native commands");
        VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&gpu.command;
        CheckVk(gpu.QueueSubmit(gpu.queue,1,&submit,gpu.fence),"Submit native synthetic work");
        CheckVk(gpu.WaitForFences(h.device,1,&gpu.fence,VK_TRUE,30'000'000'000ull),"Wait native synthetic fence");
        output_initialized[context]=true;
        CheckVk(gpu.MapMemory(h.device,gpu.buffers[1].memory,0,VK_WHOLE_SIZE,0,&mapped),"Read native packed output");
        const auto* packed=static_cast<const uint32_t*>(mapped);
        size_t finite=0,measured=0; float low=10000,high=-10000;
        double absolute_error=0,mean=0;
        float max_error=0;
        for (size_t p=0;p<size_t(target.width)*target.height;++p) for (uint32_t channel=0;channel<3;++channel) {
            const uint32_t bits=channel==2?5:6, shift=channel==0?0:channel==1?11:22;
            const uint32_t v=packed[p]>>shift, exp=(v>>bits)&31, mant=v&((1u<<bits)-1);
            const float value=exp==31?std::numeric_limits<float>::quiet_NaN():std::ldexp(float(mant)+(exp?float(1u<<bits):0.f),int(exp?exp:1)-15-int(bits));
            if (std::isfinite(value)) { ++finite; low=std::min(low,value); high=std::max(high,value); }
            const uint32_t x=static_cast<uint32_t>(p%target.width),y=static_cast<uint32_t>(p/target.width);
            if (photometric && x>=16 && y>=16 && x+16<target.width && y+16<target.height) {
                const float expected=channel==0?.2f+.4f*(float(x)+.5f)/target.width:
                                     channel==1?.25f+.3f*(float(y)+.5f)/target.height:.3f;
                const float normalized=value/pre_exposure,delta=std::abs(normalized-expected);
                mean+=normalized; absolute_error+=delta; max_error=std::max(max_error,delta); ++measured;
            }
        }
        gpu.UnmapMemory(h.device,gpu.buffers[1].memory);
        if (finite!=size_t(target.width)*target.height*3 || high-low<.01f) throw std::runtime_error("Native packed RGB is nonfinite/empty/constant");
        report["native_frames"].push_back({{"frame",frame},{"context",context},{"render",{eval.width,eval.height}},
            {"output",{target.width,target.height}},{"created",result.feature_created},{"reset",result.reset_used},{"finite_rgb",finite},{"min",low},{"max",high},
            {"pre_exposure",pre_exposure},{"exposure_texture_value",exposure_value},{"photometric",photometric},
            {"normalized_mean",measured?mean/measured:0},{"normalized_mean_absolute_error",measured?absolute_error/measured:0},
            {"normalized_max_error",max_error}});
        if (photometric && (absolute_error/measured>.05 || max_error>.2f))
            throw std::runtime_error("Native HDR brightness/subrect regression");
    }
    for (auto* feature:h.native_features) Check(api.destroy_feature(feature,1,&error),error,"Destroy native feature after fence");
    h.native_features.clear();
    Check(api.destroy_session(h.native_session,1,&error),error,"Destroy native session after features"); h.native_session=nullptr;
    // Re-enter the unchanged v1 ownership path after native shutdown.
    Check(h.api.create_context(&ci,&h.ngx,&error),error,"Recreate v1 context after native shutdown");
    Check(h.api.destroy_context(h.ngx,1,&error),error,"Destroy v1 after native shutdown"); h.ngx=nullptr;
    report["native_subresource_mip_layer"]={1,1}; report["native_depth_aspect"]=true;
    report["native_shared_histories"]=2; report["native_invalid_preflight_rejected"]=true;
    report["native_photometric_frames"]=32; report["native_photometric_regression_passed"]=true;
    report["native_color_formats"]={"R16G16B16A16_SFLOAT","B10G11R11_UFLOAT_PACK32"};
    report["native_v1_lifecycle_reentry"]=true; report["passed"]=true;
}
