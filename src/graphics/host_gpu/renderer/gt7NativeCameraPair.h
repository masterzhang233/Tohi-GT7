#pragma once

#include "graphics/host_gpu/renderer/gt7NativeDepthProjection.h"

namespace Libs::Graphics::Gt7NativeCameraPair {

using Matrix = Gt7NativeDepthProjection::Matrix;
using DepthRange = Gt7NativeDepthProjection::DepthRange;

// Views into an already decoded camera. Neither this policy nor its decision
// permits replacing the first camera/parameters/input witnesses of the scene.
struct Camera {
    const Matrix& view_projection;
    const Matrix& view;
    const Matrix& projection;
    DepthRange range;
};

struct Proof {
    bool native = false;
    bool valid = false;
    bool rejected = false;
    bool pending = false;
    bool exact_pair = false;
    bool decoded = false;
    bool trusted_vertex = false;
    bool snapshot_ready = false;
    bool snapshot_address_matches = false;
    bool snapshot_bits_match = false;
    bool no_attachment_alias = false;
    bool clean_after = false;
    bool witnesses_match = false;
};

enum class Decision : uint32_t {
    RetainAnchor, Ineligible, PoseChanged, ProjectionChanged, UntrustedVertex,
    SnapshotUnavailable, SnapshotAddressChanged, SnapshotBitsChanged,
    AttachmentAlias, BackingDirty, WitnessChanged, DepthChanged
};

[[nodiscard]] constexpr const char* Name(Decision decision) {
    switch (decision) {
        case Decision::RetainAnchor: return "equivalent-existing-camera";
        case Decision::Ineligible: return "existing-camera-ineligible";
        case Decision::PoseChanged: return "camera-pose-changed";
        case Decision::ProjectionChanged: return "camera-nondepth-projection-changed";
        case Decision::UntrustedVertex: return "camera-prepared-vertex-unverified";
        case Decision::SnapshotUnavailable: return "camera-bound-snapshot-unavailable";
        case Decision::SnapshotAddressChanged: return "camera-bound-snapshot-address-changed";
        case Decision::SnapshotBitsChanged: return "camera-bound-snapshot-differs-from-live-vp";
        case Decision::AttachmentAlias: return "camera-structure-aliases-attachment";
        case Decision::BackingDirty: return "camera-structure-backing-stale-after-read";
        case Decision::WitnessChanged: return "camera-input-witness-changed";
        case Decision::DepthChanged: return "camera-effective-depth-not-equivalent";
    }
    return "unknown-camera-pair-decision";
}

[[nodiscard]] inline bool SameNonDepthProjection(const Matrix& a, const Matrix& b) {
    for (size_t i = 0; i < a.size(); ++i)
        if (i != 10 && i != 14 && a[i] != b[i]) return false;
    return true;
}

[[nodiscard]] inline Decision Evaluate(const Camera& anchor, const Camera& incoming, const Proof& proof) {
    if (!proof.native || !proof.valid || proof.rejected || proof.pending || !proof.exact_pair || !proof.decoded)
        return Decision::Ineligible;
    if (anchor.view != incoming.view) return Decision::PoseChanged;
    if (!SameNonDepthProjection(anchor.projection, incoming.projection)) return Decision::ProjectionChanged;
    if (!proof.trusted_vertex) return Decision::UntrustedVertex;
    if (!proof.snapshot_ready) return Decision::SnapshotUnavailable;
    if (!proof.snapshot_address_matches) return Decision::SnapshotAddressChanged;
    if (!proof.snapshot_bits_match) return Decision::SnapshotBitsChanged;
    if (!proof.no_attachment_alias) return Decision::AttachmentAlias;
    if (!proof.clean_after) return Decision::BackingDirty;
    if (!proof.witnesses_match) return Decision::WitnessChanged;
    return Gt7NativeDepthProjection::Equivalent(incoming.view_projection, incoming.range,
                                               anchor.view_projection, anchor.range)
        ? Decision::RetainAnchor : Decision::DepthChanged;
}

} // namespace Libs::Graphics::Gt7NativeCameraPair
