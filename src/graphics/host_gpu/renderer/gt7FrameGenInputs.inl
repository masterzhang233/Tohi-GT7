// Included by gt7Dlss.cpp so source input images remain private to the SR helper.

static Gt7NativeDepthProjection::Status NativeDepthProjectionSnapshot(const ShaderStageRuntime& vertex,
    Gt7NativeDepthProjection::Matrix& matrix, uint64_t& matrix_address);

static void ReportNativeCameraPair(uint64_t shader, uint64_t frame, uint64_t scene,
    Gt7NativeCameraPair::Decision decision, Gt7NativeDepthProjection::Status snapshot_status,
    uint64_t structure_address, uint64_t matrix_address, const Gt7NativeCameraPair::Camera& first,
    const Gt7NativeCameraPair::Camera& incoming) {
    using Key = std::array<uint64_t, 2>;
    static thread_local std::array<Key, 64> seen {};
    static thread_local size_t count = 0;
    static thread_local int64_t next_report = 0;
    const Key key {shader, (uint64_t(decision) << 32) | uint64_t(snapshot_status)};
    const bool known = std::find(seen.begin(), seen.begin() + count, key) != seen.begin() + count;
    const bool first_class = !known && count < seen.size();
    if (first_class) seen[count++] = key;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!first_class && now < next_report) return;
    next_report = now + 5000;
    std::array<char, 3072> details {};
    size_t used = 0;
    const auto append = [&](const char* format, auto... args) {
        if (used >= details.size() - 1) return;
        const size_t remaining = details.size() - used;
        const int written = std::snprintf(details.data() + used, remaining, format, args...);
        if (written > 0) used += std::min<size_t>(size_t(written), remaining - 1);
    };
    append("GT7 native FG camera pair: frame=%llu scene=%llu vs=%016llx accepted=%u reason=%s"
           " snapshot=%s structure=0x%llx+192 bound_matrix=0x%llx first_range=%.9g,%.9g incoming_range=%.9g,%.9g"
           " pose_same=%u nondepth_projection_same=%u anchor_retained=%u",
        static_cast<unsigned long long>(frame), static_cast<unsigned long long>(scene),
        static_cast<unsigned long long>(shader), unsigned(decision == Gt7NativeCameraPair::Decision::RetainAnchor),
        Gt7NativeCameraPair::Name(decision), Gt7NativeDepthProjection::Name(snapshot_status),
        static_cast<unsigned long long>(structure_address), static_cast<unsigned long long>(matrix_address),
        first.range.minimum, first.range.maximum, incoming.range.minimum, incoming.range.maximum,
        unsigned(first.view == incoming.view),
        unsigned(Gt7NativeCameraPair::SameNonDepthProjection(first.projection, incoming.projection)),
        unsigned(decision == Gt7NativeCameraPair::Decision::RetainAnchor));
    const auto matrix = [&](const char* name, const auto& value) {
        append(" %s={", name);
        for (size_t i = 0; i < value.size(); ++i) append("%s%.9g", i ? "," : "", value[i]);
        append("%s", "}");
    };
    matrix("first_vp", first.view_projection);
    matrix("incoming_vp", incoming.view_projection);
    std::printf("%s\n", details.data());
    std::fflush(stdout);
}
bool Gt7Dlss::IsFrameGenerationCameraShader(const ShaderStageRuntime& vertex) {
    if (!vertex || vertex.program->stage != ShaderType::Vertex) return false;
    const auto hash = vertex.program->shader_hash;
    return vertex.program->gt7_camera_position || hash == 0x48a3c58d37f8582fULL ||
           hash == 0xd24c8423cd8ac4f3ULL || hash == 0x38971d1e2fbabfc8ULL;
}

void Gt7Dlss::DescribeFrameGenerationDraw(const ShaderStageRuntime& vertex, uint64_t pixel_shader,
                                        const RenderDepthInfo& depth,
                                        std::span<const RenderColorInfo> colors,
                                        bool full_viewport, bool storage_writes,
                                        const Gt7FrameGenCamera::RasterState& raster) {
    auto& s = *m_impl;
    if (!Gt7FrameGen::InputsRequested(s.graphics) ||
        !vertex || !full_viewport || !depth.image_id) return;
    const RenderColorInfo* motion = nullptr;
    for (const auto& color : colors) if (color.target_slot == 1) motion = &color;
    if (!motion || !motion->image_id) return;
    auto& textures = s.context.GetTextureCache();
    const auto& source_depth = textures.GetImage(depth.image_id);
    const auto& source_motion = textures.GetImage(motion->image_id);
    if (!Shape(source_depth, InputWidth, InputHeight, vk::Format::eD32SfloatS8Uint) ||
        !Shape(source_motion, InputWidth, InputHeight, vk::Format::eR16G16Sfloat)) return;
    ++s.fg_candidate_draws;
    if (CameraProbeEnabled() &&
        vertex.program->stage == ShaderType::Vertex && !storage_writes && s.fg_temporal_attempts == 0 &&
        depth.depth_test_enable && depth.depth_write_enable &&
        depth.depth_compare_op == vk::CompareOp::eGreaterOrEqual &&
        !depth.guest_array_layer && BaseView(depth.desc) && BaseView(motion->desc) &&
        !motion->guest_mip_level && !motion->guest_array_layer) {
        CollectCameraProbe(s.fg_camera_probe, s.context, vertex, pixel_shader,
                           depth.image_id, motion->image_id, s.fg_frame, s.fg_candidate_draws,
                           IsFrameGenerationCameraShader(vertex), full_viewport, raster);
    }
    const std::array<uint64_t, 2> signature {vertex.program->shader_hash, pixel_shader};
    if (s.fg_draw_signatures.size() >= 16 ||
        std::find(s.fg_draw_signatures.begin(), s.fg_draw_signatures.end(), signature) != s.fg_draw_signatures.end()) return;
    s.fg_draw_signatures.push_back(signature);
    LOGF("GT7 FSR FG: producer signature #%zu vs=%016llx ps=%016llx depth=%u:%u motion=%u:%u depth_test=%u depth_write=%u depth_compare=%u full_viewport=%u storage_writes=%u base_views=%u/%u\n",
         s.fg_draw_signatures.size(), static_cast<unsigned long long>(signature[0]),
         static_cast<unsigned long long>(signature[1]), depth.image_id.index, depth.image_id.generation,
         motion->image_id.index, motion->image_id.generation, depth.depth_test_enable ? 1u : 0u,
         depth.depth_write_enable ? 1u : 0u, static_cast<uint32_t>(depth.depth_compare_op),
         full_viewport ? 1u : 0u, storage_writes ? 1u : 0u, BaseView(depth.desc) ? 1u : 0u,
         BaseView(motion->desc) ? 1u : 0u);
}

