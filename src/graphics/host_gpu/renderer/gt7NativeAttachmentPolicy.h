#pragma once

#include <cmath>
#include <cstddef>
#include <span>

namespace Libs::Graphics::Gt7NativeAttachmentPolicy {

enum class Role { Depth, Motion, Other };
enum class Clear { None, ZeroDepth, Other };

struct Write {
    bool native = false;
    bool verified_camera = false;
    bool writes = false;
    bool exact_image = false;
    Role attachment_role = Role::Other;
    Role input_role = Role::Other;
    Clear clear = Clear::None;
    bool depth_mapping_matches = true;
};

[[nodiscard]] inline bool SameDepthMapping(float minimum, float maximum,
                                           float saved_minimum, float saved_maximum) {
    return std::isfinite(minimum) && std::isfinite(maximum) &&
           std::isfinite(saved_minimum) && std::isfinite(saved_maximum) &&
           minimum == saved_minimum && maximum == saved_maximum;
}

struct DepthRange { float minimum, maximum; };

// A final shader can select any viewport when it exports ViewportIndex. Match
// SetGraphicsDynamicParams' active-slot rule; unused slots need not be valid.
[[nodiscard]] inline bool EnabledViewportsMatch(bool indexed, std::span<const DepthRange> viewports) {
    if (viewports.empty()) return false;
    const auto count = indexed ? viewports.size() : 1;
    for (std::size_t i = 0; i < count; ++i)
        if (!SameDepthMapping(viewports[i].minimum, viewports[i].maximum,
                              viewports[0].minimum, viewports[0].maximum)) return false;
    return true;
}

// The native PSSR request, accepted only after successful SR evaluation, names
// the final depth/MV pair and their scalar convention. Normal ordered attachment
// writes may fill that pair in separate depth-only, EQUAL or tessellated passes.
// A camera observation supplies matrices; it is not a proof of every pixel's
// motion. Identity/epoch, camera conflicts, unknown writes and the final PSSR
// capture remain independent guards. An alias is never an exact image here.
[[nodiscard]] constexpr bool Allows(const Write& write) {
    if (!write.writes) return true;
    if (!write.exact_image || write.attachment_role != write.input_role ||
        write.input_role == Role::Other) return false;
    if (write.native && write.input_role == Role::Depth && !write.depth_mapping_matches) return false;
    // Keep the original clear exception: only a verified camera's zero-depth
    // clear can retain the witness. Other clears do not establish association.
    if (write.clear != Clear::None)
        return write.input_role == Role::Depth && write.clear == Clear::ZeroDepth &&
               write.verified_camera;
    return write.native || write.verified_camera;
}

} // namespace Libs::Graphics::Gt7NativeAttachmentPolicy
