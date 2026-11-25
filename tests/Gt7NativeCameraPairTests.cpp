#include "graphics/host_gpu/renderer/gt7NativeCameraPair.h"
#include "graphics/host_gpu/renderer/gt7FrameGenCamera.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
namespace Pair = Libs::Graphics::Gt7NativeCameraPair;
namespace Camera = Libs::Graphics::Gt7FrameGenCamera;
using Pair::Matrix;
using Pair::DepthRange;
using Pair::Decision;
int checks = 0;
void Require(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

Matrix Perspective(double near_plane, double far_plane, float jitter = 0.0f) {
    return {1.1f,0,0,0, 0,1.1f*1920.0f/1080.0f,0,0, jitter,0,
        float(near_plane/(far_plane-near_plane)),-1, 0,0,
        float(near_plane*far_plane/(far_plane-near_plane)),0};
}
Matrix Product(const Matrix& view, const Matrix& projection) {
    Matrix result {};
    for (size_t row = 0; row < 4; ++row) for (size_t column = 0; column < 4; ++column) {
        double value = 0;
        for (size_t k = 0; k < 4; ++k) value += double(view[row*4+k])*projection[k*4+column];
        result[row*4+column] = float(value);
    }
    return result;
}
Pair::Proof CompleteProof() {
    return {.native=true, .valid=true, .rejected=false, .pending=false, .exact_pair=true, .decoded=true,
        .trusted_vertex=true, .snapshot_ready=true, .snapshot_address_matches=true, .snapshot_bits_match=true,
        .no_attachment_alias=true, .clean_after=true, .witnesses_match=true};
}
bool Decoded(const Matrix& vp, const Matrix& view, const Matrix& projection, DepthRange range) {
    Camera::RasterState raster {.z_scale=range.maximum-range.minimum, .z_offset=range.minimum,
        .position_format=Libs::Graphics::ViewportTransform::IncomingW, .dx_clip_space=true,
        .min_z_clip_enabled=true, .max_z_clip_enabled=true};
    Camera::Parameters parameters;
    return Camera::DecodeNative(vp, view, projection, raster, 1920, 1080, parameters);
}

// CPU lifecycle fixture: it models only the policy's RetainAnchor/Reject
// transition, not texture-cache, GPU ordering or runtime camera provenance.
struct Canonical {
    Matrix vp, view, projection;
    DepthRange range;
    std::array<float, 20> parameters {};
    std::array<uint64_t, 8> witnesses {};
    Pair::Camera AsCamera() const { return {vp,view,projection,range}; }
    bool Same(const Canonical& other) const {
        return vp == other.vp && view == other.view && projection == other.projection &&
            range.minimum == other.range.minimum && range.maximum == other.range.maximum &&
            parameters == other.parameters && witnesses == other.witnesses;
    }
};
}