bool Gt7Dlss::ObserveFrameGenerationCamera(const ShaderStageRuntime& vertex,
                                         const RenderDepthInfo& depth,
                                         std::span<const RenderColorInfo> colors,
                                         const Gt7FrameGenCamera::RasterState& raster,
                                         const ShaderStageRuntime* trusted_depth_vertex) {
    auto& s = *m_impl;
    if (!Gt7FrameGen::InputsRequested(s.graphics) || !vertex ||
        vertex.program->stage != ShaderType::Vertex ||
        !IsFrameGenerationCameraShader(vertex)) return false;
    auto& observations = s.fg_camera_observations;
    const auto observe = [&](CameraObservation branch) {
        ++observations[branch];
        if (s.native_mode) ++s.native_camera_observations[branch];
    };
    observe(CameraCalls);
    if (!depth.image_id || !depth.depth_test_enable || !depth.depth_write_enable ||
        depth.depth_compare_op != vk::CompareOp::eGreaterOrEqual ||
        depth.guest_array_layer != 0 || !BaseView(depth.desc)) { observe(CameraDepth); return false; }
    const RenderColorInfo* motion = nullptr;
    for (const auto& color : colors) if (color.target_slot == 1) motion = &color;
    if (!motion || !motion->image_id || !BaseView(motion->desc) ||
        motion->guest_mip_level || motion->guest_array_layer) { observe(CameraMotion); return false; }
    auto& textures = s.context.GetTextureCache();
    if (!Shape(textures.GetImage(depth.image_id), InputWidth, InputHeight, vk::Format::eD32SfloatS8Uint) ||
        !Shape(textures.GetImage(motion->image_id), InputWidth, InputHeight, vk::Format::eR16G16Sfloat)) {
        observe(CameraShape); return false;
    }
    // A complete display output owns independent depth/MV/color copies now.
    // The next proved producer starts a new live scene even if the CPU has not
    // submitted the preceding output's flip yet. A rejected temporal sequence
    // can also recover after a real flip, without resetting an in-progress
    // camera set merely because an older output was prepared asynchronously.
    if (s.fg_scene_closed || (s.fg_boundary_seen && s.fg_temporal_attempts != 0)) {
        s.ResetLiveScene();
        ++observations[CameraCalls];
    }
    // A new scene writer after the temporal evaluation breaks the input/final
    // color association even if no second TAA has run yet.
    if (s.fg_temporal_attempts != 0) {
        s.fg_scene_diagnostic.NoteFirst("late camera writer", "camera producer after temporal pass",
                                      depth.image_id, motion->image_id, vertex.program->shader_hash);
        s.fg_camera_valid = false;
        s.fg_input_ambiguous = true;
        observe(CameraLate);
        return false;
    }
    auto entry = std::find_if(s.fg_cameras.begin(), s.fg_cameras.end(), [&](const auto& camera) {
        return camera.depth_id == depth.image_id && camera.motion_id == motion->image_id;
    });
    if (entry == s.fg_cameras.end()) {
        if (s.fg_cameras.size() >= 8) { observe(CameraLimit); return false; }
        s.fg_cameras.emplace_back();
        entry = std::prev(s.fg_cameras.end());
        entry->depth_id = depth.image_id;
        entry->motion_id = motion->image_id;
        entry->depth_range = depth.desc.info.data;
        entry->motion_range = motion->desc.info.data;
    }
    if (entry->rejected) {
        NoteCameraProbeObservation(s.fg_camera_probe, vertex, depth.image_id, motion->image_id,
                                   s.fg_frame, "pair already rejected");
        observe(CameraAlreadyRejected); return false;
    }
    const auto reject = [&](CameraObservation observation, const char* reason) {
        NoteCameraProbeObservation(s.fg_camera_probe, vertex, depth.image_id, motion->image_id, s.fg_frame, reason);
        observe(observation); entry->valid = false; entry->rejected = true; entry->reject_reason = reason;
    };
    const auto& depth_image = textures.GetImage(depth.image_id);
    const auto& motion_image = textures.GetImage(motion->image_id);
    const auto depth_witness = CurrentInputWitness(depth_image, depth.desc.info.data);
    const auto motion_witness = CurrentInputWitness(motion_image, motion->desc.info.data);
    if (s.native_mode && entry->valid &&
        (entry->native_witness_pending || !Gt7FrameGenInputWitness::Matches(entry->DepthWitness(), depth_witness) ||
         !Gt7FrameGenInputWitness::Matches(entry->MotionWitness(), motion_witness))) {
        reject(CameraChanged, "native camera input replaced or externally written"); return false;
    }
    const auto& user = vertex.resources->user_data;
    const auto base = vertex.program->user_data_base;
    if (base > 10 || user.size() < size_t(12 - base) || !vertex.resources->gpu_fills.empty()) {
        reject(CameraUser, "user-data registers or GPU fills"); return false;
    }
    const size_t reg = 10 - base;
    const uint64_t address = uint64_t(user[reg]) | (uint64_t(user[reg + 1]) << 32u);
    constexpr uint64_t LastByte = 272;
    if (!address || (address & 3u) || address > std::numeric_limits<uint64_t>::max() - LastByte) {
        reject(CameraAddress, "camera pointer alignment/range"); return false;
    }
    const auto read = [&](uint64_t offset, void* destination, uint64_t bytes) {
        const auto source = address + offset;
        if (Libs::LibKernel::Memory::TryClampRangeSize(source, bytes) != bytes) {
            reject(CameraUnmapped, "camera matrix range not mapped"); return false;
        }
        if (!s.context.GetBufferCache().IsCleanForConcurrentRead(source, bytes)) {
            reject(CameraGpuDirty, "camera matrix range GPU dirty"); return false;
        }
        if (!Libs::LibKernel::Memory::TryReadBacking(source, destination, bytes)) {
            reject(CameraRead, "camera matrix backing read failed"); return false;
        }
        return true;
    };
    std::array<float, 48> current {};
    if (!read(80, current.data(), sizeof(current))) {
        return false;
    }
    Gt7FrameGenCamera::Matrix view_projection {}, view {}, projection {};
    std::copy_n(current.begin(), 16, view_projection.begin());
    std::copy_n(current.begin() + 16, 16, view.begin());
    std::copy_n(current.begin() + 32, 16, projection.begin());
    entry->diagnostic_matrices_read = true;
    entry->diagnostic_projection_jitter_x = projection[8];
    entry->diagnostic_projection_jitter_y = projection[9];
    entry->diagnostic_viewport_depth = Gt7FrameGenCamera::ViewportDepthRange(raster);
    Gt7FrameGenCamera::Parameters parameters {};
    const char* decode_failure = nullptr;
    const bool decoded = s.native_mode
        ? Gt7FrameGenCamera::DecodeNative(view_projection, view, projection, raster,
                                        InputWidth, InputHeight, parameters, &decode_failure)
        : Gt7FrameGenCamera::Decode(view_projection, view, projection,
                                  InputWidth, InputHeight, parameters, &decode_failure);
    if (!decoded) {
        NoteCameraProbeMatrices(s.fg_camera_probe, vertex, depth.image_id, motion->image_id,
                                s.fg_frame, address, view_projection, view, projection);
        reject(CameraMatrix, decode_failure); return false;
    }
    const auto viewport_depth = Gt7FrameGenCamera::ViewportDepthRange(raster);
    if (s.native_mode) {
        bool mixed = false;
        // Multiple motion views can share one depth attachment. An incompatible
        // writer invalidates every existing witness, including this new pair.
        for (auto& other : s.fg_cameras) {
            if (&other == &*entry ||
                (other.depth_id != depth.image_id && other.motion_id != motion->image_id &&
                 other.motion_id != depth.image_id && other.depth_id != motion->image_id &&
                 !textures.WriteOverlapsImage(depth.desc.info.data, other.depth_id, other.depth_range) &&
                 !textures.WriteOverlapsImage(motion->desc.info.data, other.motion_id, other.motion_range) &&
                 !textures.WriteOverlapsImage(depth.desc.info.data, other.motion_id, other.motion_range) &&
                 !textures.WriteOverlapsImage(motion->desc.info.data, other.depth_id, other.depth_range))) continue;
            if (other.depth_id != depth.image_id || other.motion_id != motion->image_id) {
                // Shared depth with different MV storage (or the converse)
                // does not prove that either pair covers the same geometry.
                other.valid = false;
                other.rejected = true;
                other.reject_reason = "native input pairs share only depth or motion storage";
                mixed = true;
                continue;
            }
            if (other.rejected) { mixed = true; continue; }
            if (!other.valid) continue;
            if (other.viewport_depth == viewport_depth && other.view_projection == view_projection &&
                other.view == view && other.projection == projection) continue;
            other.valid = false;
            other.rejected = true;
            other.reject_reason = "mixed camera or viewport depth mapping on shared native inputs";
            mixed = true;
        }
        if (mixed) {
            reject(CameraChanged, "mixed camera or viewport depth mapping on shared native inputs");
            return false;
        }
        if (entry->valid) {
            // The first camera is canonical. A compensated representation must
            // not silently replace its pose, parameters, range or input witnesses.
            const bool unchanged = entry->viewport_depth == viewport_depth &&
                entry->view_projection == view_projection && entry->view == view && entry->projection == projection;
            if (!unchanged) {
                namespace Pair = Gt7NativeCameraPair;
                namespace Projection = Gt7NativeDepthProjection;
                const Pair::Camera first {entry->view_projection, entry->view, entry->projection,
                    {entry->viewport_depth.minimum, entry->viewport_depth.maximum}};
                const Pair::Camera incoming {view_projection, view, projection,
                    {viewport_depth.minimum, viewport_depth.maximum}};
                Pair::Proof proof {
                    .native = s.native_mode, .valid = entry->valid, .rejected = entry->rejected,
                    .pending = entry->native_witness_pending,
                    .exact_pair = entry->depth_id == depth.image_id && entry->motion_id == motion->image_id,
                    .decoded = decoded,
                    .trusted_vertex = trusted_depth_vertex && *trusted_depth_vertex &&
                        trusted_depth_vertex->program == vertex.program &&
                        trusted_depth_vertex->resources == vertex.resources &&
                        trusted_depth_vertex->resource_plan == vertex.resource_plan};
                auto snapshot_status = Projection::Status::NotAttempted;
                uint64_t matrix_address = 0;
                // Pose/jitter/FOV mismatches never gain authority from a depth
                // equivalence test. This branch is only a representation change.
                if (first.view == incoming.view && Pair::SameNonDepthProjection(first.projection, incoming.projection) &&
                    proof.trusted_vertex) {
                    Projection::Matrix actual {};
                    snapshot_status = NativeDepthProjectionSnapshot(*trusted_depth_vertex, actual, matrix_address);
                    proof.snapshot_ready = snapshot_status == Projection::Status::Ready;
                    proof.snapshot_address_matches = matrix_address == address + 80;
                    proof.snapshot_bits_match = Projection::SameBits(actual, view_projection);
                    if (proof.snapshot_ready && proof.snapshot_address_matches && proof.snapshot_bits_match) {
                        // View/projection from the same already-read 48 words
                        // also participate in eligibility. Protect the full
                        // structure, not just its 64-byte bound VP prefix.
                        const GuestRange range {address + 80, sizeof(current)};
                        const auto aliases = [&](ImageId id, GuestRange watched) {
                            return textures.WriteOverlapsImage(range, id, watched);
                        };
                        bool feedback = aliases(entry->depth_id, entry->depth_range) ||
                            aliases(entry->motion_id, entry->motion_range) || aliases(depth.image_id, depth.desc.info.data);
                        for (const auto& color : colors)
                            feedback |= color.image_id && aliases(color.image_id, color.desc.info.data);
                        proof.no_attachment_alias = !feedback;
                        proof.clean_after = s.context.GetBufferCache().IsCleanForConcurrentRead(range.address, range.size);
                        proof.witnesses_match = Gt7FrameGenInputWitness::Matches(entry->DepthWitness(),
                            CurrentInputWitness(textures.GetImage(depth.image_id), depth.desc.info.data)) &&
                            Gt7FrameGenInputWitness::Matches(entry->MotionWitness(),
                                CurrentInputWitness(textures.GetImage(motion->image_id), motion->desc.info.data));
                    }
                }
                const auto decision = Pair::Evaluate(first, incoming, proof);
                ReportNativeCameraPair(vertex.program->shader_hash, s.fg_frame, s.fg_scene_serial,
                    decision, snapshot_status, address + 80, matrix_address, first, incoming);
                if (decision != Pair::Decision::RetainAnchor) {
                    reject(CameraChanged, Pair::Name(decision));
                    return false;
                }
            }
            // Do not flow through the first-observation assignments below.
            // Finalize still checks the same witnesses after AcquireRenderTargets.
            NoteCameraProbeObservation(s.fg_camera_probe, vertex, depth.image_id, motion->image_id, s.fg_frame,
                unchanged ? "accepted" : "accepted equivalent existing camera");
            observe(CameraAccepted);
            return true;
        }
    }
    if (entry->valid && (entry->view_projection != view_projection || entry->view != view ||
                        entry->projection != projection)) {
        NoteCameraProbeMatrices(s.fg_camera_probe, vertex, depth.image_id, motion->image_id,
                                s.fg_frame, address, view_projection, view, projection);
        reject(CameraChanged, "camera changed for the same depth/motion targets"); return false;
    }
    entry->view_projection = view_projection;
    entry->view = view;
    entry->projection = projection;
    entry->parameters = parameters;
    entry->viewport_depth = viewport_depth;
    if (!s.native_mode || !entry->valid) {
        entry->depth_image = depth_image.backing.image;
        entry->motion_image = motion_image.backing.image;
        entry->depth_external_version = depth_witness.version;
        entry->motion_external_version = motion_witness.version;
        entry->depth_external_epoch = depth_witness.epoch;
        entry->motion_external_epoch = motion_witness.epoch;
        // The first native witness permits only completion of this known
        // pre-acquire state, and is committed immediately after acquisition.
        entry->native_witness_pending = s.native_mode;
    }
    entry->valid = true;
    NoteCameraProbeObservation(s.fg_camera_probe, vertex, depth.image_id, motion->image_id, s.fg_frame, "accepted");
    observe(CameraAccepted);
    return true;
}

bool Gt7Dlss::FinalizeFrameGenerationCamera(const RenderDepthInfo& depth,
                                           std::span<const RenderColorInfo> colors) {
    auto& s = *m_impl;
    if (!s.native_mode) return true;
    const RenderColorInfo* motion = nullptr;
    for (const auto& color : colors) if (color.target_slot == 1) motion = &color;
    if (!depth.image_id || !motion || !motion->image_id) return false;
    const auto entry = std::find_if(s.fg_cameras.begin(), s.fg_cameras.end(), [&](const auto& camera) {
        return camera.depth_id == depth.image_id && camera.motion_id == motion->image_id;
    });
    if (entry == s.fg_cameras.end() || !entry->valid || entry->rejected) return false;
    auto& textures = s.context.GetTextureCache();
    const auto current_depth = CurrentInputWitness(textures.GetImage(depth.image_id), depth.desc.info.data);
    const auto current_motion = CurrentInputWitness(textures.GetImage(motion->image_id), motion->desc.info.data);
    const auto decision = Gt7FrameGenInputWitness::AfterAcquire(entry->native_witness_pending,
        entry->valid, entry->rejected, entry->DepthWitness(), entry->MotionWitness(), current_depth, current_motion);
    if (decision == Gt7FrameGenInputWitness::Finalize::Reject) {
        entry->valid = false;
        entry->rejected = true;
        entry->reject_reason = "native camera input replaced or externally written during attachment acquisition";
        ++s.fg_camera_observations[CameraChanged];
        ++s.native_camera_observations[CameraChanged];
        return false;
    }
    if (decision == Gt7FrameGenInputWitness::Finalize::CaptureFirst) {
        // Retain the exact stamp that was checked, never a later reread that
        // could hide an invalidation arriving after the decision.
        entry->depth_external_version = current_depth.version;
        entry->motion_external_version = current_motion.version;
        entry->depth_external_epoch = current_depth.epoch;
        entry->motion_external_epoch = current_motion.epoch;
        entry->native_witness_pending = false;
    }
    return true;
}

// Match the address semantics of the retained scalar reads, rather than assuming
// a flat-buffer offset is stable across fetch/permutation or compiler changes.
static Gt7NativeDepthProjection::Status NativeDepthProjectionSnapshot(const ShaderStageRuntime& vertex,
    Gt7NativeDepthProjection::Matrix& matrix, uint64_t& matrix_address) {
    using namespace ShaderRecompiler::IR;
    namespace Projection = Gt7NativeDepthProjection;
    using Status = Projection::Status;
    if (!vertex || vertex.program->stage != ShaderType::Vertex || !vertex.resource_plan) return Status::MissingPlan;
    const auto& contract = vertex.program->position_matrix;
    const bool typed = contract.kind == PositionMatrixKind::UserDataRowMajor4x4V1 &&
        contract.byte_offset == 80 && contract.user_sgpr < NumScalarRegs - 1;
    // Retain the independently audited exact layouts as a migration fallback.
    // A typed proof never changes the current snapshot/live-memory safeguards.
    const auto reg = typed ? contract.user_sgpr : Projection::CameraRegister(vertex.program->shader_hash);
    const uint32_t matrix_offset = typed ? contract.byte_offset : 80u;
    const auto& plan = *vertex.resource_plan;
    const auto& snapshot = *vertex.resources;
    if (reg == Projection::Missing || plan.stage != vertex.program->stage ||
        plan.shader_hash != vertex.program->shader_hash ||
        plan.user_data_base != vertex.program->user_data_base) return Status::Layout;
    if (!snapshot.gpu_fills.empty()) return Status::GpuFills;
    const auto base = vertex.program->user_data_base;
    if (base > reg || snapshot.user_data.size() < size_t(reg + 2 - base)) return Status::UserData;
    const auto pointer = uint64_t(snapshot.user_data[reg - base]) |
        (uint64_t(snapshot.user_data[reg + 1 - base]) << 32u);
    if (!pointer || (pointer & 3u) || pointer > std::numeric_limits<uint64_t>::max() - matrix_offset - 64u) return Status::Pointer;
    matrix_address = pointer + matrix_offset;
    const auto bits = [](Value value) {
        for (unsigned depth = 0; depth < 16; ++depth) {
            const auto* inst = value.TryInstruction();
            if (!inst) break;
            const auto op = inst->GetOpcode();
            if (op != ValueOpcode::Identity && op != ValueOpcode::BitCastF32U32 &&
                op != ValueOpcode::BitCastU32F32) break;
            if (inst->NumArgs() != 1) return Value {};
            value = inst->Arg(0);
        }
        return value;
    };
    const auto immediate = [](Value value, Type type, uint64_t expected) {
        return value.IsImmediate() && value.GetType() == type && value.ImmediateBits() == expected;
    };
    const auto user = [&](Value value, uint32_t expected) {
        const auto* inst = bits(value).TryInstruction();
        return inst && inst->GetOpcode() == ValueOpcode::GetUserData && inst->NumArgs() == 1 &&
            inst->Arg(0).GetType() == Type::ScalarReg && RegIndex(inst->Arg(0).ScalarRegister()) == expected;
    };
    Projection::Slots slots;
    slots.fill(Projection::Missing);
    for (const auto& read : plan.srt_reads) {
        const auto* load = bits(read.value).TryInstruction();
        if (!load || load->GetOpcode() != ValueOpcode::LoadAddressU32 || load->NumArgs() != 4 ||
            !immediate(load->Arg(2), Type::U32, 0) || !immediate(load->Arg(3), Type::U1, 1)) continue;
        const auto index = load->Flags<MemoryFlags>().index;
        if (index >= plan.memory_info.size()) continue;
        const auto& memory = plan.memory_info[index];
        if (memory.kind != ResourceKind::ScalarAddress || memory.data_bits != 32 || memory.data_dwords != 1 ||
            !load->Arg(1).IsImmediate() || load->Arg(1).GetType() != Type::U32) continue;
        if (memory.offset > INT32_MAX || (memory.offset & 3u) || (load->Arg(1).U32() & 3u)) continue;
        const auto* address = bits(load->Arg(0)).TryInstruction();
        if (!address || address->GetOpcode() != ValueOpcode::GetAddressResource || address->NumArgs() != 2 ||
            !user(address->Arg(0), reg) || !user(address->Arg(1), reg + 1)) continue;
        const auto offset = uint64_t(memory.offset) + load->Arg(1).U32();
        if (offset < matrix_offset || offset >= uint64_t(matrix_offset) + 64u || (offset & 3u)) continue;
        auto& slot = slots[(offset - matrix_offset) / 4];
        if (slot != Projection::Missing && slot != read.flat_offset) return Status::ConflictingSlots;
        slot = read.flat_offset;
    }
    // RebindBuffers may patch these exact pairs before upload. They must not
    // overlap any camera slot; all GPU-filled slots were excluded above.
    for (const auto& heap : snapshot.bindless_heaps)
        if (Projection::PatchTouches(slots, heap.mapping_offset)) return Status::BindlessPatch;
    for (const auto& heap : snapshot.bindless_sampler_heaps)
        if (Projection::PatchTouches(slots, heap.mapping_offset)) return Status::BindlessPatch;
    return Projection::ReadSnapshot(slots, snapshot.flattened_srt, false, matrix) ? Status::Ready : Status::MissingSlots;
}

