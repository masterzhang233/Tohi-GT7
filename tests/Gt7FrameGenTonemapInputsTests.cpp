#include "graphics/host_gpu/renderer/gt7FrameGenTonemapInputs.h"

#include <cstdio>
#include <cstdlib>

namespace {
using namespace Libs::Graphics::Gt7FrameGenTonemapInputs;
unsigned checks = 0;
void Require(bool condition, const char* label) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}

TextureFacts Texture(size_t index) {
    constexpr uint32_t formats[] {1, 1, 2, 3, 3};
    TextureFacts texture;
    texture.image = texture.view = texture.sampled_binding = true;
    texture.extent = texture.backing_extent = LegacyExtents[index];
    texture.samples = texture.backing_samples = 1;
    texture.format = texture.expected_format = texture.backing_format = formats[index];
    texture.level_count = texture.layer_count = 1;
    texture.view_type = texture.backing_type = ExpectedDimension(index);
    texture.range_valid = texture.backing_image = texture.backing_sampled = texture.backing_format_compatible = true;
    texture.backing_levels = texture.backing_layers = 1;
    texture.resource.present = texture.resource.sampled = texture.resource.read = true;
    texture.resource.numeric = texture.descriptor_numeric = NumericClass::Float;
    texture.view_format_valid = texture.color_aspect = true;
    return texture;
}

// Literal predicate from the previous per-texture loop. Backing validity and
// range-validity were not extra legacy acceptance conditions.
bool LegacyBefore(size_t index, const TextureFacts& texture) {
    return texture.image && texture.view && texture.sampled_binding &&
        texture.extent == LegacyExtents[index] && texture.samples == 1 &&
        texture.format == texture.expected_format && texture.base_level == 0 && texture.level_count == 1 &&
        texture.base_layer == 0 && texture.layer_count == 1 &&
        texture.view_type == ExpectedDimension(index) && !texture.output_feedback;
}

void Mutate(TextureFacts& texture, unsigned variant) {
    switch (variant) {
        case 0: break;
        case 1: texture.image = false; break;
        case 2: texture.view = false; break;
        case 3: texture.sampled_binding = false; break;
        case 4: ++texture.extent.width; break;
        case 5: texture.extent.height = 0; break;
        case 6: ++texture.extent.depth; break;
        case 7: texture.samples = 4; break;
        case 8: texture.format = 0; texture.view_format_valid = false; break;
        case 9: texture.base_level = 1; break;
        case 10: texture.level_count = 2; break;
        case 11: texture.base_layer = 1; break;
        case 12: texture.layer_count = 2; break;
        case 13: texture.view_type = Dimension::Other; break;
        case 14: texture.output_feedback = true; break;
        case 15: texture.range_valid = false; break;
        case 16: texture.backing_image = false; break;
        case 17: ++texture.backing_extent.width; break;
        case 18: texture.backing_format = 999; texture.backing_format_compatible = false; break;
        case 19: texture.backing_type = Dimension::Other; break;
        case 20: texture.backing_samples = 4; break;
        case 21: texture.backing_levels = 0; break;
        case 22: texture.backing_layers = 0; break;
        case 23: texture.backing_sampled = false; break;
    }
}
}

