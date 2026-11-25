#include "graphics/presentation/window/gt7DlssFrameGenPolicy.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace Policy = Libs::Graphics::Gt7DlssFrameGenPolicy;

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
}
bool Close(float actual, float expected) {
    return std::isfinite(actual) && std::abs(actual - expected) <= 1e-6f;
}
}

int main() {
    // Regression from the 2026-11-25 game run: 10,477 successful presents,
    // one out-of-date attempt, and 10,478 SDK presents with FG always off.
    Check(Policy::GeneratedPresentationsLowerBound(10478, 10478, false) == 0,
          "off-only resize cannot report a generated frame");
    Check(Policy::GeneratedPresentationsLowerBound(10478, 10478, true) == 0,
          "resize remains excluded after an earlier enabled interval");
    Check(Policy::GeneratedPresentationsLowerBound(10479, 10478, false) == 0,
          "never-enabled feature cannot claim SDK excess as generation");
    Check(Policy::GeneratedPresentationsLowerBound(17, 18, true) == 0 &&
          Policy::GeneratedPresentationsLowerBound(18, 18, true) == 0,
          "delayed real-frame polls neither underflow nor claim generation");
    Check(Policy::GeneratedPresentationsLowerBound(30, 18, true) == 12,
          "controlled fixed-2x run retains twelve extra presentations");
    Check(Policy::GeneratedPresentationsLowerBound(30, 19, true) == 11,
          "an uncounted failed application attempt conservatively reduces the bound");
    Check(!Policy::SupportsFixed2x(0), "missing runtime capability must reject enable");
    for (const uint32_t maximum : {1u, 2u, 3u, 5u, std::numeric_limits<uint32_t>::max()}) {
        Check(Policy::SupportsFixed2x(maximum), "single generated frame supported");
        const auto enabled = Policy::Options(true);
        Check(enabled.mode == sl::DLSSGMode::eOn, "fixed On mode, never Auto or Dynamic");
        Check(enabled.numFramesToGenerate == 1 && enabled.numFramesToGenerate <= maximum,
              "higher runtime capability must not select higher multiplier");
    }
    for (const bool enabled : {false, true}) {
        const auto options = Policy::Options(enabled);
        Check(options.mode == (enabled ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff),
              "suspension explicitly disables interpolation");
        Check(options.numFramesToGenerate == 1, "off/on transitions retain fixed 2x selection");
        Check(options.flags == sl::DLSSGFlags::eRetainResourcesWhenOff,
              "transient missing inputs retain resources without extra feature flags");
        Check(options.queueParallelismMode == sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue,
              "presenting queue remains ordered with FG input consumption");
        Check(options.enableUserInterfaceRecomposition == sl::Boolean::eFalse,
              "HUD-less input does not claim an unavailable UI alpha layer");
        Check(options.dynamicTargetFrameRate == 0, "no dynamic target or frame limiter requested");
    }

    // A point was at (960, 540), and now is at (1200, 300) in the 1080p
    // render. Backward UV displacement is (-1/8, 2/9). Upscaling that frame
    // to 3840x2160 must not halve these normalized motion vectors.
    const auto scale = Policy::MotionScale(1920, 1080);
    Check(Close((960.0f - 1200.0f) * scale.x, -0.125f) &&
          Close((540.0f - 300.0f) * scale.y, 2.0f / 9.0f),
          "1080p render motion preserves backward direction and magnitude at 4K output");
    // The equivalent point displacement measured directly at native 4K has
    // twice the pixel magnitude and must normalize to the same UV vector.
    const auto native4k_scale = Policy::MotionScale(3840, 2160);
    Check(Close(-480.0f * native4k_scale.x, -0.125f) &&
          Close(480.0f * native4k_scale.y, 2.0f / 9.0f),
          "motion normalization follows input dimensions");

    std::printf("GT7 fixed 2x DLSS-G policy checks: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
