#include "graphics/host_gpu/renderer/gt7OpaqueOverlay.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
using namespace Libs::Graphics::Gt7OpaqueOverlay;
int checks = 0;
void Require(bool value, const char* description) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
}

struct Fixture {
    std::array<ReadRange, 2> uniforms {{{0x1000, 64}, {0x2000, 16}}};
    std::array<ReadRange, 2> attributes {{{0x3000, 112}, {0x3010, 112}}};
    std::array<ReadRange, 1> vertices {{{0x3000, 128}}};
    std::array<ReadRange, 3> controls {{{0x4000, 12}, {0x5000, 20}, {0x6000, 4}}};
    std::array<ReadRange, 1> specialization {{{0x7000, 8}}};
    std::vector<ReadRange> watched {{0x100000, 4096}, {0x200000, 512}};
    size_t visits = 0;

    bool Check() {
        visits = 0;
        return IndependentReads(uniforms, attributes, vertices, controls, specialization,
            [&](ReadRange range) {
                ++visits;
                for (auto storage : watched)
                    if (range.address < storage.address + storage.size &&
                        storage.address < range.address + range.size) return true;
                return false;
            });
    }
};
}

int main() {
    Require(KnownPrograms(0xe7256a93fa207358ULL, 0x152988376c074445ULL), "proven pair");
    Require(!KnownPrograms(0xe7256a93fa207359ULL, 0x152988376c074445ULL), "unknown VS");
    Require(!KnownPrograms(0xe7256a93fa207358ULL, 0x152988376c074444ULL), "unknown PS");
    const auto attribute = AttributeRange(0x3010, 32, 4, 16, 0);
    Require(attribute.address == 0x3010 && attribute.size == 112, "last attribute uses format, not full stride");
    Require(Contains({0x3000, 128}, attribute), "interleaved last attribute ends exactly at buffer end");
    Require(!Contains({0x3000, 127}, attribute), "clamped vertex buffer rejects missing last byte");
    Require(AttributeRange(0x3000, 0, 1, 16, 2).size == 16, "constant OOB2 consumes full format");
    Require(!AttributeRange(0x3000, 0, 1, 16, 0).Valid(), "short byte-bounded constant rejects");
    Require(!AttributeRange(0x3000, 32, 0, 16, 0).Valid(), "empty attribute rejects");
    Require(!Contains({0x3000, 128}, {0x2fff, 16}), "attribute before vertex base rejects");
    Require(!ReadRange {UINT64_MAX - 7, 16}.Valid(), "overflow rejects");
    Require(AttributeRange(0x3000, 16383, UINT32_MAX, 16, 0).size > UINT32_MAX,
            "large strided range uses 64-bit arithmetic");

    Fixture valid;
    Require(valid.Check() && valid.visits == 9, "all read categories are visited");
    // Pixel storage and metadata may have unrelated addresses. Test alias rejection
    // independently for every input, including the VS constant and vertex-buffer
    // span omitted by the old pixel-only HUD check.
    for (size_t i = 0; i < 9; ++i) {
        Fixture f;
        std::array<ReadRange*, 9> inputs {&f.uniforms[0], &f.uniforms[1], &f.attributes[0], &f.attributes[1],
            &f.vertices[0], &f.controls[0], &f.controls[1], &f.controls[2], &f.specialization[0]};
        inputs[i]->address = i % 2 ? 0x100010 : 0x200010;
        Require(!f.Check(), "pixel/metadata alias in any resolved input rejects");
    }
    for (size_t i = 0; i < 9; ++i) {
        Fixture f;
        std::array<ReadRange*, 9> inputs {&f.uniforms[0], &f.uniforms[1], &f.attributes[0], &f.attributes[1],
            &f.vertices[0], &f.controls[0], &f.controls[1], &f.controls[2], &f.specialization[0]};
        inputs[i]->size = 0;
        Require(!f.Check(), "unresolved/empty input rejects");
    }
    Fixture boundary;
    boundary.uniforms[0] = {0x100000 - 64, 64};
    Require(boundary.Check(), "touching range is not overlap");
    boundary.uniforms[0].size++;
    Require(!boundary.Check(), "one-byte overlap rejects");
    Fixture overflow;
    overflow.uniforms[0] = {UINT64_MAX - 7, 16};
    Require(!overflow.Check() && overflow.visits == 0, "overflow rejects before alias arithmetic");
    const auto never_alias = [](ReadRange) { return false; };
    Require(!IndependentReads(std::span(valid.uniforms).first(1), valid.attributes, valid.vertices,
        valid.controls, valid.specialization, never_alias), "missing uniform rejects");
    Require(!IndependentReads(valid.uniforms, std::span(valid.attributes).first(1), valid.vertices,
        valid.controls, valid.specialization, never_alias), "missing attribute rejects");
    Require(!IndependentReads(valid.uniforms, valid.attributes, std::span<const ReadRange> {},
        valid.controls, valid.specialization, never_alias), "missing vertex binding rejects");
    Require(IndependentReads(valid.uniforms, valid.attributes, valid.vertices,
        std::span<const ReadRange> {}, std::span<const ReadRange> {}, never_alias),
        "direct draw with no specialization reads needs no control range");
    std::printf("opaque overlay read contract: %d checks passed\n", checks);
}
