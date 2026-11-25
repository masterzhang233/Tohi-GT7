#include "graphics/host_gpu/renderer/gt7NativeAttachmentPolicy.h"
#include "graphics/host_gpu/renderer/gt7FrameGenInputWitness.h"

#include <cstdio>
#include <cstdlib>
#include <array>
#include <initializer_list>
#include <limits>

namespace {
using namespace Libs::Graphics::Gt7NativeAttachmentPolicy;
namespace Witness = Libs::Graphics::Gt7FrameGenInputWitness;
int checks = 0;
void Require(bool value, const char* description) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
}

Write Depth(bool native = true, bool verified = false) {
    return {native, verified, true, true, Role::Depth, Role::Depth, Clear::None};
}
Write Motion(bool native = true, bool verified = false) {
    return {native, verified, true, true, Role::Motion, Role::Motion, Clear::None};
}

// Apply actual policy decisions to the retained witness. Once a real rejection
// happens, a later verified write must not revive the candidate.
struct Candidate {
    bool valid = true;
    Witness::Stamp stamp {1, 2, 0x1000, 0x100, 4, 3};
    void WriteAttachment(const Write& write) { valid = valid && Allows(write); }
    bool Capture(const Witness::Stamp& current) const { return valid && Witness::Matches(stamp, current); }
};
}