static void ReportNativeDepthProjection(uint64_t shader, uint64_t frame, uint64_t scene,
    Gt7NativeDepthProjection::Status status, uint64_t address, bool matrix_read,
    const Gt7NativeDepthProjection::Matrix& actual, Gt7NativeDepthProjection::DepthRange actual_range,
    const FrameGenerationCamera& saved) {
    // One report per new (shader, outcome), bounded to 64 classes, then one
    // sample every five seconds. No matrix reread and no per-draw output.
    using Key = std::pair<uint64_t, Gt7NativeDepthProjection::Status>;
    static thread_local std::array<Key, 64> seen {};
    static thread_local size_t count = 0;
    static thread_local uint64_t attempts = 0;
    static thread_local int64_t next_report = 0;
    ++attempts;
    const Key key {shader, status};
    const bool first = count < seen.size() && std::find(seen.begin(), seen.begin() + count, key) == seen.begin() + count;
    if (first) seen[count++] = key;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!first && now < next_report) return;
    next_report = now + 5000;
    uint32_t non_depth_mismatch_mask = matrix_read ? 0u : 0xffffu;
    if (matrix_read) for (size_t i = 0; i < actual.size(); ++i)
        if (i % 4 != 2 && actual[i] != saved.view_projection[i]) non_depth_mismatch_mask |= 1u << i;
    std::printf("GT7 native FG depth projection: frame=%llu scene=%llu vs=%016llx attempt=%llu first_class=%u status=%s"
                " pointer=0x%llx snapshot_read=%u non_depth_mismatch_mask=0x%x ranges={actual=%.9g,%.9g saved=%.9g,%.9g}"
                " actual_z={%.9g,%.9g,%.9g,%.9g} actual_w={%.9g,%.9g,%.9g,%.9g}"
                " saved_z={%.9g,%.9g,%.9g,%.9g} saved_w={%.9g,%.9g,%.9g,%.9g}\n",
        static_cast<unsigned long long>(frame), static_cast<unsigned long long>(scene),
        static_cast<unsigned long long>(shader), static_cast<unsigned long long>(attempts), unsigned(first),
        Gt7NativeDepthProjection::Name(status), static_cast<unsigned long long>(address), unsigned(matrix_read), non_depth_mismatch_mask,
        double(actual_range.minimum), double(actual_range.maximum), double(saved.viewport_depth.minimum), double(saved.viewport_depth.maximum),
        double(actual[2]), double(actual[6]), double(actual[10]), double(actual[14]),
        double(actual[3]), double(actual[7]), double(actual[11]), double(actual[15]),
        double(saved.view_projection[2]), double(saved.view_projection[6]), double(saved.view_projection[10]), double(saved.view_projection[14]),
        double(saved.view_projection[3]), double(saved.view_projection[7]), double(saved.view_projection[11]), double(saved.view_projection[15]));
    std::fflush(stdout);
}

static unsigned ClaimNativeDepthStructureSample(uint64_t shader) {
    // A process-wide budget, including failed samples. Claim before any extra
    // range query or read. Monotonic slots also avoid sampling one shader twice.
    static std::array<std::atomic<uint64_t>, 4> shaders {};
    if (!shader) return 0;
    for (size_t i = 0; i < shaders.size(); ++i) {
        uint64_t empty = 0;
        if (shaders[i].compare_exchange_strong(empty, shader, std::memory_order_relaxed))
            return static_cast<unsigned>(i + 1);
        if (empty == shader) return 0;
    }
    return 0;
}

static void ReportNativeDepthStructure(unsigned sample, uint64_t shader, uint64_t frame, uint64_t scene,
    uint64_t address, const char* result, const Gt7NativeDepthProjection::Matrix& actual,
    Gt7NativeDepthProjection::DepthRange actual_range, const FrameGenerationCamera& saved,
    std::span<const float> candidate) {
    std::array<char, 4096> details {};
    size_t used = 0;
    const auto append = [&](const char* format, auto... args) {
        if (used >= details.size() - 1) return;
        const auto remaining = details.size() - used;
        const int written = std::snprintf(details.data() + used, remaining, format, args...);
        if (written > 0) used += std::min<size_t>(static_cast<size_t>(written), remaining - 1);
    };
    append("GT7 native FG depth structure: sample=%u/4 frame=%llu scene=%llu vs=%016llx"
           " range=0x%llx+0xc0 result=%s candidate_read=%u candidate_layout=unproved"
           " candidate_offsets={vp=0x0,view=0x40,projection=0x80}"
           " depth_ranges={actual=%.9g,%.9g saved=%.9g,%.9g}",
        sample, static_cast<unsigned long long>(frame), static_cast<unsigned long long>(scene),
        static_cast<unsigned long long>(shader), static_cast<unsigned long long>(address), result,
        unsigned(candidate.size() == 48), double(actual_range.minimum), double(actual_range.maximum),
        double(saved.viewport_depth.minimum), double(saved.viewport_depth.maximum));
    const auto matrix = [&](const char* name, std::span<const float> values) {
        append(" %s={", name);
        for (size_t i = 0; i < values.size(); ++i) append("%s%.9g", i ? "," : "", double(values[i]));
        append("%s", "}");
    };
    matrix("actual_vp", actual);
    matrix("saved_vp", saved.view_projection);
    matrix("saved_view", saved.view);
    matrix("saved_projection", saved.projection);
    matrix("candidate48_unproved", candidate);
    std::printf("%s\n", details.data());
    std::fflush(stdout);
}

