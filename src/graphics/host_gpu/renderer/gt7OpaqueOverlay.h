#pragma once

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>

namespace Libs::Graphics::Gt7OpaqueOverlay {

// The decoded programs transform position by one matrix and interpolate vertex RGBA;
// PS optionally applies a uniform-selected RGB colour transform. Neither reads an image.
// This is only the program identity part of the proof: renderDraw also checks the actual
// pipeline, descriptors and every resolved input against watched pixels AND metadata.
constexpr bool KnownPrograms(uint64_t vertex, uint64_t pixel) {
    return vertex == 0xe7256a93fa207358ULL && pixel == 0x152988376c074445ULL;
}

struct ReadRange {
    uint64_t address = 0;
    uint64_t size = 0;

    constexpr bool Valid() const {
        return address != 0 && size != 0 && size <= std::numeric_limits<uint64_t>::max() - address;
    }
};

// Attribute base includes its byte offset within the vertex. The last record reads
// one format, not another complete stride. A stride-zero constant still reads a full
// format (not zero bytes, nor NumRecords==1 byte under OOB_SELECT=2).
constexpr ReadRange AttributeRange(uint64_t address, uint32_t stride, uint32_t records,
                                   uint32_t format_bytes, uint8_t out_of_bounds) {
    if (records == 0 || format_bytes == 0 ||
        (stride == 0 && out_of_bounds != 2 && records < format_bytes)) return {};
    return {address, stride == 0 ? uint64_t(format_bytes) :
        uint64_t(stride) * (records - 1u) + format_bytes};
}

constexpr bool Contains(ReadRange outer, ReadRange inner) {
    return outer.Valid() && inner.Valid() && inner.address >= outer.address &&
        inner.address - outer.address <= outer.size &&
        inner.size <= outer.size - (inner.address - outer.address);
}

// Distinct spans make omissions visible to the caller/reviewer. The proven program
// has exactly two uniform inputs and two vertex attributes; both the attributes and
// the host vertex-buffer extents are checked. Index/indirect controls and any captured
// scalar-specialization reads are inputs too. overlaps must include image metadata
// and cache aliases, not just intersect the current colour target's pixel range.
template <typename Overlaps>
bool IndependentReads(std::span<const ReadRange> uniforms,
                      std::span<const ReadRange> attributes,
                      std::span<const ReadRange> vertex_buffers,
                      std::span<const ReadRange> draw_controls,
                      std::span<const ReadRange> specialization_reads,
                      Overlaps&& overlaps) {
    if (uniforms.size() != 2 || attributes.size() != 2 ||
        vertex_buffers.empty() || vertex_buffers.size() > 2) return false;
    for (const auto group : {uniforms, attributes, vertex_buffers, draw_controls, specialization_reads})
        for (const auto range : group)
            if (!range.Valid() || overlaps(range)) return false;
    return true;
}

} // namespace Libs::Graphics::Gt7OpaqueOverlay
