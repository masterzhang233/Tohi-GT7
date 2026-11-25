#pragma once

#include <cstdint>
#include <sl_dlss_g.h>

namespace Libs::Graphics::Gt7DlssFrameGenPolicy {

// One interpolated frame between real frames is fixed 2x, including on GPUs
// which advertise a larger maximum. Never select Auto or Dynamic here.
inline constexpr uint32_t GeneratedFrames = 1;

[[nodiscard]] constexpr bool SupportsFixed2x(uint32_t maximum_generated_frames) {
    return maximum_generated_frames >= GeneratedFrames;
}

// The SDK can count a present attempt which Vulkan reports as out-of-date.
// Subtract every application attempt, including failures, so a resize cannot
// masquerade as an interpolated frame. This is a conservative cumulative
// lower bound, not a display FPS measurement or a per-poll generation count.
[[nodiscard]] constexpr uint64_t GeneratedPresentationsLowerBound(
    uint64_t sdk_presentations, uint64_t application_attempts, bool generation_was_enabled) {
    return generation_was_enabled && sdk_presentations > application_attempts
        ? sdk_presentations - application_attempts : 0;
}

[[nodiscard]] inline sl::DLSSGOptions Options(bool enabled) {
    sl::DLSSGOptions options {};
    options.mode = enabled ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    options.numFramesToGenerate = GeneratedFrames;
    options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    // The same-frame HUD-less image supports ordinary UI extraction. No
    // separate alpha image exists, so do not enable UI recomposition.
    options.enableUserInterfaceRecomposition = sl::Boolean::eFalse;
    return options;
}

// FrameInputs motion is unjittered current-to-previous displacement in render
// pixels. Normalize by its own dimensions, even when presentation is 4K.
// RecordPrepare validates nonzero dimensions before calling this helper.
[[nodiscard]] inline sl::float2 MotionScale(uint32_t render_width, uint32_t render_height) {
    return {1.0f / render_width, 1.0f / render_height};
}

} // namespace Libs::Graphics::Gt7DlssFrameGenPolicy
