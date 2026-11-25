// Included by gt7Dlss.cpp. No legacy SR feature/evaluation is used here.
void Gt7Dlss::BeginNativePssr() {
    auto& s = *m_impl;
    if (!s.native_mode) return;
    if (s.fg_scene_closed || (s.fg_boundary_seen && s.fg_temporal_attempts)) s.ResetLiveScene();
    s.native_prior_reset = s.native_reset_pending;
    s.native_prior_reset_sources = s.native_reset_pending ? s.native_pending_sources : 0;
    s.native_reset_pending = true; // Cleared only when a complete scene is frozen.
    s.native_pending_sources = Gt7FrameGenLifecycle::PendingUnfinished;
    s.native_ready = false;
    s.fg_camera_valid = false;
    s.fg_hudless_valid = false;
    if (++s.fg_temporal_attempts != 1) s.fg_input_ambiguous = true;
}

void Gt7Dlss::RecordNativePssr(CommandBuffer& buffer, const Pssr::Request& request,
                              const TextureBinding& depth, const TextureBinding& motion,
                              const TextureBinding& output, bool reset_used) {
    auto& s = *m_impl;
    if (!s.native_mode || !Gt7FrameGen::InputsRequested(s.graphics)) return;
    const Image* diagnostic_depth = nullptr;
    const Image* diagnostic_motion = nullptr;
    const Image* diagnostic_output = nullptr;
    // The request and already observed CPU metadata are enough to attribute a
    // rejection. No extra backing reads, image resolution or GPU copies occur.
    const auto report = [&](bool captured, const char* why, uint64_t reason_count) {
        const auto& extent = diagnostic_output ? diagnostic_output->backing.extent : output.desc.info.extent;
        std::printf("GT7 native FG: input result=%s frame=%llu scene=%llu captured=%llu rejected=%llu reason_count=%llu reason=%s"
                    " depth=%u:%u motion=%u:%u output=%u:%u flags=0x%x context=0x%llx input=%ux%u origin=%u,%u output_extent=%ux%u"
                    " jitter=%.9g,%.9g motion_scale=%.9g,%.9g attempts=%u ambiguous=%u candidates=%zu\n",
            captured ? "captured" : "rejected", static_cast<unsigned long long>(s.fg_frame),
            static_cast<unsigned long long>(s.fg_scene_serial), static_cast<unsigned long long>(s.native_input_captures),
            static_cast<unsigned long long>(s.native_input_diagnostics.rejected),
            static_cast<unsigned long long>(reason_count), why,
            depth.image_id.index, depth.image_id.generation, motion.image_id.index, motion.image_id.generation,
            output.image_id.index, output.image_id.generation, request.flags,
            static_cast<unsigned long long>(request.context), request.width, request.height, request.x, request.y,
            extent.width, extent.height, request.jitter[0], request.jitter[1], request.motion_scale[0], request.motion_scale[1],
            s.fg_temporal_attempts, s.fg_input_ambiguous ? 1u : 0u, s.fg_cameras.size());
        s.ReportNativeCameraObservations();
        for (size_t i = 0; i < s.fg_cameras.size(); ++i) {
            const auto& candidate = s.fg_cameras[i]; // At most eight scene candidates.
            const bool pair = candidate.depth_id == depth.image_id && candidate.motion_id == motion.image_id;
            const bool current = pair && diagnostic_depth && diagnostic_motion;
            std::printf("GT7 native FG candidate: slot=%zu pair=%u depth=%u:%u motion=%u:%u valid=%u rejected=%u pending=%u reason=%s"
                        " matrices_read=%u projection_jitter=%.9g,%.9g viewport_depth=%.9g,%.9g jitter_matches=%u"
                        " camera_near=%.9g camera_far=%.9g current_available=%u images_same=%u,%u"
                        " depth_versions=%llu,%llu motion_versions=%llu,%llu depth_epochs=%llu,%llu motion_epochs=%llu,%llu\n",
                i, pair ? 1u : 0u, candidate.depth_id.index, candidate.depth_id.generation,
                candidate.motion_id.index, candidate.motion_id.generation, candidate.valid ? 1u : 0u,
                candidate.rejected ? 1u : 0u, candidate.native_witness_pending ? 1u : 0u,
                candidate.reject_reason ? candidate.reject_reason : "none",
                candidate.diagnostic_matrices_read ? 1u : 0u,
                candidate.diagnostic_projection_jitter_x, candidate.diagnostic_projection_jitter_y,
                candidate.diagnostic_viewport_depth.minimum, candidate.diagnostic_viewport_depth.maximum,
                candidate.diagnostic_matrices_read && Gt7NativePssrInputs::CameraJitterMatches(
                    candidate.diagnostic_projection_jitter_x, candidate.diagnostic_projection_jitter_y,
                    request.jitter, InputWidth, InputHeight) ? 1u : 0u,
                candidate.parameters.near_plane, candidate.parameters.far_plane, current ? 1u : 0u,
                current && candidate.depth_image == diagnostic_depth->backing.image ? 1u : 0u,
                current && candidate.motion_image == diagnostic_motion->backing.image ? 1u : 0u,
                static_cast<unsigned long long>(candidate.depth_external_version),
                static_cast<unsigned long long>(current ? diagnostic_depth->ExternalWriteVersion() : 0),
                static_cast<unsigned long long>(candidate.motion_external_version),
                static_cast<unsigned long long>(current ? diagnostic_motion->ExternalWriteVersion() : 0),
                static_cast<unsigned long long>(candidate.depth_external_epoch),
                static_cast<unsigned long long>(current ? diagnostic_depth->ExternalWriteEpoch() : 0),
                static_cast<unsigned long long>(candidate.motion_external_epoch),
                static_cast<unsigned long long>(current ? diagnostic_motion->ExternalWriteEpoch() : 0));
        }
    };
    const auto reject = [&](const char* why) {
        s.fg_scene_diagnostic.NoteFirst("native temporal input rejected", why, depth.image_id, motion.image_id);
        ++s.rejected;
        uint64_t reason_count = 0;
        if (s.native_input_diagnostics.Reject(why, std::chrono::steady_clock::now(), reason_count))
            report(false, why, reason_count);
    };
    if (s.fg_temporal_attempts != 1 || s.fg_input_ambiguous) {
        reject("multiple native temporal requests in one scene"); return;
    }
    for (const auto* binding : {&depth, &motion, &output}) {
        if (!binding->image_id || !binding->image_view || !BaseView(binding->desc)) {
            reject("unresolved image or non-base view"); return;
        }
    }
    auto& textures = s.context.GetTextureCache();
    auto& source_depth = textures.GetImage(depth.image_id);
    auto& source_motion = textures.GetImage(motion.image_id);
    const auto& result = textures.GetImage(output.image_id);
    diagnostic_depth = &source_depth;
    diagnostic_motion = &source_motion;
    diagnostic_output = &result;
    if (!Gt7NativePssrInputs::Compatible(request.width, request.height, request.x, request.y,
            result.backing.extent.width, result.backing.extent.height,
            (request.flags & Pssr::kReverseDepth) != 0, (request.flags & Pssr::kSpatialOnly) != 0,
            request.context, request.jitter, request.motion_scale) ||
        !Shape(source_depth, InputWidth, InputHeight, vk::Format::eD32SfloatS8Uint) ||
        !Shape(source_motion, InputWidth, InputHeight, vk::Format::eR16G16Sfloat) ||
        !Shape(result, GuestWidth, GuestHeight, vk::Format::eB10G11R11UfloatPack32) ||
        depth.desc.view_info.format != vk::Format::eR32Sfloat ||
        motion.desc.view_info.format != vk::Format::eR16G16Sfloat ||
        output.desc.view_info.format != vk::Format::eB10G11R11UfloatPack32) {
        reject("native dimensions, formats or scalar contract"); return;
    }
    const auto channel = [](vk::ComponentSwizzle actual, vk::ComponentSwizzle expected) {
        return actual == vk::ComponentSwizzle::eIdentity || actual == expected;
    };
    if (!channel(depth.desc.view_info.mapping.r, vk::ComponentSwizzle::eR) ||
        !channel(motion.desc.view_info.mapping.r, vk::ComponentSwizzle::eR) ||
        !channel(motion.desc.view_info.mapping.g, vk::ComponentSwizzle::eG) ||
        source_depth.backing.image == source_motion.backing.image ||
        result.backing.image == source_depth.backing.image || result.backing.image == source_motion.backing.image ||
        Overlap(depth.desc.info.data, motion.desc.info.data) || Overlap(output.desc.info.data, depth.desc.info.data) ||
        Overlap(output.desc.info.data, motion.desc.info.data)) {
        reject("input channel mapping or image alias"); return;
    }
    const auto camera = std::find_if(s.fg_cameras.begin(), s.fg_cameras.end(), [&](const auto& candidate) {
        return candidate.valid && !candidate.rejected && !candidate.native_witness_pending && candidate.depth_id == depth.image_id &&
            candidate.motion_id == motion.image_id &&
            Gt7NativePssrInputs::CameraJitterMatches(candidate.parameters.jitter_ndc_x,
                candidate.parameters.jitter_ndc_y, request.jitter, InputWidth, InputHeight);
    });
    if (camera == s.fg_cameras.end()) {
        for (const auto& candidate : s.fg_cameras) {
            if (candidate.depth_id == depth.image_id && candidate.motion_id == motion.image_id &&
                candidate.rejected && candidate.reject_reason) { reject(candidate.reject_reason); return; }
            if (candidate.depth_id == depth.image_id && candidate.motion_id == motion.image_id &&
                candidate.native_witness_pending) { reject("native camera inputs not finalized after attachment acquisition"); return; }
        }
        reject("no verified same-scene camera/depth/motion/jitter"); return;
    }
    if (!Gt7FrameGenInputWitness::Matches(camera->DepthWitness(), CurrentInputWitness(source_depth, depth.desc.info.data)) ||
        !Gt7FrameGenInputWitness::Matches(camera->MotionWitness(), CurrentInputWitness(source_motion, motion.desc.info.data))) {
        reject("native camera input replaced or externally written before capture"); return;
    }
    if (!s.InitializeNativeInputs()) { reject("native conversion unavailable"); return; }
    s.scheduler.EndRendering();
    const auto command = buffer.Handle();
    const auto depth_state = source_depth.backing.state, motion_state = source_motion.backing.state;
    Transit(source_depth, SampledLayout, ShaderRead, command);
    Transit(source_motion, SampledLayout, ShaderRead, command);
    Transit(*s.depth, vk::ImageLayout::eGeneral, ShaderWrite, command);
    Transit(*s.motion, vk::ImageLayout::eGeneral, ShaderWrite, command);
    const struct { uint32_t width, height; float scale_x, scale_y; } push {
        InputWidth, InputHeight, request.motion_scale[0], request.motion_scale[1]};
    const std::array<Image*, 2> sampled {&source_depth, &source_motion}, storage {s.depth.get(), s.motion.get()};
    const std::array<vk::ImageView, 2> views {depth.image_view, motion.image_view};
    Dispatch(command, s.native_prepare, sampled, storage, s.nearest, &push, InputWidth, InputHeight, views);
    Transit(*s.depth, SampledLayout, ShaderRead, command);
    Transit(*s.motion, SampledLayout, ShaderRead, command);
    Transit(source_depth, depth_state.layout, depth_state.access_mask, command);
    Transit(source_motion, motion_state.layout, motion_state.access_mask, command);
    const auto now = std::chrono::steady_clock::now();
    s.fg_frame_time_ms = s.last_time == std::chrono::steady_clock::time_point {} ? 0.0f :
        std::chrono::duration<float, std::milli>(now - s.last_time).count();
    const auto reset_reasons = Gt7FrameGenLifecycle::ProducerResetReasons(
        reset_used, s.native_prior_reset, s.native_context != request.context, s.fg_frame_time_ms);
    s.fg_reset = reset_reasons != 0;
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    if (s.native_reset_diagnostics.Report(reset_reasons, now_ms)) {
        std::printf("GT7 native FG reset: frame=%llu scene=%llu mask=0x%x sr=%u prior=%u prior_sources=0x%x"
                    " context=0x%llx previous_context=0x%llx interval_ms=%.6f reset=%u\n",
            static_cast<unsigned long long>(s.fg_frame), static_cast<unsigned long long>(s.fg_scene_serial),
            reset_reasons, reset_used ? 1u : 0u, s.native_prior_reset ? 1u : 0u, s.native_prior_reset_sources,
            static_cast<unsigned long long>(request.context), static_cast<unsigned long long>(s.native_context),
            s.fg_frame_time_ms, s.fg_reset ? 1u : 0u);
    }
    s.last_time = now;
    s.native_context = request.context;
    s.fg_camera = camera->parameters;
    s.fg_camera_valid = true;
    s.fg_jitter_x = request.jitter[0]; s.fg_jitter_y = request.jitter[1];
    s.fg_scaled_scene_id = output.image_id;
    s.native_output_image = result.backing.image;
    s.native_output_range = result.info.data;
    s.native_output_serial = result.content_serial;
    s.native_output_write_version = result.ExternalWriteVersion();
    s.native_ready = true;
    if (s.native_input_diagnostics.Capture(++s.native_input_captures, now)) report(true, "none", 0);
}
