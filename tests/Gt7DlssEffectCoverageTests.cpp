#include "graphics/host_gpu/renderer/pipeline/gt7DlssEffectCoverage.h"

#include <cstdio>
#include <initializer_list>

using namespace Libs::Graphics::Gt7DlssEffectCoverage;

int main() {
    int failures = 0;
    const auto check = [&](bool good, const char* name) {
        if (!good) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
    };

    // Nurburgring frame 9963: PA_SC_AA_CONFIG reports 2/2 (four samples),
    // but this draw binds a one-sample RT0 with no depth, no per-sample
    // shader behavior and full coverage. The actual attachment determines both
    // the original Vulkan pipeline and the private effect-layer pipeline.
    const State captured {.color_samples = 1, .sample_masks = {0xffffffffu, 0xffffffffu}};
    check(CanReplay(captured), "captured one-sample draw with stale guest AA 2/2 accepted");

    // Exhaust all 2x2 coverage patterns. An effect replay may only proceed when
    // it preserves all four pixels, regardless of unused samples' mask bits.
    for (uint32_t pixels = 0; pixels < 16; ++pixels) {
        auto state = captured;
        state.sample_masks = {0xfffefffeu, 0xfffefffeu};
        for (uint32_t pixel = 0; pixel < 4; ++pixel)
            if (pixels & (1u << pixel))
                state.sample_masks[pixel / 2] |= 1u << ((pixel % 2) * 16);
        check(CanReplay(state) == (pixels == 15), "all four effective pixel coverage bits required");
    }
    auto unused_samples = captured;
    unused_samples.sample_masks = {0x00010001u, 0x00010001u};
    check(CanReplay(unused_samples), "disabled nonexistent samples do not affect one-sample coverage");

    for (uint32_t samples : {0u, 2u, 4u, 8u, 16u}) {
        auto multisampled = captured;
        multisampled.color_samples = samples;
        check(!CanReplay(multisampled), "missing or truly multisampled color attachment rejected");
    }
    for (auto member : {&State::depth_attachment, &State::sample_shading,
                        &State::alpha_to_coverage, &State::depth_export,
                        &State::sample_mask_export, &State::pixel_discard}) {
        auto sample_dependent = captured;
        sample_dependent.*member = true;
        check(!CanReplay(sample_dependent), "sample-dependent shader or raster coverage rejected");
    }
    std::printf("GT7 effect coverage checks: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