int main() {
    for (size_t index = 0; index < LegacyExtents.size(); ++index) {
        Require(Check(false, index, Texture(index)) == Failure::None, "legacy captured tuple accepts");
        Require(Check(true, index, Texture(index)) == Failure::None, "native matching captured tuple accepts");
        for (unsigned a = 0; a < 24; ++a) for (unsigned b = 0; b < 24; ++b) {
            auto texture = Texture(index);
            Mutate(texture, a);
            Mutate(texture, b);
            Require((Check(false, index, texture) == Failure::None) == LegacyBefore(index, texture),
                    "legacy descriptor/feedback predicate remains exactly equivalent");
        }
    }

    struct Variant { size_t index; Extent extent; };
    constexpr Variant variants[] {
        {1, {1920, 1080, 1}}, {1, {480, 270, 1}},
        {2, {4, 4, 1}}, {2, {16, 8, 1}},
        {3, {32, 32, 32}}, {3, {128, 128, 128}}, {3, {16, 32, 64}},
        {4, {1920, 1080, 1}}, {4, {960, 540, 1}}};
    for (const auto& variant : variants) {
        auto texture = Texture(variant.index);
        texture.extent = texture.backing_extent = variant.extent;
        Require(Check(true, variant.index, texture) == Failure::None, "native auxiliary accepts matching valid game allocation");
        Require(Check(false, variant.index, texture) == Failure::Extent, "legacy auxiliary still requires captured extent");
        ++texture.backing_extent.width;
        Require(Check(true, variant.index, texture) == Failure::BackingExtent, "variable dimensions do not accept stale or mismatched backing");
    }

    auto scene = Texture(0);
    scene.extent = scene.backing_extent = {1920, 1080, 1};
    Require(Check(true, 0, scene) == Failure::Extent, "native main scene remains exactly 4K");
    Require(Check(true, 5, Texture(0)) == Failure::Index, "unknown resource slot rejects");
    constexpr Failure expected[] {
        Failure::None, Failure::Image, Failure::View, Failure::Binding, Failure::BackingExtent,
        Failure::Extent, Failure::Extent, Failure::Samples, Failure::Format, Failure::BaseLevel,
        Failure::LevelCount, Failure::BaseLayer, Failure::LayerCount, Failure::ViewType, Failure::Feedback,
        Failure::Range, Failure::BackingImage, Failure::BackingExtent, Failure::BackingFormat,
        Failure::BackingType, Failure::BackingSamples, Failure::BackingLevels, Failure::BackingLayers,
        Failure::BackingUsage};
    for (unsigned variant = 0; variant < 24; ++variant) {
        auto texture = Texture(1);
        Mutate(texture, variant);
        Require(Check(true, 1, texture) == expected[variant], "native invalid descriptor/backing reports exact first field");
    }
    for (size_t index = 0; index < LegacyExtents.size(); ++index) {
        auto texture = Texture(index);
        texture.output_feedback = true;
        Require(Check(true, index, texture) == Failure::Feedback, "all native texture slots retain output feedback rejection");
        Require(Check(false, index, texture) == Failure::Feedback, "all legacy texture slots retain output feedback rejection");
    }
    auto lut = Texture(3);
    lut.view_type = Dimension::Two;
    Require(Check(true, 3, lut) == Failure::ViewType, "LUT slot cannot become a 2D image");
    lut = Texture(3);
    lut.backing_type = Dimension::Two;
    Require(Check(true, 3, lut) == Failure::BackingType, "3D LUT view must have 3D backing");
    auto auxiliary = Texture(4);
    auxiliary.backing_format = 456;
    Require(Check(true, 4, auxiliary) == Failure::None, "compatible mutable backing may differ from the expected sampled view format");
    auxiliary.backing_format_compatible = false;
    Require(Check(true, 4, auxiliary) == Failure::BackingFormat, "incompatible backing is rejected even with a correctly formatted view");
    auxiliary = Texture(4);
    auxiliary.backing_levels = 5;
    auxiliary.backing_layers = 3;
    Require(Check(true, 4, auxiliary) == Failure::None, "one checked base view may use a backing with additional mips/layers");
    auxiliary.level_count = 5;
    Require(Check(true, 4, auxiliary) == Failure::LevelCount, "extra backing mips do not widen the accepted view");

    // Actual run 20261125-201720: native slot 1 is a valid R8_UNORM 1x1
    // guest allocation, not the 960x540 B10G11R11 input from the legacy capture.
    auto placeholder = Texture(1);
    placeholder.extent = placeholder.backing_extent = {1, 1, 1};
    placeholder.format = placeholder.backing_format = 9;
    Require(Check(true, 1, placeholder) == Failure::None, "observed native 1x1 R8 optional input accepts");
    Require(Check(false, 1, placeholder) == Failure::Extent, "observed native placeholder does not alter legacy contract");

    // Vulkan R8_UNORM, RGBA8_UNORM, RGBA16_SFLOAT and B10G11R11_UFLOAT.
    // Format class comes from the same resolved sampler metadata as production.
    constexpr uint32_t float_formats[] {9, 37, 97, 122};
    for (const auto format : float_formats) {
        for (size_t index = 1; index < LegacyExtents.size(); ++index) {
            auto texture = Texture(index);
            texture.format = texture.backing_format = format;
            Require(Check(true, index, texture) == Failure::None, "every native auxiliary accepts valid resolved float formats");
            Require(Check(false, index, texture) == Failure::Format, "legacy auxiliary formats remain exact");
        }
        auto main_scene = Texture(0);
        main_scene.format = main_scene.backing_format = format;
        Require(Check(true, 0, main_scene) == Failure::Format, "native main scene format remains exact");
    }
    for (size_t index = 1; index < LegacyExtents.size(); ++index) {
        for (unsigned variant = 0; variant < 17; ++variant) {
            auto texture = Texture(index);
            auto expected_failure = Failure::Resource;
            switch (variant) {
                case 0: texture.resource.present = false; break;
                case 1: texture.resource.sampled = false; break;
                case 2: texture.resource.read = false; break;
                case 3: texture.resource.written = true; break;
                case 4: texture.resource.atomic = true; break;
                case 5: texture.resource.atomic64 = true; break;
                case 6: texture.resource.depth_compare = true; break;
                case 7: texture.resource.bindless = true; break;
                case 8: texture.resource.runtime = true; break;
                case 9: texture.resource.numeric = NumericClass::Unsupported; expected_failure = Failure::NumericClass; break;
                case 10: texture.resource.numeric = NumericClass::Uint; expected_failure = Failure::NumericClass; break;
                case 11: texture.resource.numeric = NumericClass::Sint; expected_failure = Failure::NumericClass; break;
                case 12: texture.descriptor_numeric = NumericClass::Unsupported; expected_failure = Failure::NumericClass; break;
                case 13: texture.descriptor_numeric = NumericClass::Uint; expected_failure = Failure::NumericClass; break;
                case 14: texture.descriptor_numeric = NumericClass::Sint; expected_failure = Failure::NumericClass; break;
                case 15: texture.view_format_valid = false; expected_failure = Failure::Format; break;
                case 16: texture.color_aspect = false; expected_failure = Failure::ColorAspect; break;
            }
            Require(Check(true, index, texture) == expected_failure, "native float policy rejects invalid shader/descriptor class or aspect");
            Require(Check(false, index, texture) == Failure::None, "new native resource requirements do not change legacy acceptance");
        }
    }

    std::printf("GT7 tone-map texture policy: %u checks passed\n", checks);
    return 0;
}
