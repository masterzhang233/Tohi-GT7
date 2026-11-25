#pragma once

#include <cmath>
#include <cstdint>

namespace Libs::Graphics::Gt7NativePssrInputs {

// This bridge describes the observed full-frame native PSSR path. Accepting
// these scalar parameters does not prove the camera, image identity or HUD;
// the recording hooks must establish those separately for the same scene.
[[nodiscard]] inline bool Compatible(uint32_t width, uint32_t height, uint32_t x, uint32_t y,
                                     uint32_t output_width, uint32_t output_height,
                                     bool reverse_depth, bool spatial_only, uint64_t context,
                                     const float* jitter, const float* motion_scale) {
    if (width != 1920 || height != 1080 || x != 0 || y != 0 ||
        output_width != 3840 || output_height != 2160 ||
        !reverse_depth || spatial_only || context == 0 || !jitter || !motion_scale) return false;
    for (uint32_t axis = 0; axis != 2; ++axis) {
        if (!std::isfinite(jitter[axis]) || std::abs(jitter[axis]) > 2.0f ||
            !std::isfinite(motion_scale[axis]) || motion_scale[axis] == 0.0f) return false;
    }
    return true;
}

// Projection jitter is NDC; the native request is in render pixels, Y down.
// Bound the allowance in pixels, not in NDC or relative to an arbitrary value.
// Double intermediates avoid adding another float rounding step to the check.
[[nodiscard]] inline bool CameraJitterMatches(float ndc_x, float ndc_y, const float* jitter,
                                              uint32_t width, uint32_t height) {
    if (!jitter || width == 0 || height == 0 ||
        !std::isfinite(ndc_x) || !std::isfinite(ndc_y) ||
        !std::isfinite(jitter[0]) || !std::isfinite(jitter[1])) return false;
    constexpr double PixelRoundoff = 1.0e-5;
    const double pixel_x = double(ndc_x) * (-double(width) * 0.5);
    const double pixel_y = double(ndc_y) * (double(height) * 0.5);
    return std::abs(pixel_x - double(jitter[0])) <= PixelRoundoff &&
           std::abs(pixel_y - double(jitter[1])) <= PixelRoundoff;
}

} // namespace Libs::Graphics::Gt7NativePssrInputs