Gt7Dlss::NativeInputDrawRejection Gt7Dlss::CheckNativeFrameGenerationInputDraw(const RenderDepthInfo& depth,
    std::span<const RenderColorInfo> colors, bool verified_camera, bool clear_depth, float clear_value,
    float viewport_min_depth, float viewport_max_depth, bool viewport_depths_match,
    const ShaderStageRuntime* trusted_depth_vertex) {
    NativeInputDrawRejection diagnostic;
    if (!WatchesFrameGenerationInputs()) return diagnostic;
    auto& s = *m_impl;
    auto& textures = s.context.GetTextureCache();
    const RenderColorInfo* motion = nullptr;
    for (const auto& color : colors) if (color.target_slot == 1) motion = &color;
    const auto touches = [&](ImageId id, GuestRange range, const FrameGenerationCamera& camera) {
        return (id && (id == camera.depth_id || id == camera.motion_id)) ||
            textures.WriteOverlapsImage(range, camera.depth_id, camera.depth_range) ||
            textures.WriteOverlapsImage(range, camera.motion_id, camera.motion_range);
    };
    for (auto& camera : s.fg_cameras) {
        if (!camera.valid || camera.rejected) continue;
        using namespace Gt7NativeAttachmentPolicy;
        bool invalid = false;
        auto projection_status = Gt7NativeDepthProjection::Status::NotAttempted;
        uint32_t reasons = 0, color_touches = 0;
        bool depth_touched = false;
        if (depth.image_id && (clear_depth || (depth.depth_test_enable && depth.depth_write_enable)) &&
            touches(depth.image_id, depth.desc.info.data, camera)) {
            depth_touched = true;
            bool same_depth_mapping = viewport_depths_match && SameDepthMapping(viewport_min_depth, viewport_max_depth,
                camera.viewport_depth.minimum, camera.viewport_depth.maximum);
            if (!same_depth_mapping && s.native_mode && trusted_depth_vertex && !clear_depth &&
                depth.image_id == camera.depth_id && viewport_depths_match) {
                namespace Projection = Gt7NativeDepthProjection;
                using Status = Projection::Status;
                const auto witnesses_match = [&] {
                    return Gt7FrameGenInputWitness::Matches(camera.DepthWitness(),
                        CurrentInputWitness(textures.GetImage(camera.depth_id), camera.depth_range)) &&
                        Gt7FrameGenInputWitness::Matches(camera.MotionWitness(),
                        CurrentInputWitness(textures.GetImage(camera.motion_id), camera.motion_range));
                };
                const Projection::Attempt attempt {s.native_mode, camera.valid, camera.rejected,
                    camera.native_witness_pending, witnesses_match(), depth.image_id == camera.depth_id,
                    depth.depth_test_enable && depth.depth_write_enable, clear_depth,
                    trusted_depth_vertex != nullptr, viewport_depths_match};
                if (Projection::Eligible(attempt)) {
                    Projection::Matrix actual {}, live {};
                    uint64_t address = 0;
                    projection_status = NativeDepthProjectionSnapshot(*trusted_depth_vertex, actual, address);
                    const bool snapshot_read = projection_status == Status::Ready;
                    if (snapshot_read) {
                        const GuestRange range {address, sizeof(actual)};
                        const auto aliases = [&](ImageId id, GuestRange watched) {
                            return textures.WriteOverlapsImage(range, id, watched);
                        };
                        bool feedback = aliases(camera.depth_id, camera.depth_range) ||
                            aliases(camera.motion_id, camera.motion_range) ||
                            aliases(depth.image_id, depth.desc.info.data);
                        for (const auto& color : colors)
                            feedback |= color.image_id && aliases(color.image_id, color.desc.info.data);
                        auto& buffers = s.context.GetBufferCache();
                        // No wait/readback and no rebasing: the actual uploaded
                        // snapshot must still equal clean CPU backing. A late
                        // matrix write or input invalidation retains the old rejection.
                        if (feedback) projection_status = Status::Feedback;
                        else if (Libs::LibKernel::Memory::TryClampRangeSize(address, sizeof(live)) != sizeof(live))
                            projection_status = Status::Unmapped;
                        else if (!buffers.IsCleanForConcurrentRead(address, sizeof(live))) projection_status = Status::DirtyBefore;
                        else if (!Libs::LibKernel::Memory::TryReadBacking(address, live.data(), sizeof(live))) projection_status = Status::ReadFailed;
                        else if (!Projection::SameBits(actual, live)) projection_status = Status::SnapshotChanged;
                        else if (!buffers.IsCleanForConcurrentRead(address, sizeof(live))) projection_status = Status::DirtyAfter;
                        else if (!witnesses_match()) projection_status = Status::Witness;
                        else {
                            same_depth_mapping = Projection::Equivalent(actual,
                                {viewport_min_depth, viewport_max_depth}, camera.view_projection,
                                {camera.viewport_depth.minimum, camera.viewport_depth.maximum});
                            projection_status = same_depth_mapping ? Status::Accepted : Status::NotEquivalent;
                            if (projection_status == Status::NotEquivalent) {
                                const auto shader = trusted_depth_vertex->program->shader_hash;
                                if (const auto sample = ClaimNativeDepthStructureSample(shader)) {
                                    // Diagnostic-only adjacent structure. Only VP is a proved
                                    // binding; the following view/projection labels are hypotheses.
                                    // These bytes never enter Equivalent or any acceptance state.
                                    std::array<float, 48> candidate {};
                                    const char* result = "ready";
                                    bool complete = false;
                                    constexpr uint64_t bytes = sizeof(candidate);
                                    if (!address || address > std::numeric_limits<uint64_t>::max() - bytes)
                                        result = "range-overflow";
                                    else if (Libs::LibKernel::Memory::TryClampRangeSize(address, bytes) != bytes)
                                        result = "unmapped";
                                    else if (!buffers.IsCleanForConcurrentRead(address, bytes))
                                        result = "dirty-before";
                                    else {
                                        const GuestRange structure_range {address, bytes};
                                        const auto structure_alias = [&](ImageId id, GuestRange watched) {
                                            return textures.WriteOverlapsImage(structure_range, id, watched);
                                        };
                                        bool feedback = structure_alias(camera.depth_id, camera.depth_range) ||
                                            structure_alias(camera.motion_id, camera.motion_range) ||
                                            structure_alias(depth.image_id, depth.desc.info.data);
                                        for (const auto& color : colors)
                                            feedback |= color.image_id && structure_alias(color.image_id, color.desc.info.data);
                                        if (feedback) result = "attachment-alias";
                                        else if (!Libs::LibKernel::Memory::TryReadBacking(address, candidate.data(), bytes))
                                            result = "read-failed";
                                        else if (!buffers.IsCleanForConcurrentRead(address, bytes))
                                            result = "dirty-after";
                                        else if (!std::equal(actual.begin(), actual.end(), candidate.begin(),
                                            [](float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }))
                                            result = "bound-vp-changed";
                                        else if (!witnesses_match()) result = "input-witness-changed";
                                        else complete = true;
                                    }
                                    ReportNativeDepthStructure(sample, shader, s.fg_frame, s.fg_scene_serial, address,
                                        result, actual, {viewport_min_depth, viewport_max_depth}, camera,
                                        complete ? std::span<const float>(candidate) : std::span<const float>{});
                                }
                            }
                        }
                    }
                    ReportNativeDepthProjection(trusted_depth_vertex->program->shader_hash, s.fg_frame, s.fg_scene_serial,
                        projection_status, address, snapshot_read, actual, {viewport_min_depth, viewport_max_depth}, camera);
                } else if (!attempt.witness_matches || attempt.witness_pending) {
                    projection_status = Status::Witness;
                }
            }
            invalid = !Allows({s.native_mode, verified_camera, true,
                depth.image_id == camera.depth_id, Role::Depth, Role::Depth,
                !clear_depth ? Clear::None : clear_value == 0.0f ? Clear::ZeroDepth : Clear::Other,
                same_depth_mapping});
            reasons |= ((!s.native_mode || clear_depth) && !verified_camera ? 1u : 0u) |
                (depth.image_id != camera.depth_id ? 2u : 0u) |
                (clear_depth && clear_value != 0.0f ? 4u : 0u) |
                (s.native_mode && !same_depth_mapping ? 64u : 0u);
        }
        // Draw discovery includes only actual MRT exports with a nonzero write
        // mask here; read-only depth attachments are excluded by the test above.
        for (const auto& color : colors) {
            if (!color.image_id || !touches(color.image_id, color.desc.info.data, camera)) continue;
            invalid |= !Allows({s.native_mode, verified_camera, true,
                color.image_id == camera.motion_id, &color == motion ? Role::Motion : Role::Other,
                Role::Motion, Clear::None});
            color_touches |= 1u << (&color - colors.data());
            reasons |= (!s.native_mode && !verified_camera ? 8u : 0u) | (&color != motion ? 16u : 0u) |
                (color.image_id != camera.motion_id ? 32u : 0u);
        }
        if (invalid) {
            // Capture only the first candidate invalidated by this draw. Do not
            // repeat cache overlap queries or alter any acceptance expression.
            if (diagnostic.reason_mask == 0) {
                diagnostic.reason_mask = reasons;
                diagnostic.color_touch_mask = color_touches;
                diagnostic.frame = s.fg_frame;
                diagnostic.scene = s.fg_scene_serial;
                diagnostic.depth_id = uint64_t(camera.depth_id.index) | (uint64_t(camera.depth_id.generation) << 32);
                diagnostic.motion_id = uint64_t(camera.motion_id.index) | (uint64_t(camera.motion_id.generation) << 32);
                diagnostic.depth_address = camera.depth_range.address;
                diagnostic.depth_size = camera.depth_range.size;
                diagnostic.motion_address = camera.motion_range.address;
                diagnostic.motion_size = camera.motion_range.size;
                diagnostic.saved_viewport_min_depth = camera.viewport_depth.minimum;
                diagnostic.saved_viewport_max_depth = camera.viewport_depth.maximum;
                diagnostic.depth_projection_status = Gt7NativeDepthProjection::Name(projection_status);
                const auto relation = [&](ImageId id, GuestRange range) {
                    const uint32_t bits = (id == camera.depth_id ? 1u : 0u) | (id == camera.motion_id ? 2u : 0u) |
                        (Overlap(range, camera.depth_range) ? 4u : 0u) | (Overlap(range, camera.motion_range) ? 8u : 0u);
                    return bits != 0 ? bits : 16u;
                };
                if (depth_touched) diagnostic.depth_touch_relation = relation(depth.image_id, depth.desc.info.data);
                for (size_t i = 0; i < colors.size() && i < std::size(diagnostic.color_touch_relation); ++i)
                    if (color_touches & (1u << i))
                        diagnostic.color_touch_relation[i] = relation(colors[i].image_id, colors[i].desc.info.data);
            }
            camera.valid = false;
            camera.rejected = true;
            camera.reject_reason = (reasons & 64u) != 0 ? "viewport depth mapping changed for native camera input write"
                : "unverified draw or clear writes native camera inputs";
        }
    }
    return diagnostic;
}

void Gt7Dlss::NoteFrameGenerationSceneTarget(const RenderColorInfo& target) {
    auto& s = *m_impl;
    if (Gt7FrameGen::InputsRequested(s.graphics))
        s.fg_scaled_scene_id = target.image_id;
}

