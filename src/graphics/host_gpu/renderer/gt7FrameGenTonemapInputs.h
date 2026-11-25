#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Libs::Graphics::Gt7FrameGenTonemapInputs {

struct Extent {
    uint32_t width = 0, height = 0, depth = 0;
    constexpr bool operator==(const Extent&) const = default;
    [[nodiscard]] constexpr bool Valid() const { return width && height && depth; }
};
inline constexpr std::array<Extent, 5> LegacyExtents {{
    {3840, 2160, 1}, {960, 540, 1}, {1, 1, 1}, {64, 64, 64}, {3840, 2160, 1}}};

enum class Dimension { Other, Two, Three };
enum class NumericClass { Unsupported, Float, Uint, Sint };
enum class Failure {
    None, Index, Image, View, Binding, Extent, Samples, Format, BaseLevel,
    LevelCount, BaseLayer, LayerCount, ViewType, Range, Feedback, BackingImage,
    BackingExtent, BackingFormat, BackingType, BackingSamples, BackingLevels,
    BackingLayers, BackingUsage, Resource, NumericClass, ColorAspect
};

struct ResourceFacts {
    bool present = false, sampled = false, read = false, written = false;
    bool atomic = false, atomic64 = false, depth_compare = false;
    bool bindless = false, runtime = false;
    NumericClass numeric = NumericClass::Unsupported;
};

struct TextureFacts {
    bool image = false, view = false, sampled_binding = false;
    Extent extent {};
    uint32_t samples = 0, format = 0, expected_format = 0;
    uint32_t base_level = 0, level_count = 0, base_layer = 0, layer_count = 0;
    Dimension view_type = Dimension::Other;
    bool range_valid = false, output_feedback = false;
    bool backing_image = false, backing_sampled = false, backing_format_compatible = false;
    Extent backing_extent {};
    uint32_t backing_format = 0, backing_samples = 0, backing_levels = 0, backing_layers = 0;
    Dimension backing_type = Dimension::Other;
    ResourceFacts resource {};
    NumericClass descriptor_numeric = NumericClass::Unsupported;
    bool view_format_valid = false, color_aspect = false;
};

[[nodiscard]] constexpr Dimension ExpectedDimension(size_t index) {
    return index == 3 ? Dimension::Three : Dimension::Two;
}

// The native request identifies the full 4K scene. Auxiliary bloom/LUT/noise
// allocations are game inputs, not temporal-image association dimensions.
// Keep the original legacy tuple and all common descriptor checks unchanged.
[[nodiscard]] constexpr Failure CheckDescriptor(bool native, size_t index, const TextureFacts& texture) {
    if (index >= LegacyExtents.size()) return Failure::Index;
    if (!texture.image) return Failure::Image;
    if (!texture.view) return Failure::View;
    if (!texture.sampled_binding) return Failure::Binding;
    if ((!native || index == 0) && texture.extent != LegacyExtents[index]) return Failure::Extent;
    if (native && index != 0 && (!texture.extent.Valid() ||
        (ExpectedDimension(index) == Dimension::Two && texture.extent.depth != 1))) return Failure::Extent;
    if (texture.samples != 1) return Failure::Samples;
    if (native && index != 0) {
        // FG copies the guest's completed tone-map output. Its auxiliary
        // textures follow the actual sampled-resource contract, not the format
        // tuple from one legacy capture (optional inputs can be R8 placeholders).
        const auto& resource = texture.resource;
        if (!resource.present || !resource.sampled || !resource.read || resource.written ||
            resource.atomic || resource.atomic64 || resource.depth_compare ||
            resource.bindless || resource.runtime) return Failure::Resource;
        if (resource.numeric != NumericClass::Float || texture.descriptor_numeric != NumericClass::Float)
            return Failure::NumericClass;
        if (!texture.view_format_valid) return Failure::Format;
        if (!texture.color_aspect) return Failure::ColorAspect;
    } else if (texture.format != texture.expected_format) return Failure::Format;
    if (texture.base_level != 0) return Failure::BaseLevel;
    if (texture.level_count != 1) return Failure::LevelCount;
    if (texture.base_layer != 0) return Failure::BaseLayer;
    if (texture.layer_count != 1) return Failure::LayerCount;
    if (texture.view_type != ExpectedDimension(index)) return Failure::ViewType;
    if (native && index != 0 && !texture.range_valid) return Failure::Range;
    return Failure::None;
}

// Native auxiliary extent flexibility requires a matching, usable resolved
// backing. Extra backing mips/layers are harmless with the checked single base
// view. The main scene backing is still validated by the caller's 4K Shape and
// exact native image/content-version checks before this helper is reached.
[[nodiscard]] constexpr Failure CheckBacking(bool native, size_t index, const TextureFacts& texture) {
    if (texture.output_feedback) return Failure::Feedback;
    if (!native || index == 0) return Failure::None;
    if (!texture.backing_image) return Failure::BackingImage;
    if (texture.backing_extent != texture.extent) return Failure::BackingExtent;
    // The resolved sampled view may legally reinterpret a compatible mutable
    // backing. The caller supplies the same compatibility rule as FindView.
    if (!texture.backing_format_compatible) return Failure::BackingFormat;
    if (texture.backing_type != ExpectedDimension(index)) return Failure::BackingType;
    if (texture.backing_samples != 1) return Failure::BackingSamples;
    if (texture.backing_levels < 1) return Failure::BackingLevels;
    if (texture.backing_layers < 1) return Failure::BackingLayers;
    if (!texture.backing_sampled) return Failure::BackingUsage;
    return Failure::None;
}

[[nodiscard]] constexpr Failure Check(bool native, size_t index, const TextureFacts& texture) {
    const auto descriptor = CheckDescriptor(native, index, texture);
    return descriptor != Failure::None ? descriptor : CheckBacking(native, index, texture);
}

[[nodiscard]] constexpr const char* Name(Failure failure) {
    switch (failure) {
        case Failure::None: return "none";
        case Failure::Index: return "index";
        case Failure::Image: return "image-id";
        case Failure::View: return "image-view";
        case Failure::Binding: return "binding-type";
        case Failure::Extent: return "descriptor-extent";
        case Failure::Samples: return "descriptor-samples";
        case Failure::Format: return "view-format";
        case Failure::BaseLevel: return "base-level";
        case Failure::LevelCount: return "level-count";
        case Failure::BaseLayer: return "base-layer";
        case Failure::LayerCount: return "layer-count";
        case Failure::ViewType: return "view-type";
        case Failure::Range: return "descriptor-range";
        case Failure::Feedback: return "output-feedback";
        case Failure::BackingImage: return "backing-image";
        case Failure::BackingExtent: return "backing-extent";
        case Failure::BackingFormat: return "backing-format";
        case Failure::BackingType: return "backing-type";
        case Failure::BackingSamples: return "backing-samples";
        case Failure::BackingLevels: return "backing-levels";
        case Failure::BackingLayers: return "backing-layers";
        case Failure::BackingUsage: return "backing-sampled-usage";
        case Failure::Resource: return "shader-sampled-resource";
        case Failure::NumericClass: return "sampled-numeric-class";
        case Failure::ColorAspect: return "sampled-color-aspect";
    }
    return "unknown";
}

} // namespace Libs::Graphics::Gt7FrameGenTonemapInputs
