#pragma once

#include <cstdint>
#include <limits>

namespace Libs::Graphics::Gt7FrameGenInputWitness {

struct Stamp {
    uint64_t image_id = 0;
    uint64_t native_image = 0;
    uint64_t address = 0;
    uint64_t size = 0;
    uint64_t version = 0;
    uint64_t epoch = 0;
};

inline bool SameIdentity(const Stamp& saved, const Stamp& current) {
    return saved.native_image != 0 && saved.size != 0 &&
        saved.address <= std::numeric_limits<uint64_t>::max() - saved.size &&
        saved.image_id == current.image_id && saved.native_image == current.native_image &&
        saved.address == current.address && saved.size == current.size;
}

inline bool Matches(const Stamp& saved, const Stamp& current) {
    return SameIdentity(saved, current) && saved.version == current.version && saved.epoch == current.epoch;
}

enum class Finalize { Reject, CaptureFirst, Retain };

// A first camera is decoded before acquiring its attachments so scene boundary
// handling retains its original order. Only completion of already known dirty
// state may occur before that first witness is committed. Later witnesses and
// final temporal capture always require the complete retained stamp.
inline Finalize AfterAcquire(bool pending_first, bool valid, bool rejected,
                             const Stamp& saved_depth, const Stamp& saved_motion,
                             const Stamp& current_depth, const Stamp& current_motion) {
    if (!valid || rejected || !SameIdentity(saved_depth, current_depth) ||
        !SameIdentity(saved_motion, current_motion)) return Finalize::Reject;
    if (pending_first) {
        return saved_depth.epoch == current_depth.epoch && saved_motion.epoch == current_motion.epoch
            ? Finalize::CaptureFirst : Finalize::Reject;
    }
    return Matches(saved_depth, current_depth) && Matches(saved_motion, current_motion)
        ? Finalize::Retain : Finalize::Reject;
}

} // namespace Libs::Graphics::Gt7FrameGenInputWitness
