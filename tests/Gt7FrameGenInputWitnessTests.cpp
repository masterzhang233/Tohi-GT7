#include "graphics/host_gpu/renderer/gt7FrameGenInputWitness.h"
#include "graphics/host_gpu/renderer/image/externalWriteState.h"

#include <cstdio>
#include <limits>

using Libs::Graphics::ImageExternalWriteState;
using namespace Libs::Graphics::Gt7FrameGenInputWitness;

int main() {
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool result, const char* label) {
        ++checks;
        if (!result) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
    };
    ImageExternalWriteState depth_state, motion_state;
    const auto current = [](uint64_t id, uint64_t image, uint64_t address,
                            const ImageExternalWriteState& state) {
        // Match production sampling: version first, then epoch. A new write
        // after this sample is still caught by the final temporal capture.
        const auto version = state.Version();
        const auto epoch = state.Epoch();
        return Stamp {id, image, address, 0x20000, version, epoch};
    };
    const auto depth = [&] { return current(0x100000003, 0x11, 0x200000, depth_state); };
    const auto motion = [&] { return current(0x100000004, 0x22, 0x240000, motion_state); };
    auto saved_depth = depth(), saved_motion = motion();

    // Fresh images have dirty guest contents before their first target binding.
    // Refreshing both attachments must not invalidate the first camera that
    // will render into them. The old pre-acquisition snapshot fails this case.
    depth_state.NoteRefresh();
    motion_state.NoteRefresh();
    check(!Matches(saved_depth, depth()), "old first-witness ordering reproduces rejection");
    check(depth().epoch == saved_depth.epoch && depth().version != saved_depth.version,
          "attachment refresh advances version only");
    check(AfterAcquire(true, true, false, saved_depth, saved_motion, depth(), motion()) == Finalize::CaptureFirst,
          "fresh dirty pair finalizes after attachment initialization");
    saved_depth = depth(); saved_motion = motion();
    check(Matches(saved_depth, depth()) && Matches(saved_motion, motion()), "committed first pair captures");
    check(AfterAcquire(false, true, false, saved_depth, saved_motion, depth(), motion()) == Finalize::Retain,
          "existing clean witness is retained");

    // An existing witness may never absorb even a completion-only version.
    depth_state.NoteRefresh();
    check(AfterAcquire(false, true, false, saved_depth, saved_motion, depth(), motion()) == Finalize::Reject,
          "existing witness rejects upload completion");
    check(!Matches(saved_depth, depth()), "upload before temporal capture rejects");
    saved_depth = depth(); saved_motion = motion();
    depth_state.NoteWrite();
    depth_state.NoteRefresh();
    check(AfterAcquire(false, true, false, saved_depth, saved_motion, depth(), motion()) == Finalize::Reject,
          "existing witness rejects external dirty plus acquisition");
    check(AfterAcquire(true, true, false, saved_depth, saved_motion, depth(), motion()) == Finalize::Reject,
          "pending first witness rejects new external write plus acquisition");
    saved_depth = depth(); saved_motion = motion();
    motion_state.NoteWrite();
    motion_state.NoteWrite();
    motion_state.NoteRefresh();
    check(motion().epoch == saved_motion.epoch + 2 && motion().version == saved_motion.version + 3,
          "repeated external writes are not coalesced into completion");
    check(AfterAcquire(true, true, false, saved_depth, saved_motion, depth(), motion()) == Finalize::Reject,
          "pending first witness rejects changed motion epoch");
    saved_depth = depth(); saved_motion = motion();
    check(AfterAcquire(true, false, false, saved_depth, saved_motion, depth(), motion()) == Finalize::Reject,
          "invalid pending candidate cannot revive");
    check(AfterAcquire(true, true, true, saved_depth, saved_motion, depth(), motion()) == Finalize::Reject,
          "write guard rejection cannot revive");

    for (unsigned field = 0; field != 4; ++field) {
        auto replaced = depth();
        switch (field) {
            case 0: replaced.image_id += uint64_t{1} << 32; break;
            case 1: replaced.native_image += 1; break;
            case 2: replaced.address += 0x1000; break;
            case 3: replaced.size += 0x1000; break;
        }
        check(AfterAcquire(true, true, false, saved_depth, saved_motion, replaced, motion()) == Finalize::Reject,
              "first finalize rejects changed image generation, VkImage, or range");
        check(!Matches(saved_depth, replaced), "alias replacement before temporal capture rejects");
    }
    auto invalid = depth();
    invalid.native_image = 0;
    check(!Matches(invalid, invalid), "missing native image cannot witness");
    invalid = depth(); invalid.address = std::numeric_limits<uint64_t>::max();
    check(!Matches(invalid, invalid), "overflowing guest range cannot witness");

    // Even if a write's ordinary version has not yet been observed, its epoch
    // alone invalidates a retained capture (no image-content synchronization is
    // inferred from either counter).
    auto epoch_only = depth(); ++epoch_only.epoch;
    check(!Matches(saved_depth, epoch_only), "capture detects notification before version advance");
    std::printf("GT7 native attachment witness: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