int main() {
    Require(Allows(Depth()), "native depth-only prepass retains the camera association");
    Require(Allows(Motion()), "native motion-only pass need not write depth");
    Candidate native;
    native.WriteAttachment(Depth()); // Depth-only, Greater test, no PS/MV export.
    native.WriteAttachment(Depth()); // EQUAL depth write of the same input.
    native.WriteAttachment(Motion()); // EQUAL shading also exports motion.
    native.WriteAttachment(Depth()); // Final TES attachment write, no VS camera observation.
    native.WriteAttachment(Motion());
    Require(native.Capture(native.stamp), "ordered depth-only, EQUAL and tessellated contributions can reach capture");
    auto read_only = Depth();
    read_only.writes = false;
    read_only.exact_image = false;
    read_only.attachment_role = Role::Other;
    Require(Allows(read_only), "read-only attachment is not treated as an alias write");
    read_only.depth_mapping_matches = false;
    Require(Allows(read_only), "read-only depth cannot invalidate the candidate's depth mapping");
    Require(SameDepthMapping(0.0f, 1.0f, 0.0f, 1.0f), "same viewport endpoints match");
    Require(!SameDepthMapping(0.0f, 0.5f, 0.0f, 1.0f), "changed depth slope rejects");
    Require(!SameDepthMapping(0.1f, 1.0f, 0.0f, 1.0f), "changed depth offset rejects");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    Require(!SameDepthMapping(nan, 1.0f, nan, 1.0f), "NaN viewport cannot match");
    Require(!SameDepthMapping(0.0f, infinity, 0.0f, infinity), "equal infinite endpoints reject");
    auto changed_mapping = Depth();
    changed_mapping.depth_mapping_matches = false;
    Require(!Allows(changed_mapping), "unobserved native depth writer cannot change observed affine");
    changed_mapping.verified_camera = true;
    Require(!Allows(changed_mapping), "verified flag cannot hide a native affine conflict");
    changed_mapping.native = false;
    Require(Allows(changed_mapping), "new affine policy does not change legacy acceptance");
    auto motion_mapping = Motion();
    motion_mapping.depth_mapping_matches = false;
    Require(Allows(motion_mapping), "motion-only pass does not write the depth mapping");
    std::array<DepthRange, 3> viewports {{{0.0f, 1.0f}, {0.0f, 1.0f}, {0.0f, 1.0f}}};
    Require(EnabledViewportsMatch(true, viewports), "indexed viewports with matching endpoints accept");
    viewports[1].maximum = 0.5f;
    Require(!EnabledViewportsMatch(true, viewports), "indexed viewport one differing depth mapping rejects");
    Require(EnabledViewportsMatch(false, viewports), "nonindexed draw ignores unused viewport mappings");
    viewports[1] = viewports[0];
    viewports[2].minimum = nan;
    Require(!EnabledViewportsMatch(true, viewports), "indexed nonzero viewport NaN rejects");
    viewports[2].minimum = infinity;
    Require(!EnabledViewportsMatch(true, viewports), "indexed nonzero viewport infinity rejects");
    Require(EnabledViewportsMatch(false, viewports), "unused nonfinite viewport does not reject nonindexed draw");
    viewports[0].maximum = infinity;
    Require(!EnabledViewportsMatch(false, viewports), "nonindexed active viewport must be finite");
    Require(!EnabledViewportsMatch(true, {}), "missing viewport state rejects");
    auto indexed_depth = Depth();
    indexed_depth.depth_mapping_matches = EnabledViewportsMatch(true, viewports);
    Require(!Allows(indexed_depth), "indexed mapping conflict rejects actual native depth write");
    indexed_depth.writes = false;
    Require(Allows(indexed_depth), "indexed conflict does not turn read-only depth into a write");
    auto indexed_motion = Motion();
    indexed_motion.depth_mapping_matches = EnabledViewportsMatch(true, viewports);
    Require(Allows(indexed_motion), "indexed conflict does not reject motion-only attachment writes");

    for (bool native_mode : {false, true}) {
        for (bool verified : {false, true}) {
            auto depth = Depth(native_mode, verified);
            auto motion = Motion(native_mode, verified);
            Require(Allows(depth) == (native_mode || verified), "ordinary depth acceptance is mode-specific");
            Require(Allows(motion) == (native_mode || verified), "ordinary motion acceptance is mode-specific");
            depth.exact_image = false;
            motion.exact_image = false;
            Require(!Allows(depth), "different image, data alias or metadata-only depth alias rejects");
            Require(!Allows(motion), "different image, data alias or metadata-only motion alias rejects");
            depth = Depth(native_mode, verified);
            depth.attachment_role = Role::Motion;
            motion = Motion(native_mode, verified);
            motion.attachment_role = Role::Depth;
            Require(!Allows(depth), "motion role cannot overwrite watched depth");
            Require(!Allows(motion), "depth role cannot overwrite watched motion");
            motion.attachment_role = Role::Other;
            Require(!Allows(motion), "color slot other than motion rejects even with exact image");
            depth = Depth(native_mode, verified);
            depth.clear = Clear::ZeroDepth;
            Require(Allows(depth) == verified, "zero depth clear still requires verified camera");
            depth.clear = Clear::Other;
            Require(!Allows(depth), "nonzero or nonfinite depth clear always rejects");
            motion = Motion(native_mode, verified);
            motion.clear = Clear::Other;
            Require(!Allows(motion), "generic clear is not an ordered attachment exception");
        }
    }
    Require(!Allows(Depth(false)) && !Allows(Motion(false)), "legacy unverified writes retain prior rejection");
    Require(Allows(Depth(false, true)) && Allows(Motion(false, true)), "legacy verified writes retain prior acceptance");

    auto changed = native.stamp;
    ++changed.version;
    Require(!native.Capture(changed), "allowed draws do not bypass external version check");
    changed = native.stamp;
    ++changed.epoch;
    Require(!native.Capture(changed), "allowed draws do not bypass CPU or buffer write epoch");
    changed = native.stamp;
    ++changed.image_id;
    Require(!native.Capture(changed), "resource reuse cannot be hidden by attachment policy");
    changed = native.stamp;
    ++changed.native_image;
    Require(!native.Capture(changed), "native image replacement still rejects");
    Candidate rejected;
    auto clear = Depth();
    clear.clear = Clear::ZeroDepth;
    rejected.WriteAttachment(clear);
    rejected.WriteAttachment(Depth(true, true));
    Require(!rejected.Capture(rejected.stamp), "verified observation after rejected clear cannot revive witness");

    std::printf("GT7 native attachment policy: %d checks passed\n", checks);
    return 0;
}
