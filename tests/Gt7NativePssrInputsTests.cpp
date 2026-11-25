#include "graphics/host_gpu/renderer/gt7NativePssrInputs.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace Libs::Graphics::Gt7NativePssrInputs;

namespace {
struct Request {
    uint32_t width = 1920, height = 1080, x = 0, y = 0;
    uint32_t output_width = 3840, output_height = 2160;
    bool reverse_depth = true, spatial_only = false;
    uint64_t context = 1;
    // Existing native PSSR run, request 60. These are scalar request values,
    // not evidence that its camera or HUD was captured.
    std::array<float, 2> jitter {-0.21875f, 0.42592591f};
    std::array<float, 2> motion_scale {-960.0f, 540.0f};

    bool Accepted() const {
        return Compatible(width, height, x, y, output_width, output_height,
                          reverse_depth, spatial_only, context, jitter.data(), motion_scale.data());
    }
};
} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool good, const char* name) {
        if (!good) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
    };
    const auto reject = [&](auto mutate, const char* name) {
        Request request;
        mutate(request);
        check(!request.Accepted(), name);
    };
    const Request native;
    check(native.Accepted(), "observed native 1080-to-4K scalar contract");
    reject([](auto& r) { r.width = 1919; }, "cropped render width");
    reject([](auto& r) { r.height = 1079; }, "cropped render height");
    reject([](auto& r) { r.x = 1; }, "nonzero source X");
    reject([](auto& r) { r.y = 1; }, "nonzero source Y");
    reject([](auto& r) { r.output_width = 2560; }, "legacy 1440p output width");
    reject([](auto& r) { r.output_height = 1440; }, "legacy 1440p output height");
    reject([](auto& r) { r.reverse_depth = false; }, "forward depth");
    reject([](auto& r) { r.spatial_only = true; }, "spatial-only request");
    reject([](auto& r) { r.context = 0; }, "missing temporal context");
    check(!Compatible(1920, 1080, 0, 0, 3840, 2160, true, false, 1,
                      nullptr, native.motion_scale.data()), "missing jitter");
    check(!Compatible(1920, 1080, 0, 0, 3840, 2160, true, false, 1,
                      native.jitter.data(), nullptr), "missing motion scale");
    for (uint32_t axis = 0; axis != 2; ++axis) {
        for (float bad : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()}) {
            reject([&](auto& r) { r.jitter[axis] = bad; }, "nonfinite request jitter");
            reject([&](auto& r) { r.motion_scale[axis] = bad; }, "nonfinite motion scale");
        }
        for (float zero : {0.0f, -0.0f})
            reject([&](auto& r) { r.motion_scale[axis] = zero; }, "zero motion scale");
        for (float boundary : {-2.0f, 2.0f}) {
            auto request = native;
            request.jitter[axis] = boundary;
            check(request.Accepted(), "two-pixel jitter boundary");
            reject([&](auto& r) {
                r.jitter[axis] = std::nextafter(boundary, boundary * 2.0f);
            }, "jitter beyond two pixels");
        }
    }
    auto scaled = native;
    scaled.motion_scale = {1920.0f, -1080.0f};
    check(scaled.Accepted(), "finite nonzero request scale is not hardcoded");

    const float ndc_x = native.jitter[0] / -960.0f;
    const float ndc_y = native.jitter[1] / 540.0f;
    const auto matches = [&](float x, float y, const std::array<float, 2>& jitter) {
        return CameraJitterMatches(x, y, jitter.data(), 1920, 1080);
    };
    check(matches(ndc_x, ndc_y, native.jitter), "NDC conversion tolerates float rounding");
    check(!matches(-ndc_x, ndc_y, native.jitter), "wrong horizontal convention");
    check(!matches(ndc_x, -ndc_y, native.jitter), "wrong vertical convention");
    check(!matches(ndc_y, ndc_x, native.jitter), "swapped camera axes");
    check(!matches(ndc_x, ndc_y, {0.28125f, 0.0925925672f}), "different actual jitter sample");
    check(!CameraJitterMatches(ndc_x, ndc_y, native.jitter.data(), 3840, 2160),
          "display dimensions cannot substitute for render dimensions");
    const std::array<float, 2> origin {0.0f, -0.0f};
    check(matches(-0.0f, 0.0f, origin), "signed zero jitter");
    check(matches(0.0f, 0.0f, {9.9e-6f, -9.9e-6f}), "sub-bound pixel roundoff");
    check(!matches(0.0f, 0.0f, {1.01e-5f, 0.0f}), "X allowance capped at 1e-5 pixel");
    check(!matches(0.0f, 0.0f, {0.0f, -1.01e-5f}), "Y allowance capped at 1e-5 pixel");
    check(!CameraJitterMatches(ndc_x, ndc_y, nullptr, 1920, 1080), "missing camera comparison jitter");
    check(!CameraJitterMatches(0, 0, origin.data(), 0, 1080), "zero render width");
    check(!CameraJitterMatches(0, 0, origin.data(), 1920, 0), "zero render height");
    for (float bad : {std::numeric_limits<float>::quiet_NaN(),
                      std::numeric_limits<float>::infinity(),
                      -std::numeric_limits<float>::infinity()}) {
        check(!matches(bad, ndc_y, native.jitter), "nonfinite camera X");
        check(!matches(ndc_x, bad, native.jitter), "nonfinite camera Y");
        check(!matches(ndc_x, ndc_y, {bad, native.jitter[1]}), "nonfinite compared request X");
        check(!matches(ndc_x, ndc_y, {native.jitter[0], bad}), "nonfinite compared request Y");
    }
    if (failures == 0) std::puts("GT7 native PSSR scalar and camera-jitter contracts passed");
    return failures == 0 ? 0 : 1;
}
