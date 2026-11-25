#pragma once

#include <array>
#include <cstdint>

namespace Libs::Graphics::Gt7DlssEffectCoverage {

struct State {
    uint32_t color_samples = 0;
    bool depth_attachment = false;
    bool sample_shading = false;
    bool alpha_to_coverage = false;
    bool depth_export = false;
    bool sample_mask_export = false;
    bool pixel_discard = false;
    std::array<uint32_t, 2> sample_masks {};
};

// This contract applies only after the exact geometric shader pair, destination,
// blend equation and absence of resource feedback have been verified by callers.
// With a color attachment and no depth, PipelineCache chooses the raster sample
// count from that attachment. PA_SC_AA_CONFIG is only its no-attachment fallback;
// stale values there must not reject an otherwise identical single-sample replay.
// The four halfwords describe a 2x2 pixel quad. Only sample 0 exists in each pixel.
[[nodiscard]] constexpr bool CanReplay(const State& state) {
    constexpr uint32_t sample_zero_in_both_pixels = 0x00010001u;
    return state.color_samples == 1 && !state.depth_attachment &&
           !state.sample_shading && !state.alpha_to_coverage &&
           !state.depth_export && !state.sample_mask_export && !state.pixel_discard &&
           (state.sample_masks[0] & sample_zero_in_both_pixels) == sample_zero_in_both_pixels &&
           (state.sample_masks[1] & sample_zero_in_both_pixels) == sample_zero_in_both_pixels;
}

} // namespace Libs::Graphics::Gt7DlssEffectCoverage
