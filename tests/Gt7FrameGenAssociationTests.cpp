#include "graphics/host_gpu/renderer/gt7FrameGenAssociation.h"

#include <array>
#include <cstdio>
#include <limits>

using namespace Libs::Graphics::Gt7FrameGenAssociation;

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
}

OutputIdentity Output(uint64_t n) {
    return {(1ull << 32) | n, 0x1000 + n, 0x100000 + n * 0x10000, 0x8000, 2560, 1440, 44};
}

// Stand-in ownership witnesses: the pure registry must select the slot whose
// copied scene/depth/MV/camera came from this output, not the latest guest flip.
struct CompletedFrame {
    uint64_t scene = 0;
    uint64_t depth = 0;
    uint64_t motion = 0;
    uint64_t camera = 0;
};

struct Replay {
    CandidateState states;
    std::array<CompletedFrame, CandidateState::Capacity> copied {};

    std::optional<size_t> Complete(const OutputIdentity& output, uint64_t scene, bool verified = true) {
        const auto slot = states.Reserve(output, scene);
        if (slot) {
            copied[*slot] = {scene, scene * 10 + 1, scene * 10 + 2, scene * 10 + 3};
            states.Commit(*slot, verified);
        }
        return slot;
    }

    void Flip(const OutputIdentity& output, uint64_t expected_scene) {
        const auto slot = states.Find(output);
        Check(slot.has_value(), "completed output can be selected");
        if (!slot) return;
        const auto& frame = copied[*slot];
        Check(states.Get(*slot).scene_serial == expected_scene && frame.scene == expected_scene &&
              frame.depth == expected_scene * 10 + 1 && frame.motion == expected_scene * 10 + 2 &&
              frame.camera == expected_scene * 10 + 3, "flip retains its own temporal resource bundle");
        Check(states.Consume(*slot), "selected output consumed once");
        Check(!states.Consume(*slot) && !states.Find(output), "repeat flip cannot reuse consumed inputs");
    }
};

void BasicAndInterleaved() {
    const auto a = Output(1), b = Output(2);
    Replay ordinary;
    ordinary.Complete(a, 10);
    ordinary.Flip(a, 10);
    ordinary.Complete(b, 11);
    ordinary.Flip(b, 11);

    Replay overlapped;
    // Observed scheduling: tone map + UI for A, begin/complete B, CPU flips A,
    // then flips B. B must not overwrite A's privately captured input bundle.
    const auto sa = overlapped.Complete(a, 20);
    const auto sb = overlapped.Complete(b, 21);
    Check(sa && sb && *sa != *sb, "independent pending outputs get separate resource slots");
    overlapped.Flip(a, 20);
    overlapped.Flip(b, 21);

    Replay incomplete_b;
    incomplete_b.Complete(a, 30);
    const auto pending = incomplete_b.states.Reserve(b, 31);
    incomplete_b.Flip(a, 30);
    Check(pending && !incomplete_b.states.Find(b), "starting a new scene neither loses A nor exposes incomplete B");

    Replay reordered;
    reordered.Complete(a, 40);
    reordered.Complete(b, 41);
    reordered.Flip(b, 41);
    reordered.Flip(a, 40);
    // Serial 41 then 40 is deliberately retained, so the integration's history
    // reset check can see the discontinuity instead of receiving flip ordinals.
    reordered.Complete(a, 45);
    reordered.Flip(a, 45);
}

void WriterInvalidation() {
    const auto a = Output(1), b = Output(2);
    Replay replay;
    const auto sa = replay.Complete(a, 1);
    replay.Complete(b, 2);
    replay.states.Invalidate(a);
    Check(!replay.states.Find(a), "unknown UI writer invalidates its target");
    Check(sa && !replay.states.Commit(*sa, true), "invalidated reservation cannot be recommitted");
    Check(sa && replay.states.Get(*sa).output == a && replay.states.Get(*sa).scene_serial == 1,
          "invalidated output retains identity for old-flip rejection");
    replay.Flip(b, 2);

    replay.Complete(a, 3);
    replay.Complete(b, 4);
    replay.states.InvalidateRange(a.address + a.size - 1, 1);
    Check(!replay.states.Find(a) && replay.states.Find(b), "one-byte overlap invalidates only the matching guest range");
    replay.Complete(a, 5);
    replay.states.InvalidateRange(a.address + a.size, 1);
    Check(replay.states.Find(a).has_value(), "adjacent non-overlapping byte does not invalidate output");

    auto alias = Output(9);
    alias.backing = a.backing;
    replay.states.Invalidate(alias);
    Check(!replay.states.Find(a) && replay.states.Find(b), "backing alias invalidates A without losing B");
    replay.Complete(a, 6);
    alias = Output(9);
    alias.image_id = a.image_id;
    replay.states.Invalidate(alias);
    Check(!replay.states.Find(a) && replay.states.Find(b), "image identity catches writes outside the recorded address range");

    const auto sb = replay.states.Find(b);
    if (sb) replay.states.InvalidateSlot(*sb);
    Check(!replay.states.Find(b), "external texture-cache alias proof can invalidate one slot");
    replay.Complete(a, 7);
    replay.Complete(b, 8);
    replay.states.InvalidateAll();
    Check(!replay.states.Find(a) && !replay.states.Find(b), "unbounded writer invalidates every pending output");
}