bool Gt7Dlss::FrameGenerationTonemapMatches(const ShaderVertexInputInfo& vertex,
                                           const PreparedBindings& pixel,
                                           const RenderColorInfo& target) {
    auto& s = *m_impl;
    if (!Gt7FrameGen::InputsRequested(s.graphics) ||
        !s.fg_scaled_scene_id || !s.SceneReady() ||
        !pixel.runtime || !*pixel.runtime || !vertex.stage ||
        pixel.runtime->program->shader_hash != 0xedb9cdad0985bf44ULL ||
        vertex.stage.program->shader_hash != 0xf13bd2584bf580c0ULL) {
        ++s.fg_hudless_diagnostic.tonemap_calls;
        s.fg_hudless_diagnostic.Note("tone-map precondition rejected",
            !s.fg_scaled_scene_id ? "no scaled scene target" :
            !s.SceneReady() ? "no single completed temporal scene" :
            "runtime, shader identity or FG configuration unavailable");
        return false;
    }
    ++s.fg_hudless_diagnostic.tonemap_calls;
    // Exactly one final tone-map per scene. Never replace a prior snapshot after
    // UI has begun, even if a second superficially similar pass is encountered.
    ++s.fg_tonemap_attempts;
    s.fg_hudless_valid = false;
    const auto reject = [&](const char* reason) {
        s.fg_hudless_diagnostic.Note("tone-map contract rejected", reason);
        if (++s.fg_tonemap_rejections <= 8 || s.fg_tonemap_rejections % 300 == 0)
            LOGF("GT7 FSR FG: HUD-less tone-map rejected #%llu frame=%llu: %s\n",
                 static_cast<unsigned long long>(s.fg_tonemap_rejections),
                 static_cast<unsigned long long>(s.fg_frame), reason);
        return false;
    };
    if (s.fg_tonemap_attempts != 1 || pixel.images.size() != 5 ||
        pixel.buffer_sources.size() != 1 || !pixel.bindless_heaps.empty() ||
        !pixel.bindless_patches.empty() || !pixel.runtime->resources->gpu_fills.empty() ||
        pixel.runtime->program->info.uses_dma || !target.image_id || target.target_slot != 0 ||
        target.guest_mip_level || target.guest_array_layer || !BaseView(target.desc) ||
        target.desc.view_info.format != vk::Format::eA2B10G10R10UnormPack32 ||
        !target.export_mapping.IsIdentity()) return reject("tone-map count, resource cardinality or target view");
    for (const auto& resource : pixel.runtime->program->info.images)
        if (resource.written || resource.bindless || resource.runtime) return reject("unbounded tone-map image resource");
    for (const auto& resource : pixel.runtime->program->info.buffers)
        if (resource.written) return reject("tone-map writes buffer storage");
    const auto& scene = pixel.images[0];
    if (scene.image_id != s.fg_scaled_scene_id || !BaseView(scene.desc) ||
        scene.desc.view_info.format != vk::Format::eB10G11R11UfloatPack32)
        return reject("tone-map source is not the current SR composite");
    const std::array<vk::Format, 5> formats {
        vk::Format::eB10G11R11UfloatPack32, vk::Format::eB10G11R11UfloatPack32,
        vk::Format::eR8Unorm, vk::Format::eR16G16B16A16Sfloat, vk::Format::eR16G16B16A16Sfloat};
    auto& textures = s.context.GetTextureCache();
    const auto& output = textures.GetImage(target.image_id);
    const auto& scene_image = textures.GetImage(scene.image_id);
    if (s.native_mode && (scene_image.backing.image != s.native_output_image ||
        scene_image.content_serial != s.native_output_serial ||
        scene_image.ExternalWriteVersion() != s.native_output_write_version))
        return reject("native SR output changed before tone-map");
    if (!Shape(output, GuestWidth, GuestHeight, vk::Format::eA2B10G10R10UnormPack32) ||
        !Shape(textures.GetImage(scene.image_id), GuestWidth, GuestHeight,
               vk::Format::eB10G11R11UfloatPack32)) return reject("tone-map backing shape or format");
    namespace Tone = Gt7FrameGenTonemapInputs;
    const auto numeric_class = [](Prospero::TextureNumericClass numeric) {
        switch (numeric) {
            case Prospero::TextureNumericClass::Float: return Tone::NumericClass::Float;
            case Prospero::TextureNumericClass::Uint: return Tone::NumericClass::Uint;
            case Prospero::TextureNumericClass::Sint: return Tone::NumericClass::Sint;
            default: return Tone::NumericClass::Unsupported;
        }
    };
    for (size_t i = 0; i < pixel.images.size(); ++i) {
        const auto& texture = pixel.images[i];
        const auto& view = texture.desc.view_info;
        const auto& extent = texture.desc.info.extent;
        Tone::TextureFacts facts {
            .image = bool(texture.image_id), .view = bool(texture.image_view),
            .sampled_binding = texture.desc.type == TextureCache::BindingType::Texture,
            .extent = {extent.width, extent.height, extent.depth},
            .samples = texture.desc.info.samples, .format = uint32_t(view.format), .expected_format = uint32_t(formats[i]),
            .base_level = view.base_level, .level_count = view.level_count,
            .base_layer = view.base_layer, .layer_count = view.layer_count,
            .view_type = view.type == vk::ImageViewType::e2D ? Tone::Dimension::Two :
                view.type == vk::ImageViewType::e3D ? Tone::Dimension::Three : Tone::Dimension::Other,
            .range_valid = texture.desc.info.data.Valid()};
        // PreparedBindings and RebindImages preserve program.info.images[i].
        // Validate that actual sampled resource, rather than a captured aux format.
        if (i < pixel.runtime->program->info.images.size()) {
            const auto& resource = pixel.runtime->program->info.images[i];
            facts.resource = {
                .present = true,
                .sampled = resource.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled,
                .read = resource.read, .written = resource.written,
                .atomic = resource.atomic, .atomic64 = resource.atomic64,
                .depth_compare = resource.depth_compare, .bindless = resource.bindless,
                .runtime = resource.runtime, .numeric = numeric_class(resource.numeric_class)};
        }
        facts.descriptor_numeric = numeric_class(Prospero::SampledTextureNumericClass(texture.desc.info.guest_format));
        facts.view_format_valid = view.format != vk::Format::eUndefined;
        facts.color_aspect = view.aspect == vk::ImageAspectFlagBits::eColor;
        const Image* texture_image = nullptr;
        const auto backing_facts = [&] {
            if (texture_image == nullptr && texture.image_id) texture_image = &textures.GetImage(texture.image_id);
            if (texture_image == nullptr) return;
            const auto& backing = texture_image->backing;
            facts.backing_image = bool(backing.image);
            facts.backing_sampled = bool(backing.usage & vk::ImageUsageFlagBits::eSampled);
            facts.backing_extent = {backing.extent.width, backing.extent.height, backing.extent.depth};
            facts.backing_format = uint32_t(backing.format);
            facts.backing_format_compatible = ImageViewOps::FormatsCompatible(backing.format, view.format);
            facts.backing_samples = backing.samples;
            facts.backing_levels = backing.mip_levels;
            facts.backing_layers = backing.layers;
            facts.backing_type = backing.image_type == vk::ImageType::e2D ? Tone::Dimension::Two :
                backing.image_type == vk::ImageType::e3D ? Tone::Dimension::Three : Tone::Dimension::Other;
        };
        auto failure = Tone::CheckDescriptor(s.native_mode, i, facts);
        if (failure == Tone::Failure::None) {
            // Preserve the original feedback gate before accepting a native
            // auxiliary's resolved backing. These are cached metadata only.
            facts.output_feedback = texture.image_id == target.image_id;
            if (!facts.output_feedback) {
                backing_facts();
                facts.output_feedback = texture_image->backing.image == output.backing.image ||
                    Overlap(texture.desc.info.data, target.desc.info.data);
            }
            failure = Tone::CheckBacking(s.native_mode, i, facts);
        }
        if (failure != Tone::Failure::None) {
            using TextureKey = std::array<uint32_t, 3>;
            static thread_local std::array<TextureKey, 32> seen {};
            static thread_local size_t seen_count = 0;
            static thread_local int64_t next_report = 0;
            const TextureKey key {uint32_t(s.native_mode), uint32_t(i), uint32_t(failure)};
            const bool known = std::find(seen.begin(), seen.begin() + seen_count, key) != seen.begin() + seen_count;
            const bool first = !known && seen_count < seen.size();
            if (first) seen[seen_count++] = key;
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (first || now >= next_report) {
                next_report = now + 5000;
                backing_facts();
                const auto expected = Tone::LegacyExtents[i];
                const bool diagnostic_feedback = texture.image_id == target.image_id ||
                    (texture_image && texture_image->backing.image == output.backing.image) ||
                    Overlap(texture.desc.info.data, target.desc.info.data);
                std::printf("GT7 FG tone-map texture rejected: frame=%llu scene=%llu native=%u index=%zu field=%s"
                    " id=%u:%u view=%u binding=%u extent=%ux%ux%u samples=%u format=%u type=%u mip=%u+%u layer=%u+%u"
                    " range=0x%llx+0x%llx range_valid=%u output=%u:%u feedback=%u"
                    " backing={present=%u extent=%ux%ux%u samples=%u format=%u format_compatible=%u type=%u levels=%u layers=%u sampled=%u}"
                    " expected={extent_policy=%s legacy_extent=%ux%ux%u format_policy=%s legacy_format=%u type=%u samples=1 mip=0+1 layer=0+1}\n",
                    static_cast<unsigned long long>(s.fg_frame), static_cast<unsigned long long>(s.fg_scene_serial),
                    unsigned(s.native_mode), i, Tone::Name(failure), texture.image_id.index, texture.image_id.generation,
                    unsigned(facts.view), unsigned(texture.desc.type), extent.width, extent.height, extent.depth,
                    facts.samples, facts.format, unsigned(view.type), view.base_level, view.level_count, view.base_layer, view.layer_count,
                    static_cast<unsigned long long>(texture.desc.info.data.address),
                    static_cast<unsigned long long>(texture.desc.info.data.size), unsigned(facts.range_valid),
                    target.image_id.index, target.image_id.generation, unsigned(diagnostic_feedback),
                    unsigned(facts.backing_image), facts.backing_extent.width, facts.backing_extent.height, facts.backing_extent.depth,
                    facts.backing_samples, facts.backing_format, unsigned(facts.backing_format_compatible),
                    unsigned(texture_image ? texture_image->backing.image_type : vk::ImageType::e2D),
                    facts.backing_levels, facts.backing_layers, unsigned(facts.backing_sampled),
                    s.native_mode && i != 0 ? "nonzero-matching-backing" : "fixed", expected.width, expected.height, expected.depth,
                    s.native_mode && i != 0 ? "resolved-sampled-float" : "fixed",
                    unsigned(formats[i]), unsigned(i == 3 ? vk::ImageViewType::e3D : vk::ImageViewType::e2D));
                // One bounded report includes every actual binding, so the first
                // failed slot does not hide later optional inputs. No readback.
                for (size_t j = 0; j < pixel.images.size(); ++j) {
                    const auto& binding = pixel.images[j];
                    const auto& desc = binding.desc;
                    const auto& slot_view = desc.view_info;
                    const auto* resource = j < pixel.runtime->program->info.images.size() ?
                        &pixel.runtime->program->info.images[j] : nullptr;
                    const auto* image = j == i ? texture_image :
                        binding.image_id ? &textures.GetImage(binding.image_id) : nullptr;
                    std::printf("GT7 FG tone-map texture context: frame=%llu scene=%llu index=%zu failed=%u"
                        " resource={present=%u source=%u class=%u numeric=%u dimension=%u read=%u written=%u atomic=%u atomic64=%u depth_compare=%u bindless=%u runtime=%u}"
                        " id=%u:%u view=%u binding=%u guest_format=%u guest_numeric=%u extent=%ux%ux%u format=%u aspect=0x%x type=%u samples=%u mip=%u+%u layer=%u+%u range=0x%llx+0x%llx"
                        " backing={present=%u extent=%ux%ux%u format=%u compatible=%u type=%u samples=%u levels=%u layers=%u sampled=%u} feedback=%u\n",
                        static_cast<unsigned long long>(s.fg_frame), static_cast<unsigned long long>(s.fg_scene_serial), j, unsigned(j == i),
                        unsigned(resource != nullptr), resource ? resource->source : 0u,
                        resource ? unsigned(resource->resource_class) : 0u, resource ? unsigned(resource->numeric_class) : 0u,
                        resource ? unsigned(resource->dimension) : 0u, resource ? unsigned(resource->read) : 0u,
                        resource ? unsigned(resource->written) : 0u, resource ? unsigned(resource->atomic) : 0u,
                        resource ? unsigned(resource->atomic64) : 0u, resource ? unsigned(resource->depth_compare) : 0u,
                        resource ? unsigned(resource->bindless) : 0u, resource ? unsigned(resource->runtime) : 0u,
                        binding.image_id.index, binding.image_id.generation, unsigned(bool(binding.image_view)), unsigned(desc.type),
                        unsigned(desc.info.guest_format), unsigned(Prospero::SampledTextureNumericClass(desc.info.guest_format)),
                        desc.info.extent.width, desc.info.extent.height, desc.info.extent.depth,
                        unsigned(slot_view.format), static_cast<vk::ImageAspectFlags::MaskType>(slot_view.aspect), unsigned(slot_view.type),
                        desc.info.samples, slot_view.base_level, slot_view.level_count, slot_view.base_layer, slot_view.layer_count,
                        static_cast<unsigned long long>(desc.info.data.address), static_cast<unsigned long long>(desc.info.data.size),
                        unsigned(image && image->backing.image), image ? image->backing.extent.width : 0u,
                        image ? image->backing.extent.height : 0u, image ? image->backing.extent.depth : 0u,
                        image ? unsigned(image->backing.format) : 0u,
                        unsigned(image && ImageViewOps::FormatsCompatible(image->backing.format, slot_view.format)),
                        image ? unsigned(image->backing.image_type) : 0u, image ? image->backing.samples : 0u,
                        image ? image->backing.mip_levels : 0u, image ? image->backing.layers : 0u,
                        unsigned(image && bool(image->backing.usage & vk::ImageUsageFlagBits::eSampled)),
                        unsigned(binding.image_id == target.image_id || (image && image->backing.image == output.backing.image) ||
                            Overlap(desc.info.data, target.desc.info.data)));
                }
                std::fflush(stdout);
            }
            return reject("tone-map texture shape, view or feedback");
        }
    }
    // The captured f13b fullscreen fan has a 28-byte XYZ/UV vertex and an
    // identity vertex matrix. Validate it so a crop/zoom cannot masquerade as
    // the complete display-color image merely by sharing shader hashes.
    if (vertex.logical_stage != ShaderType::Vertex || vertex.prepare_failed ||
        !vertex.fetch_embedded || vertex.fetch_external || vertex.clip_space.enabled ||
        vertex.buffers_num != 1 || vertex.resources_num != 2 ||
        vertex.buffers[0].stride != 28 || vertex.buffers[0].num_records != 4 ||
        vertex.stage.resources->buffers.size() != 1 ||
        !vertex.stage.resources->gpu_fills.empty()) return reject("tone-map fullscreen vertex bindings");
    const auto address = vertex.buffers[0].addr;
    for (size_t i = 0; i < 2; ++i) {
        const auto& resource = vertex.resources[i];
        if (resource.Base48() != address + i * 12 || resource.Stride() != 28 ||
            resource.NumRecords() != 4 || resource.RawFormat() != (i == 0 ? 74u : 77u) ||
            resource.AddTid() || resource.SwizzleEnabled()) return reject("tone-map vertex attribute descriptor");
    }
    const auto matrix = DecodeNativeDescriptor<ShaderBufferResource>(vertex.stage.resources->buffers[0]);
    if (matrix.Stride() != 16 || matrix.NumRecords() != 4 || matrix.RawFormat() != 77 ||
        matrix.DstSelXYZW() != 0xfac || matrix.AddTid() || matrix.SwizzleEnabled())
        return reject("tone-map vertex matrix descriptor");
    constexpr std::array<float, 28> quad {
        -1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0,
        1, -1, 0, 1, 1, 0, 0, -1, -1, 0, 0, 1, 0, 0};
    constexpr std::array<float, 16> identity {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const auto equal_bytes = [&]<size_t N>(uint64_t base, const std::array<float, N>& expected) {
        if (!base || base > std::numeric_limits<uint64_t>::max() - sizeof(expected) ||
            Libs::LibKernel::Memory::TryClampRangeSize(base, sizeof(expected)) != sizeof(expected) ||
            !s.context.GetBufferCache().IsCleanForConcurrentRead(base, sizeof(expected))) return false;
        std::array<float, N> actual {};
        return Libs::LibKernel::Memory::TryReadBacking(base, actual.data(), sizeof(actual)) && actual == expected;
    };
    if (!equal_bytes(address, quad) || !equal_bytes(matrix.Base48(), identity))
        return reject("tone-map fullscreen quad or identity matrix changed");
    s.fg_hudless_diagnostic.Note("tone-map verified", "waiting for HUD-less copy");
    return true;
}

void Gt7Dlss::CaptureFrameGenerationHudless(CommandBuffer& buffer, const RenderColorInfo& target) {
    auto& s = *m_impl;
    ++s.fg_hudless_diagnostic.capture_attempts;
    s.fg_hudless_valid = false;
    if (s.fg_tonemap_attempts != 1 || !target.image_id) {
        s.fg_hudless_diagnostic.Note("HUD-less copy rejected", "tone-map count or target identity");
        return;
    }
    auto& source = s.context.GetTextureCache().GetImage(target.image_id);
    if (!(source.backing.usage & vk::ImageUsageFlagBits::eTransferSrc)) {
        s.fg_hudless_diagnostic.Note("HUD-less copy rejected", "source lacks transfer usage");
        return;
    }
    if (source.backing.state.layout == vk::ImageLayout::eUndefined) {
        s.fg_hudless_diagnostic.Note("HUD-less copy rejected", "source layout undefined");
        return;
    }
    if (!ConfigureFrameGenerationImage(s.graphics, s.fg_hudless,
            vk::Format::eA2B10G10R10UnormPack32, GuestWidth, GuestHeight)) {
        s.fg_hudless_diagnostic.Note("HUD-less copy rejected", "private image allocation unavailable");
        return;
    }
    s.scheduler.EndRendering();
    const auto command = buffer.Handle();
    const auto saved = source.backing.state;
    Transit(source, vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, command);
    TransitFrameGenerationImage(command, s.fg_hudless, vk::ImageLayout::eTransferDstOptimal,
                                 vk::AccessFlagBits2::eTransferWrite);
    const vk::ImageSubresourceLayers layer {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    const vk::ImageCopy region {layer, {}, layer, {}, {GuestWidth, GuestHeight, 1}};
    command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal,
                      s.fg_hudless.image, vk::ImageLayout::eTransferDstOptimal, 1, &region);
    Transit(source, saved.layout, saved.access_mask, command);
    TransitFrameGenerationImage(command, s.fg_hudless, SampledLayout, ShaderRead);
    s.fg_output_id = target.image_id;
    s.fg_output_image = source.backing.image;
    s.fg_output_range = source.info.data;
    s.fg_hudless_valid = true;
    s.fg_hudless_diagnostic.captured = true;
    s.fg_hudless_diagnostic.Note("HUD-less captured", "none");
    s.fg_scene_closed = true;
    const auto reserved = s.fg_completed_state.Reserve(OutputIdentity(source), s.fg_scene_serial);
    if (reserved) {
        const auto slot = *reserved;
        auto& completed = s.fg_completed[slot];
        auto& frozen = completed.inputs;
        completed.external_write_version = source.ExternalWriteVersion();
        frozen.valid = frozen.camera_valid = frozen.camera_pose_valid = frozen.hudless_valid = false;
        const bool scene_ok = s.SceneReady();
        completed.rejection = !scene_ok ? Gt7FrameGen::StatusReason::SceneUnverified :
            !s.fg_camera_valid ? Gt7FrameGen::StatusReason::CameraUnverified :
            Gt7FrameGen::StatusReason::InputUnavailable;
        const bool readable = scene_ok && s.fg_camera_valid && s.depth && s.motion &&
            s.depth->backing.state.layout != vk::ImageLayout::eUndefined &&
            s.motion->backing.state.layout != vk::ImageLayout::eUndefined &&
            bool(s.depth->backing.usage & vk::ImageUsageFlagBits::eTransferSrc) &&
            bool(s.motion->backing.usage & vk::ImageUsageFlagBits::eTransferSrc);
        const bool allocated = readable &&
            ConfigureCompletedImage(s.graphics, frozen.depth, vk::Format::eR32Sfloat, InputWidth, InputHeight) &&
            ConfigureCompletedImage(s.graphics, frozen.motion, vk::Format::eR32G32Sfloat, InputWidth, InputHeight) &&
            ConfigureCompletedImage(s.graphics, frozen.hudless_color, vk::Format::eA2B10G10R10UnormPack32, GuestWidth, GuestHeight);
        if (allocated) {
            const std::array<Image*, 2> temporal {s.depth.get(), s.motion.get()};
            const std::array<VulkanImage*, 2> destinations {&frozen.depth, &frozen.motion};
            const vk::ImageCopy temporal_region {layer, {}, layer, {}, {InputWidth, InputHeight, 1}};
            for (size_t i = 0; i < temporal.size(); ++i) {
                auto& input = *temporal[i];
                auto& destination = *destinations[i];
                const auto previous = input.backing.state;
                Transit(input, vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, command);
                TransitFrameGenerationImage(command, destination, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
                command.copyImage(input.backing.image, vk::ImageLayout::eTransferSrcOptimal,
                    destination.image, vk::ImageLayout::eTransferDstOptimal, 1, &temporal_region);
                TransitFrameGenerationImage(command, destination, SampledLayout, ShaderRead);
                Transit(input, previous.layout, previous.access_mask, command);
            }
            TransitFrameGenerationImage(command, s.fg_hudless, vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead);
            TransitFrameGenerationImage(command, frozen.hudless_color, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
            command.copyImage(s.fg_hudless.image, vk::ImageLayout::eTransferSrcOptimal,
                frozen.hudless_color.image, vk::ImageLayout::eTransferDstOptimal, 1, &region);
            TransitFrameGenerationImage(command, s.fg_hudless, SampledLayout, ShaderRead);
            TransitFrameGenerationImage(command, frozen.hudless_color, SampledLayout, ShaderRead);
            frozen.render_extent = vk::Extent2D {InputWidth, InputHeight};
            frozen.guest_frame = s.fg_scene_serial;
            frozen.jitter_x = s.fg_jitter_x; frozen.jitter_y = s.fg_jitter_y;
            frozen.frame_time_ms = s.fg_frame_time_ms;
            frozen.near_plane = s.fg_camera.near_plane; frozen.far_plane = s.fg_camera.far_plane;
            frozen.fov_y = s.fg_camera.fov_y;
            std::copy(s.fg_camera.position.begin(), s.fg_camera.position.end(), frozen.camera_position);
            std::copy(s.fg_camera.right.begin(), s.fg_camera.right.end(), frozen.camera_right);
            std::copy(s.fg_camera.up.begin(), s.fg_camera.up.end(), frozen.camera_up);
            std::copy(s.fg_camera.forward.begin(), s.fg_camera.forward.end(), frozen.camera_forward);
            frozen.depth_infinite = false;
            frozen.reset = s.fg_reset;
            frozen.valid = frozen.camera_valid = frozen.camera_pose_valid = frozen.hudless_valid = true;
            completed.rejection = Gt7FrameGen::StatusReason::WaitingInputs;
            s.fg_completed_state.Commit(slot, true);
            if (s.native_mode) {
                s.native_reset_pending = false;
                s.native_pending_sources = 0;
            }
            if (Report(++s.fg_completed_captures))
                LOGF("GT7 FG completed scene: captured=%llu scene=%llu output=%u:%u slot=%zu reset=%u\n",
                    static_cast<unsigned long long>(s.fg_completed_captures),
                    static_cast<unsigned long long>(s.fg_scene_serial), source.self_id.index,
                    source.self_id.generation, slot, frozen.reset ? 1u : 0u);
        } else {
            s.fg_completed_state.Commit(slot, false);
        }
    }
    if (Report(++s.fg_hudless_captures))
        LOGF("GT7 FSR FG: HUD-less capture #%llu frame=%llu target=%u:%u 3840x2160 format=%u after guest tone-map before UI\n",
             static_cast<unsigned long long>(s.fg_hudless_captures), static_cast<unsigned long long>(s.fg_frame),
             s.fg_output_id.index, s.fg_output_id.generation, unsigned(source.backing.format));
}

bool Gt7Dlss::WatchesFrameGenerationOutput() const {
    return m_impl->WatchesNativeSource() || m_impl->fg_hudless_valid || m_impl->HasCompletedOutputs();
}

bool Gt7Dlss::PreservesFrameGenerationMetadata(const ShaderComputeInputInfo& input,
    const PreparedBindings& bindings, uint32_t group_x, uint32_t group_y,
    uint32_t group_z, uint32_t mode) {
    auto& s = *m_impl;
    if (s.active || s.WatchesNativeSource() || !WatchesFrameGenerationOutput()) return false;
    const auto& program = *input.stage.program;
    const auto& resources = *input.stage.resources;
    if (program.shader_hash != Gt7FrameGenDcc::CopyShader) return false;
    const auto reject = [&](size_t reason, const char* text) {
        static std::array<uint64_t, 8> counts {};
        if (++counts[reason] <= 4)
            LOGF("GT7 FSR FG: DCC copy proof rejected frame=%llu: %s\n",
                 static_cast<unsigned long long>(s.fg_frame), text);
        return false;
    };
    if (program.has_address_writes ||
        input.workgroup_register != 9 || !input.group_id[0] || !input.group_id[1] ||
        input.thread_ids_num < 2 || input.dispatch_dimensions_indirect ||
        program.info.uses_dma || !program.info.images.empty() || program.info.buffers.size() != 2 ||
        resources.buffers.size() != 2 || !resources.images.empty() || !resources.gpu_fills.empty() ||
        program.user_data_base != 0 || resources.user_data.size() != 9 ||
        bindings.buffer_sources.size() != 2 || !bindings.images.empty() ||
        program.bindings.descriptors.empty() ||
        program.bindings.descriptors.front().resources.size() != 2 ||
        program.bindings.descriptors.front().resources[0] != 0 ||
        program.bindings.descriptors.front().resources[1] != 1 ||
        !program.info.buffers[0].read || program.info.buffers[0].written ||
        program.info.buffers[1].read || !program.info.buffers[1].written ||
        !program.info.buffers[0].formatted || !program.info.buffers[1].formatted ||
        program.info.buffers[0].atomic || program.info.buffers[1].atomic ||
        resources.buffers[0].dword_count != 4 || resources.buffers[1].dword_count != 4)
        return reject(0, "shader runtime/binding shape");
    Gt7FrameGenDcc::CopyContract copy;
    copy.shader = program.shader_hash;
    std::copy_n(resources.buffers[0].dwords.begin(), 4, copy.source.begin());
    std::copy_n(resources.buffers[1].dwords.begin(), 4, copy.destination.begin());
    if (!std::equal(copy.source.begin(), copy.source.end(), resources.user_data.begin() + 1) ||
        !std::equal(copy.destination.begin(), copy.destination.end(), resources.user_data.begin() + 5))
        return reject(1, "descriptor/user-data mismatch");
    copy.groups = {group_x, group_y, group_z};
    std::copy_n(input.threads_num, 3, copy.threads.begin());
    copy.mode = mode;
    copy.pitch = resources.user_data[0];
    copy.thread_dimensions = input.dispatch_thread_dimensions;
    if (!Gt7FrameGenDcc::Matches(copy)) return reject(2, "unsupported copy contract");
    const auto& source = bindings.buffer_sources[0];
    const auto& destination = bindings.buffer_sources[1];
    if (source.address != Gt7FrameGenDcc::Address(copy.source) ||
        destination.address != Gt7FrameGenDcc::Address(copy.destination) ||
        source.size != Gt7FrameGenDcc::Bytes || destination.size != Gt7FrameGenDcc::Bytes ||
        !source.id || !destination.id) return reject(3, "resolved buffer range mismatch");
    auto& textures = s.context.GetTextureCache();
    const GuestRange destination_range {destination.address, destination.size};
    const GuestRange source_range {source.address, source.size};
    bool watched = false;
    const auto prove_output = [&](const Gt7FrameGenAssociation::OutputIdentity& identity) {
        const auto id = OutputId(identity);
        const GuestRange range {identity.address, identity.size};
        if (!textures.WriteOverlapsImage(destination_range, id, range)) return true;
        watched = true;
        // These safe queries reject stale/deleted IDs before GetImage. The
        // caller runs synchronously on the texture-cache owning GPU lane.
        // RebindBuffers may synchronize an aliased source: prove both sides.
        if (!textures.WriteIsOnlyDccMetadata(destination_range, id, range) ||
            !textures.WriteIsOnlyDccMetadata(source_range, id, range)) return false;
        const auto& output = textures.GetImage(id);
        return OutputIdentity(output) == identity && !output.IsBufferModified() &&
            !output.IsCpuDirty() &&
            Shape(output, GuestWidth, GuestHeight, vk::Format::eA2B10G10R10UnormPack32);
    };
    for (size_t i = 0; i < s.fg_completed.size(); ++i) {
        const auto& candidate = s.fg_completed_state.Get(i);
        if (candidate.ready && !candidate.consumed && !prove_output(candidate.output))
            return reject(4, "copy is not isolated expanded DCC for every affected completed output");
    }
    if (s.fg_hudless_valid) {
        Gt7FrameGenAssociation::OutputIdentity live {};
        live.image_id = uint64_t(s.fg_output_id.index) | (uint64_t(s.fg_output_id.generation) << 32);
        live.backing = std::bit_cast<uint64_t>(static_cast<VkImage>(s.fg_output_image));
        live.address = s.fg_output_range.address;
        live.size = s.fg_output_range.size;
        live.width = GuestWidth; live.height = GuestHeight;
        live.format = static_cast<uint32_t>(vk::Format::eA2B10G10R10UnormPack32);
        if (!prove_output(live)) return reject(5, "captured display image no longer current");
    }
    if (!watched) return false;
    auto& buffers = s.context.GetBufferCache();
    const auto clean = [&] {
        return !buffers.AddressWritesInFlight() &&
            !buffers.HasGpuDirtyBytes(source.address, source.size) &&
            buffers.IsCleanForConcurrentRead(source.address, source.size) &&
            !buffers.IsRegionCpuModified(source.address, source.size);
    };
    if (!clean() || Libs::LibKernel::Memory::TryClampRangeSize(source.address, source.size) != source.size)
        return reject(6, "source is not fully mapped and CPU/GPU clean");
    // Validate the complete current source, never a hash/sample or a GPU readback.
    // As with camera constants, guest submission synchronization owns its lifetime.
    s.fg_dcc_snapshot.resize(Gt7FrameGenDcc::Bytes);
    auto& bytes = s.fg_dcc_snapshot;
    if (!Libs::LibKernel::Memory::TryReadBacking(source.address, bytes.data(), bytes.size()) ||
        !Gt7FrameGenDcc::FullyExpanded(bytes) || !clean())
        return reject(7, "source is not proven entirely expanded");
    static uint64_t preserved = 0;
    if (Report(++preserved))
        LOGF("GT7 FSR FG: expanded DCC copy preserved #%llu frame=%llu source=0x%012llx destination=0x%012llx bytes=0x%x\n",
            static_cast<unsigned long long>(preserved), static_cast<unsigned long long>(s.fg_frame),
            static_cast<unsigned long long>(source.address), static_cast<unsigned long long>(destination.address),
            Gt7FrameGenDcc::Bytes);
    return true;
}

bool Gt7Dlss::MatchesFrameGenerationOutput(const RenderColorInfo& target) const {
    const auto& s = *m_impl;
    if (s.WatchesNativeSource() && ((target.image_id && target.image_id == s.fg_scaled_scene_id) ||
                          Overlap(target.desc.info.data, s.native_output_range))) return true;
    if (s.fg_hudless_valid && ((target.image_id && target.image_id == s.fg_output_id) ||
                             Overlap(target.desc.info.data, s.fg_output_range))) return true;
    for (size_t i = 0; i < s.fg_completed.size(); ++i) {
        const auto& candidate = s.fg_completed_state.Get(i);
        if (candidate.ready && !candidate.consumed &&
            ((target.image_id && target.image_id == OutputId(candidate.output)) ||
             Overlap(target.desc.info.data, {candidate.output.address, candidate.output.size}))) return true;
    }
    return false;
}

void Gt7Dlss::CheckFrameGenerationOutputDraw(std::span<const RenderColorInfo> targets,
                                            const PreparedBindings* pixel, uint64_t vs,
                                            uint64_t ps, bool ordinary_alpha_overlay,
                                            bool proven_opaque_overlay, OutputDrawDiagnostic* diagnostic) {
    if (diagnostic != nullptr) *diagnostic = {};
    auto& s = *m_impl;
    if (s.WatchesNativeSource()) {
        for (const auto& target : targets) {
            const bool exact_id = target.image_id && target.image_id == s.fg_scaled_scene_id;
            const bool cache_overlap = !exact_id && s.context.GetTextureCache().WriteOverlapsImage(
                target.desc.info.data, s.fg_scaled_scene_id, s.native_output_range);
            if (exact_id || cache_overlap) {
                // Same short-circuit gate and same cache query as before. Log
                // only the actual first output writer. A printed cache hit is
                // rechecked below for cached ownership context only; that
                // diagnostic result never decides cancellation.
                using WriterKey = std::array<uint64_t, 4>;
                static thread_local std::array<WriterKey, 64> seen {};
                static thread_local size_t seen_count = 0;
                static thread_local int64_t next_report = 0;
                const bool direct_overlap = Overlap(target.desc.info.data, s.native_output_range);
                const WriterKey key {vs, ps, target.target_slot,
                    uint64_t(exact_id) | (uint64_t(direct_overlap) << 1) | (uint64_t(cache_overlap) << 2)};
                const bool known = std::find(seen.begin(), seen.begin() + seen_count, key) != seen.begin() + seen_count;
                const bool first = !known && seen_count < seen.size();
                if (first) seen[seen_count++] = key;
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (first || now >= next_report) {
                    next_report = now + 5000;
                    if (diagnostic != nullptr)
                        *diagnostic = {true, target.target_slot, s.fg_frame, s.fg_scene_serial};
                    std::array<char, 3072> details {};
                    size_t used = 0;
                    const auto append = [&](const char* format, auto... args) {
                        if (used >= details.size() - 1) return;
                        const auto remaining = details.size() - used;
                        const int written = std::snprintf(details.data() + used, remaining, format, args...);
                        if (written > 0) used += std::min<size_t>(static_cast<size_t>(written), remaining - 1);
                    };
                    append("GT7 native FG first SR output writer: frame=%llu scene=%llu first_class=%u vs=%016llx ps=%016llx"
                           " slot=%u target=%u:%u range=0x%llx+0x%llx range_valid=%u extent=%ux%u format=%u"
                           " native=%u:%u range=0x%llx+0x%llx range_valid=%u exact_id=%u direct_overlap=%u"
                           " cache_queried=%u metadata_alias_or_unknown_overlap=%u targets=%zu pixel_images=%zu",
                           static_cast<unsigned long long>(s.fg_frame), static_cast<unsigned long long>(s.fg_scene_serial),
                           unsigned(first), static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps),
                           target.target_slot, target.image_id.index, target.image_id.generation,
                           static_cast<unsigned long long>(target.desc.info.data.address),
                           static_cast<unsigned long long>(target.desc.info.data.size), unsigned(target.desc.info.data.Valid()),
                           target.desc.info.extent.width, target.desc.info.extent.height, unsigned(target.desc.view_info.format),
                           s.fg_scaled_scene_id.index, s.fg_scaled_scene_id.generation,
                           static_cast<unsigned long long>(s.native_output_range.address),
                           static_cast<unsigned long long>(s.native_output_range.size), unsigned(s.native_output_range.Valid()),
                           unsigned(exact_id), unsigned(direct_overlap), unsigned(!exact_id), unsigned(cache_overlap),
                           targets.size(), pixel ? pixel->images.size() : size_t(0));
                    if (cache_overlap) {
                        TextureCache::WriteOverlapDiagnostic overlap;
                        const bool recheck = s.context.GetTextureCache().WriteOverlapsImage(
                            target.desc.info.data, s.fg_scaled_scene_id, s.native_output_range, &overlap);
                        append(" cache_context={recheck=%u reason=%s source=%s image=%u:%u via=%u:%u present=%u registered=%u"
                               " data=0x%llx+0x%llx stencil=0x%llx+0x%llx metadata=0x%llx+0x%llx kind=%u}",
                               unsigned(recheck), overlap.reason, overlap.source, overlap.image.index, overlap.image.generation,
                               overlap.via.index, overlap.via.generation, unsigned(overlap.present), unsigned(overlap.registered),
                               static_cast<unsigned long long>(overlap.data.address), static_cast<unsigned long long>(overlap.data.size),
                               static_cast<unsigned long long>(overlap.stencil.address), static_cast<unsigned long long>(overlap.stencil.size),
                               static_cast<unsigned long long>(overlap.metadata.address), static_cast<unsigned long long>(overlap.metadata.size),
                               unsigned(overlap.metadata_kind));
                    }
                    if (pixel) for (size_t i = 0; i < std::min<size_t>(pixel->images.size(), 4); ++i) {
                        const auto& image = pixel->images[i];
                        append(" tex%zu={id=%u:%u range=0x%llx+0x%llx extent=%ux%u format=%u binding=%u}", i,
                               image.image_id.index, image.image_id.generation,
                               static_cast<unsigned long long>(image.desc.info.data.address),
                               static_cast<unsigned long long>(image.desc.info.data.size),
                               image.desc.info.extent.width, image.desc.info.extent.height,
                               unsigned(image.desc.view_info.format), unsigned(image.desc.type));
                    }
                    std::printf("%s\n", details.data());
                    std::fflush(stdout);
                }
                CancelLiveFrame("native SR output rewritten before tone-map");
                break;
            }
        }
    }
    const auto check = [&](ImageId output_id, GuestRange output_range, size_t slot, bool live) {
    bool writes_output = false;
    for (const auto& target : targets)
        writes_output |= (target.image_id && target.image_id == output_id) || Overlap(target.desc.info.data, output_range);
    if (!writes_output) return;
    // Exact audited UI programs. The additional 25ae pixel program draws analytic
    // clipped rectangular/edge gradients from vertex varyings and a read-only draw uniform;
    // it samples no scene/depth image and exports only MRT0. Its 76d0 vertex program
    // is shared with two existing UI pairs. Identity alone never skips the actual
    // alpha/no-depth, output-view and resource-feedback checks below.
    constexpr std::array<std::array<uint64_t, 2>, 8> ui {
        {{0x352273e237eda017ULL, 0xbbc8d9b8b2bc2c32ULL},
         {0x76d0d62ac9602de2ULL, 0xc711b01042dcbfcfULL},
         {0xa2d70c2dc6a5de23ULL, 0x259b71bf59605f74ULL},
         {0x9a82023cd470256fULL, 0x326fe59634e8b213ULL},
         {0x76d0d62ac9602de2ULL, 0xb301208be2d07966ULL},
         {0x76d0d62ac9602de2ULL, 0x25ae21ddd81e0902ULL},
         {0x25b3b1f6485541d3ULL, 0x6b306f0c24b2242aULL},
         {0x19d44ebf7ab53326ULL, 0x76298aa99b1d5958ULL}}};
    const bool known_pair = std::find(ui.begin(), ui.end(), std::array<uint64_t, 2>{vs, ps}) != ui.end();
    bool allowed = ((ordinary_alpha_overlay && known_pair) || proven_opaque_overlay) &&
        targets.size() == 1 && pixel &&
        targets[0].image_id == output_id && BaseView(targets[0].desc) &&
        targets[0].target_slot == 0 && targets[0].export_mapping.IsIdentity() &&
        targets[0].desc.view_info.format == vk::Format::eA2B10G10R10UnormPack32;
    if (pixel) {
        if (!pixel->bindless_heaps.empty() || !pixel->bindless_patches.empty()) allowed = false;
        for (const auto& texture : pixel->images)
            if (!texture.image_id || texture.image_id == output_id ||
                Overlap(texture.desc.info.data, output_range)) allowed = false;
        for (const auto& source : pixel->buffer_sources)
            if (source.size && Overlap({source.address, source.size}, output_range)) allowed = false;
    }
    if (allowed) { if (live) ++s.fg_ui_draws; return; }
    HudlessDiagnostic archived_diagnostic;
    auto& diagnostic = live ? s.fg_hudless_diagnostic : archived_diagnostic;
    diagnostic.Note("display writer rejected", "unverified draw after HUD-less capture");
    diagnostic.writer_vs = vs;
    diagnostic.writer_ps = ps;
    diagnostic.writer_overlay = ordinary_alpha_overlay;
    diagnostic.writer_targets = targets.size();
    // Diagnostic bits only; the acceptance expression above remains unchanged.
    // 0 overlay, 1 target count, 2 bindings, 3 shader pair, 4 output identity,
    // 5 base view, 6 target slot, 7 export mapping, 8 format, 9 bindless,
    // 10 texture feedback/missing image, 11 buffer feedback.
    uint32_t mask = (!ordinary_alpha_overlay && !proven_opaque_overlay ? 1u << 0 : 0u) |
        (targets.size() != 1 ? 1u << 1 : 0u) | (!pixel ? 1u << 2 : 0u) |
        (!known_pair && !proven_opaque_overlay ? 1u << 3 : 0u);
    if (targets.size() == 1) {
        mask |= (targets[0].image_id != output_id ? 1u << 4 : 0u) |
            (!BaseView(targets[0].desc) ? 1u << 5 : 0u) |
            (targets[0].target_slot != 0 ? 1u << 6 : 0u) |
            (!targets[0].export_mapping.IsIdentity() ? 1u << 7 : 0u) |
            (targets[0].desc.view_info.format != vk::Format::eA2B10G10R10UnormPack32 ? 1u << 8 : 0u);
    }
    if (pixel) {
        if (!pixel->bindless_heaps.empty() || !pixel->bindless_patches.empty()) mask |= 1u << 9;
        for (const auto& texture : pixel->images)
            if (!texture.image_id || texture.image_id == output_id ||
                Overlap(texture.desc.info.data, output_range)) mask |= 1u << 10;
        for (const auto& source : pixel->buffer_sources)
            if (source.size && Overlap({source.address, source.size}, output_range)) mask |= 1u << 11;
    }
    diagnostic.writer_mask = mask;
    // An unknown shader pair is not proof of a UI-only layer. Describe only the
    // already prepared pixel resources; never read guest contents, resolve new
    // images, or change acceptance. Vertex resources and texture provenance are
    // deliberately not inferred from this partial diagnostic.
    if (mask == (1u << 3) && pixel) {
        static std::atomic<int64_t> next_resource_report {0};
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        auto next = next_resource_report.load(std::memory_order_relaxed);
        if (now >= next && next_resource_report.compare_exchange_strong(
                next, now + 5000, std::memory_order_relaxed)) {
            constexpr size_t ResourceLimit = 4;
            const auto image_count = std::min(pixel->images.size(), ResourceLimit);
            const auto buffer_count = std::min(pixel->buffer_sources.size(), ResourceLimit);
            std::array<char, 3072> details {};
            size_t used = 0;
            const auto append = [&](const char* format, auto... args) {
                if (used >= details.size() - 1) return;
                const auto remaining = details.size() - used;
                const int written = std::snprintf(details.data() + used, remaining, format, args...);
                if (written > 0) used += std::min<size_t>(static_cast<size_t>(written), remaining - 1);
            };
            append("GT7 FSR FG HUD resources: frame=%llu vs=%016llx ps=%016llx mask=0x%x"
                   " scope=pixel_only provenance=unknown output=%u:%u output_range=0x%012llx+0x%llx"
                   " images=%zu logged_images=%zu buffers=%zu logged_buffers=%zu samplers=%zu",
                   static_cast<unsigned long long>(s.fg_frame), static_cast<unsigned long long>(vs),
                   static_cast<unsigned long long>(ps), mask, output_id.index, output_id.generation,
                   static_cast<unsigned long long>(output_range.address),
                   static_cast<unsigned long long>(output_range.size), pixel->images.size(), image_count,
                   pixel->buffer_sources.size(), buffer_count, pixel->samplers.size());
            for (size_t i = 0; i < image_count; ++i) {
                const auto& texture = pixel->images[i];
                const auto& info = texture.desc.info;
                const auto& view = texture.desc.view_info;
                append(" tex%zu={id=%u:%u view=%u binding=%u extent=%ux%ux%u samples=%u"
                       " format=%u view_format=%u type=%u mips=%u+%u layers=%u+%u aspect=0x%x"
                       " range=0x%012llx+0x%llx}",
                       i, texture.image_id.index, texture.image_id.generation, unsigned(bool(texture.image_view)),
                       unsigned(texture.desc.type), info.extent.width, info.extent.height, info.extent.depth,
                       info.samples, unsigned(info.pixel_format), unsigned(view.format), unsigned(view.type),
                       view.base_level, view.level_count, view.base_layer, view.layer_count, unsigned(view.aspect),
                       static_cast<unsigned long long>(info.data.address),
                       static_cast<unsigned long long>(info.data.size));
            }
            for (size_t i = 0; i < buffer_count; ++i) {
                const auto& source = pixel->buffer_sources[i];
                // BufferSource::id is a cached identity and may already be stale.
                append(" buffer%zu={cached_id=%u:%u range=0x%012llx+0x%llx}", i,
                       source.id.index, source.id.generation, static_cast<unsigned long long>(source.address),
                       static_cast<unsigned long long>(source.size));
            }
            std::printf("%s\n", details.data());
            std::fflush(stdout);
        }
    }
    const auto writer_scene = live ? s.fg_scene_serial :
        (slot < s.fg_completed.size() ? s.fg_completed_state.Get(slot).scene_serial : uint64_t(0));
    if (live) s.fg_hudless_valid = false;
    if (slot < s.fg_completed.size()) s.InvalidateCompleted(slot, Gt7FrameGen::StatusReason::HudlessUnverified);
    // Keep the actual invalidating writer visible in ordinary Silent playtests.
    // The general UI guard reports unrelated draws too; it cannot identify this
    // cause by proximity. Reuse the existing report bound and prepared state.
    if (++s.fg_hudless_rejections <= 8 || s.fg_hudless_rejections % 300 == 0) {
        std::printf("GT7 FG HUD-less rejected #%llu frame=%llu scene=%llu live=%u: unverified display writer"
                    " vs=%016llx ps=%016llx mask=0x%x overlay=%u opaque=%u targets=%zu live_ui_draws=%u"
                    " output=%u:%u range=0x%llx+0x%llx\n",
             static_cast<unsigned long long>(s.fg_hudless_rejections), static_cast<unsigned long long>(s.fg_frame),
             static_cast<unsigned long long>(writer_scene), unsigned(live),
             static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps),
             mask, unsigned(ordinary_alpha_overlay), unsigned(proven_opaque_overlay), targets.size(), s.fg_ui_draws,
             output_id.index, output_id.generation, static_cast<unsigned long long>(output_range.address),
             static_cast<unsigned long long>(output_range.size));
        std::fflush(stdout);
    }
    };
    bool checked_live = false;
    for (size_t i = 0; i < s.fg_completed.size(); ++i) {
        const auto candidate = s.fg_completed_state.Get(i);
        if (!candidate.ready || candidate.consumed) continue;
        const auto output_id = OutputId(candidate.output);
        const bool live = output_id == s.fg_output_id && candidate.scene_serial == s.fg_scene_serial;
        checked_live |= live;
        check(output_id, {candidate.output.address, candidate.output.size}, i, live);
    }
    if (s.fg_hudless_valid && !checked_live)
        check(s.fg_output_id, s.fg_output_range, s.fg_completed.size(), true);
}

bool Gt7Dlss::CopyFrameGenerationInputs(CommandBuffer& buffer, Gt7FrameGen::FrameInputs& inputs,
                                       const Image& presented_source) {
    auto& s = *m_impl;
    inputs.valid = inputs.camera_valid = inputs.camera_pose_valid = inputs.hudless_valid = false;
    inputs.reset = true;
    inputs.rejection_reason = Gt7FrameGen::StatusReason::WaitingInputs;
    if (!Gt7FrameGen::InputsRequested(s.graphics)) return false;
    using Reason = Gt7FrameGen::StatusReason;
    const auto identity = OutputIdentity(presented_source);
    const auto slot = s.fg_completed_state.Find(identity);
    uint32_t scene_mask = 0, hud_mask = UINT32_MAX;
    const auto reject = [&](const char* reason, Reason status) {
        inputs.rejection_reason = status;
        s.fg_association.Snapshot(presented_source, scene_mask, hud_mask, false, status);
        s.NoteFrameInputResult(false, status);
        if (Report(++s.fg_rejected))
            LOGF("GT7 FSR FG: input snapshot skipped #%llu frame=%llu: %s\n",
                 static_cast<unsigned long long>(s.fg_rejected),
                 static_cast<unsigned long long>(s.fg_frame), reason);
        return false;
    };
    if (!slot) {
        // A missing old output must not consume/reset whichever scene is live
        // now. Tombstones distinguish already-consumed and explicitly rejected
        // outputs without dereferencing a potentially stale cached ImageId.
        const Impl::CompletedScene* recorded = nullptr;
        const Gt7FrameGenAssociation::Candidate* latest = nullptr;
        for (size_t i = 0; i < s.fg_completed.size(); ++i) {
            const auto& candidate = s.fg_completed_state.Get(i);
            if (candidate.scene_serial && candidate.output == identity &&
                (!latest || candidate.scene_serial > latest->scene_serial)) {
                latest = &candidate;
                recorded = &s.fg_completed[i];
            }
        }
        if (latest) {
            hud_mask = 1;
            return reject(latest->consumed ? "completed output already snapshotted" :
                "completed output invalidated by a later writer",
                latest->consumed ? Reason::DuplicateFrame : recorded->rejection);
        }
        // These describe the current live scene only, not a substitute for a
        // completed candidate. No fallback to the latest mutable inputs exists.
        scene_mask = (!(s.native_mode ? s.native_initialized : s.initialized) ? 1u << 0 : 0u) | (s.active ? 1u << 1 : 0u) |
            (s.fg_input_ambiguous ? 1u << 2 : 0u) |
            (s.fg_temporal_attempts != 1 ? 1u << 3 : 0u) |
            ((s.native_mode ? !s.native_ready : s.evaluations != s.boundary_evaluations + 1) ? 1u << 4 : 0u) |
            (!s.native_mode && s.composites != s.boundary_composites + 1 ? 1u << 5 : 0u);
        hud_mask = 1;
        const auto now = std::chrono::steady_clock::now();
        if (now >= s.fg_next_scene_report) {
            s.fg_next_scene_report = now + std::chrono::seconds(5);
            std::printf("GT7 FG completed output missing: frame=%llu live_scene=%llu scene_mask=0x%x"
                        " presented=%u:%u range=0x%llx+0x%llx live_output=%u:%u"
                        " live_closed=%u live_taa=%u live_camera=%u stage=%s reason=%s"
                        " hud_stage=%s hud_reason=%s\n",
                static_cast<unsigned long long>(s.fg_frame),
                static_cast<unsigned long long>(s.fg_scene_serial), scene_mask,
                presented_source.self_id.index, presented_source.self_id.generation,
                static_cast<unsigned long long>(identity.address), static_cast<unsigned long long>(identity.size),
                s.fg_output_id.index, s.fg_output_id.generation, s.fg_scene_closed ? 1u : 0u,
                s.fg_temporal_attempts, s.fg_camera_valid ? 1u : 0u, s.fg_scene_diagnostic.stage,
                s.fg_scene_diagnostic.reason[0] ? s.fg_scene_diagnostic.reason.data() : "none",
                s.fg_hudless_diagnostic.stage,
                s.fg_hudless_diagnostic.reason[0] ? s.fg_hudless_diagnostic.reason.data() : "none");
            std::fflush(stdout);
        }
        return reject("no exact completed scene for the presented output",
                      scene_mask ? Reason::SceneUnverified : Reason::HudlessUnverified);
    }
    auto& completed = s.fg_completed[*slot];
    auto& frozen = completed.inputs;
    // ResolveSurface has already completed any pixel/buffer refresh. Its
    // revision survives clearing the dirty flags, which are cache-lock state
    // and must not be read here as a substitute for this atomic observation.
    if (presented_source.ExternalWriteVersion() != completed.external_write_version) {
        s.InvalidateCompleted(*slot, Reason::HudlessUnverified);
        hud_mask = 1;
        return reject("display contents changed outside verified GPU output writers", Reason::HudlessUnverified);
    }
    if (!frozen.valid || !frozen.camera_valid || !frozen.camera_pose_valid ||
        !frozen.hudless_valid || frozen.guest_frame != s.fg_completed_state.Get(*slot).scene_serial) {
        s.InvalidateCompleted(*slot, Reason::InputUnavailable);
        return reject("completed scene metadata unavailable", Reason::InputUnavailable);
    }
    const std::array<VulkanImage*, 3> sources {&frozen.depth, &frozen.motion, &frozen.hudless_color};
    for (const auto* source : sources) {
        if (!source->image || source->state.layout == vk::ImageLayout::eUndefined ||
            !(source->usage & vk::ImageUsageFlagBits::eTransferSrc))
            return reject("completed scene private input cannot be copied", Reason::InputUnavailable);
    }
    if (!ConfigureFrameGenerationImage(s.graphics, inputs.depth, vk::Format::eR32Sfloat) ||
        !ConfigureFrameGenerationImage(s.graphics, inputs.motion, vk::Format::eR32G32Sfloat) ||
        !ConfigureFrameGenerationImage(s.graphics, inputs.hudless_color,
            vk::Format::eA2B10G10R10UnormPack32, GuestWidth, GuestHeight))
        return reject("frame-owned temporal image allocation unavailable", Reason::InputUnavailable);

    buffer.EndRendering();
    const auto command = buffer.Handle();
    const std::array<VulkanImage*, 3> destinations {&inputs.depth, &inputs.motion, &inputs.hudless_color};
    const vk::ImageSubresourceLayers layer {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    for (size_t i = 0; i < sources.size(); ++i) {
        auto& source = *sources[i];
        auto& destination = *destinations[i];
        const vk::ImageCopy region {layer, {}, layer, {}, i == 2 ?
            vk::Extent3D {GuestWidth, GuestHeight, 1} : vk::Extent3D {InputWidth, InputHeight, 1}};
        TransitFrameGenerationImage(command, source, vk::ImageLayout::eTransferSrcOptimal,
                                     vk::AccessFlagBits2::eTransferRead);
        TransitFrameGenerationImage(command, destination, vk::ImageLayout::eTransferDstOptimal,
                                     vk::AccessFlagBits2::eTransferWrite);
        command.copyImage(source.image, vk::ImageLayout::eTransferSrcOptimal,
                          destination.image, vk::ImageLayout::eTransferDstOptimal, 1, &region);
        TransitFrameGenerationImage(command, source, SampledLayout, ShaderRead);
        TransitFrameGenerationImage(command, destination, SampledLayout, ShaderRead);
    }
    // Candidate images are never submitted directly to either FG backend.
    // The presenter signals producer-ready after these frame-owned copies. A
    // later scene can reuse the slot on this same ordered GPU recording lane;
    // its transfer barriers wait for the preceding candidate reads.
    inputs.render_extent = frozen.render_extent;
    inputs.guest_frame = frozen.guest_frame;
    inputs.jitter_x = frozen.jitter_x; inputs.jitter_y = frozen.jitter_y;
    inputs.frame_time_ms = frozen.frame_time_ms;
    inputs.near_plane = frozen.near_plane; inputs.far_plane = frozen.far_plane;
    inputs.fov_y = frozen.fov_y;
    std::copy(std::begin(frozen.camera_position), std::end(frozen.camera_position), inputs.camera_position);
    std::copy(std::begin(frozen.camera_right), std::end(frozen.camera_right), inputs.camera_right);
    std::copy(std::begin(frozen.camera_up), std::end(frozen.camera_up), inputs.camera_up);
    std::copy(std::begin(frozen.camera_forward), std::end(frozen.camera_forward), inputs.camera_forward);
    inputs.depth_infinite = frozen.depth_infinite;
    inputs.camera_valid = inputs.camera_pose_valid = inputs.hudless_valid = inputs.valid = true;
    inputs.reset = frozen.reset;
    s.fg_completed_state.Consume(*slot);
    if (frozen.guest_frame == s.fg_scene_serial) s.fg_hudless_valid = false;
    ++s.fg_completed_consumed;
    s.fg_association.Snapshot(presented_source, 0, 0, true, Reason::WaitingInputs);
    s.NoteFrameInputResult(true, Reason::WaitingInputs);
    if (Report(++s.fg_snapshots))
        LOGF("GT7 FSR FG: captured frame=%llu scene=%llu output=%u:%u slot=%zu 1920x1080"
             " reverse-Z finite near=%.9g far=%.9g fovY=%.9g jitter=(%.6f,%.6f) reset=%u\n",
             static_cast<unsigned long long>(s.fg_frame), static_cast<unsigned long long>(inputs.guest_frame),
             presented_source.self_id.index, presented_source.self_id.generation, *slot,
             inputs.near_plane, inputs.far_plane, inputs.fov_y, inputs.jitter_x, inputs.jitter_y, inputs.reset ? 1u : 0u);
    return true;
}