int main() {
    const DepthRange biased {std::bit_cast<float>(uint32_t{0x3606a8c9}), 1.0f};
    const DepthRange normal {0,1};
    const Matrix identity {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const Matrix rotated {.8f,0,.6f,0, 0,1,0,0, -.6f,0,.8f,0, 100,200,-300,1};
    const Matrix translated {.8f,0,.6f,0, 0,1,0,0, -.6f,0,.8f,0, 10000,-20000,6000,1};
    const auto source_projection = Perspective(.01,3800);
    const auto canonical_projection = Perspective(.01,16000);
    const auto proof = CompleteProof();
    for (const auto& view : {identity, rotated, translated}) {
        const auto source = Product(view,source_projection);
        const auto canonical = Product(view,canonical_projection);
        const Pair::Camera anchor {canonical,view,canonical_projection,normal};
        const Pair::Camera incoming {source,view,source_projection,biased};
        Require(Decoded(canonical,view,canonical_projection,normal), "canonical independently passes DecodeNative");
        Require(Decoded(source,view,source_projection,biased), "compensated camera independently passes DecodeNative");
        Require(anchor.range.minimum != incoming.range.minimum && anchor.view_projection != incoming.view_projection,
            "regression positive is rejected by the previous raw viewport and matrix equality gates");
        Require(Pair::Evaluate(anchor,incoming,proof) == Decision::RetainAnchor, "different raw viewport and projection preserve effective depth");
        Require(Pair::Evaluate(incoming,anchor,proof) == Decision::RetainAnchor, "compensation works with either first canonical representation");
        Require(Pair::Evaluate(anchor,{canonical,view,canonical_projection,biased},proof) == Decision::DepthChanged,
            "viewport change without projection compensation rejects");
        Require(Pair::Evaluate(anchor,{source,view,source_projection,normal},proof) == Decision::DepthChanged,
            "projection change without viewport compensation rejects");
        for (const auto& projection : {Perspective(.01001,3800),Perspective(.01,3900)}) {
            const auto vp = Product(view,projection);
            Require(Pair::Evaluate(anchor,{vp,view,projection,biased},proof) == Decision::DepthChanged,
                "different effective near or far plane rejects");
        }
        auto moved = view; moved[12] += 1.0f;
        Require(Pair::Evaluate(anchor,{source,moved,source_projection,biased},proof) == Decision::PoseChanged,
            "different pose rejects even if VP is copied unchanged");
        for (size_t i = 0; i < 16; ++i) {
            if (i == 10 || i == 14) continue;
            auto projection = source_projection;
            projection[i] = std::nextafter(projection[i],std::numeric_limits<float>::infinity());
            Require(Pair::Evaluate(anchor,{source,view,projection,biased},proof) == Decision::ProjectionChanged,
                "every nondepth projection field including jitter FOV and W remains exact");
        }
        for (size_t i = 0; i < 16; ++i) {
            if (i % 4 == 2) continue;
            auto vp = source;
            vp[i] = std::nextafter(vp[i],std::numeric_limits<float>::infinity());
            Require(Pair::Evaluate(anchor,{vp,view,source_projection,biased},proof) == Decision::DepthChanged,
                "bound VP XYW is exact even if decomposition metadata is copied");
        }

        Canonical state {canonical,view,canonical_projection,normal};
        state.parameters = {.01f,16000,1.0f,.25f,-.125f,1,2,3};
        state.witnesses = {10,20,30,40,50,60,70,80};
        const auto first = state;
        auto lifecycle = proof;
        for (int i = 0; i < 16; ++i) {
            const auto& writer = i % 2 ? anchor : incoming;
            Require(Pair::Evaluate(state.AsCamera(),writer,lifecycle) == Decision::RetainAnchor,
                "alternating equivalent writers keep the first canonical reference");
            Require(state.Same(first), "all canonical matrices parameters ranges and witnesses remain unchanged");
        }
        auto changed_projection = Perspective(.01,3900);
        auto changed_vp = Product(view,changed_projection);
        const auto rejected = Pair::Evaluate(state.AsCamera(),{changed_vp,view,changed_projection,biased},lifecycle);
        Require(rejected == Decision::DepthChanged, "real change is compared against original anchor after many writers");
        lifecycle.valid = false; lifecycle.rejected = true;
        Require(Pair::Evaluate(state.AsCamera(),incoming,lifecycle) == Decision::Ineligible,
            "a later equivalent writer cannot revive a rejected scene");
        Require(state.Same(first), "rejection never rebases canonical storage");
    }

    const Pair::Camera anchor {canonical_projection,identity,canonical_projection,normal};
    const Pair::Camera incoming {source_projection,identity,source_projection,biased};
    struct Gate { bool Pair::Proof::*field; Decision rejection; };
    const Gate gates[] {
        {&Pair::Proof::native,Decision::Ineligible}, {&Pair::Proof::valid,Decision::Ineligible},
        {&Pair::Proof::exact_pair,Decision::Ineligible}, {&Pair::Proof::decoded,Decision::Ineligible},
        {&Pair::Proof::trusted_vertex,Decision::UntrustedVertex},
        {&Pair::Proof::snapshot_ready,Decision::SnapshotUnavailable},
        {&Pair::Proof::snapshot_address_matches,Decision::SnapshotAddressChanged},
        {&Pair::Proof::snapshot_bits_match,Decision::SnapshotBitsChanged},
        {&Pair::Proof::no_attachment_alias,Decision::AttachmentAlias},
        {&Pair::Proof::clean_after,Decision::BackingDirty},
        {&Pair::Proof::witnesses_match,Decision::WitnessChanged},
    };
    for (const auto& gate : gates) {
        auto missing = proof; missing.*(gate.field) = false;
        Require(Pair::Evaluate(anchor,incoming,missing) == gate.rejection, "every runtime proof prerequisite fails closed");
    }
    for (auto member : {&Pair::Proof::rejected,&Pair::Proof::pending}) {
        auto blocked = proof; blocked.*member = true;
        Require(Pair::Evaluate(anchor,incoming,blocked) == Decision::Ineligible, "rejected or unfinished first witness cannot enter fallback");
    }
    auto invalid_projection = source_projection; invalid_projection[10] = std::numeric_limits<float>::quiet_NaN();
    auto invalid = proof;
    invalid.decoded = Decoded(source_projection,identity,invalid_projection,biased);
    Require(!invalid.decoded && Pair::Evaluate(anchor,{source_projection,identity,invalid_projection,biased},invalid) == Decision::Ineligible,
        "DecodeNative validity remains a prerequisite for the decomposition");
    Require(Pair::Evaluate(anchor,{source_projection,identity,source_projection,{1,0}},proof) == Decision::DepthChanged,
        "invalid viewport remains rejected by unchanged numerical helper");
    std::printf("Gt7NativeCameraPairTests: %d checks passed\n", checks);
}