void IdentityAndCompleteness() {
    const auto a = Output(1);
    Replay replay;
    replay.Complete(a, 10);
    const auto mismatch = [&](auto change, const char* name) {
        auto changed = a;
        change(changed);
        Check(!replay.states.Find(changed), name);
        Check(replay.states.Find(a).has_value(), "failed match does not alter pending output");
    };
    mismatch([](auto& v) { v.image_id += 1ull << 32; }, "reused image index with new generation cannot match stale inputs");
    mismatch([](auto& v) { ++v.backing; }, "new backing handle rejected");
    mismatch([](auto& v) { ++v.address; }, "different output address rejected");
    mismatch([](auto& v) { --v.size; }, "different extent in guest memory rejected");
    mismatch([](auto& v) { --v.width; }, "different output width rejected");
    mismatch([](auto& v) { --v.height; }, "different output height rejected");
    mismatch([](auto& v) { ++v.format; }, "different output format rejected");

    auto replacement = a;
    replacement.image_id += 1ull << 32;
    replacement.backing += 100;
    replay.Complete(replacement, 11);
    Check(!replay.states.Find(a), "new generation at the same guest address supersedes old candidate");
    replay.Flip(replacement, 11);

    const auto partial = replay.states.Reserve(a, 12);
    Check(partial && !replay.states.Find(a), "reservation is not presentation-ready");
    if (partial) Check(!replay.states.Commit(*partial, false) && !replay.states.Consume(*partial),
                       "incomplete or multi-TAA scene is never eligible");
    if (partial) Check(!replay.states.Commit(*partial, true), "failed completeness cannot be revived without a new capture");
    Check(!replay.states.Find(a), "failed completeness proof is not a match");
    Check(!replay.states.Reserve(a, 0), "missing scene serial rejected");
    Check(!replay.states.Commit(CandidateState::Capacity, true) &&
          !replay.states.Consume(CandidateState::Capacity), "invalid slots cannot become ready or consumed");
}

void CapacityAndRangeSafety() {
    CandidateState in_progress;
    for (size_t i = 0; i < CandidateState::Capacity; ++i)
        Check(in_progress.Reserve(Output(i + 1), i + 1).has_value(), "in-progress reservation has its own slot");
    Check(!in_progress.Reserve(Output(9), 9), "unrelated reservation never evicts an unfinished capture");

    Replay replay;
    for (size_t i = 0; i < CandidateState::Capacity; ++i)
        Check(replay.Complete(Output(i + 1), i + 1).has_value(), "pending output fits within bounded storage");
    Check(!replay.Complete(Output(9), 9), "full registry never evicts an unrelated unflipped output");
    for (size_t i = 0; i < CandidateState::Capacity; ++i)
        Check(replay.states.Find(Output(i + 1)).has_value(), "capacity miss preserves all pending outputs");

    replay.Flip(Output(2), 2);
    Check(replay.Complete(Output(9), 9).has_value(), "consumed resource slot can be reused");
    replay.Flip(Output(9), 9);
    const auto old = replay.states.Find(Output(1));
    Check(old.has_value(), "original output remains pending");
    Check(replay.Complete(Output(1), 10).has_value(), "same-output rewrite can reuse its own slot at capacity");
    replay.Flip(Output(1), 10);
    replay.Flip(Output(3), 3);
    replay.Flip(Output(4), 4);

    replay.Complete(Output(1), 11);
    replay.Complete(Output(2), 12);
    replay.states.InvalidateRange(0, 0);
    Check(!replay.states.Find(Output(1)) && !replay.states.Find(Output(2)), "unknown write range fails closed");
    replay.Complete(Output(1), 13);
    replay.states.InvalidateRange(std::numeric_limits<uint64_t>::max() - 2, 8);
    Check(!replay.states.Find(Output(1)), "wrapped write range fails closed");
    auto invalid = Output(7);
    invalid.size = 0;
    Check(!replay.states.Reserve(invalid, 14), "empty output range rejected");
    invalid = Output(7); invalid.backing = 0;
    Check(!replay.states.Reserve(invalid, 14), "missing GPU backing rejected");
}
} // namespace

int main() {
    BasicAndInterleaved();
    WriterInvalidation();
    IdentityAndCompleteness();
    CapacityAndRangeSafety();
    std::printf("GT7 frame-generation output association: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
