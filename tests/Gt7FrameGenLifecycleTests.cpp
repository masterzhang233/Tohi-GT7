#include "graphics/host_gpu/renderer/gt7FrameGenLifecycle.h"
#include "graphics/host_gpu/renderer/gt7FrameGenAssociation.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
using namespace Libs::Graphics::Gt7FrameGenLifecycle;
namespace Association = Libs::Graphics::Gt7FrameGenAssociation;
int checks = 0;
void Require(bool value, const char* description) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
}

// CPU lifecycle fixture. Overlap inputs stand for the existing cache's byte,
// identity and metadata decisions; this test does not approximate that cache.
// Completed-output ownership uses the actual production association class.
struct Scene {
    bool ready = true, closed = false, pending = true, live_display = false;
    bool camera_valid = true, reset = true;
    uint64_t serial = 1;
    unsigned cancellations = 0;
    Association::CandidateState completed;
    Association::OutputIdentity output {2, 20, 0x2000, 0x1000, 3840, 2160, 64};

    bool WatchesSource() const { return WatchesNativeSource(ready, closed); }

    void CancelLive() {
        live_display = false;
        ready = false;
        pending = true;
        camera_valid = false;
        ++cancellations;
    }

    void Freeze(bool copies_complete = true) {
        closed = true; // The original source has already been consumed.
        live_display = true;
        const auto slot = completed.Reserve(output, serial);
        Require(slot.has_value(), "fixture can reserve completed output");
        Require(completed.Commit(*slot, copies_complete) == copies_complete,
                "production association reflects private-copy completion");
        if (copies_complete) pending = false;
    }

    void BoundedWrite(bool touches_source, bool touches_display,
                      bool touches_current_depth_or_motion = false) {
        if (touches_current_depth_or_motion) camera_valid = false;
        // Source retirement never short-circuits completed-output invalidation.
        if (touches_display) completed.InvalidateRange(output.address, output.size);
        if ((WatchesSource() && touches_source) || (live_display && touches_display)) CancelLive();
    }

    void UnknownWrite() { completed.InvalidateAll(); CancelLive(); }

    void NextCamera() {
        if (!ready) pending = true; // ResetLiveScene preserves failure history.
        ready = false;
        closed = live_display = false;
        camera_valid = true;
        ++serial;
    }

    void NativePass(bool success = true) {
        const bool prior = pending;
        pending = true; // BeginNativePssr has not frozen this new scene yet.
        ready = false;
        if (!success || !camera_valid) return;
        reset = ProducerResetReasons(false, prior, false, 16.0f) != 0;
        ready = true;
    }
};

void Lifecycle() {
    Scene complete;
    complete.Freeze();
    Require(complete.ready && !complete.WatchesSource(), "freeze retires watch without destroying successful ready state");
    for (unsigned kind = 0; kind < 3; ++kind) { // direct bytes, exact ID, metadata-only alias hit
        complete.BoundedWrite(true, false);
        Require(!complete.pending && complete.cancellations == 0,
                "closed source-only byte/ID/metadata reuse cannot set next-frame reset");
        Require(complete.completed.Find(complete.output).has_value(), "private completed candidate survives scratch reuse");
    }
    complete.NextCamera();
    Require(!complete.WatchesSource() && !complete.pending, "new camera keeps success history but has no premature source watch");
    complete.NativePass();
    Require(complete.WatchesSource() && !complete.reset, "next successful native pass resumes source protection without a false reset");

    for (unsigned kind = 0; kind < 3; ++kind) {
        Scene open;
        open.BoundedWrite(true, false);
        Require(open.cancellations == 1 && open.pending && !open.ready,
                "before freeze source byte/ID/metadata writes still invalidate the live sequence");
        open.NextCamera();
        open.NativePass();
        Require(open.reset, "a failed preceding scene still resets recovered history");
    }

    Scene display;
    display.Freeze();
    display.BoundedWrite(true, true); // Old SR alias also reaches final display.
    Require(!display.completed.Find(display.output) && display.pending && display.cancellations == 1,
            "source retirement cannot hide an overlapping final-display write");

    Scene archived;
    archived.Freeze();
    archived.NextCamera();
    archived.BoundedWrite(true, true);
    Require(!archived.completed.Find(archived.output) && archived.cancellations == 0,
            "archived display invalidation is independent of a newer live scene");

    Scene next_input;
    next_input.Freeze();
    next_input.NextCamera();
    next_input.BoundedWrite(true, false, true);
    Require(!next_input.camera_valid, "retired source alias still invalidates a new depth/MV witness");
    next_input.NativePass();
    Require(!next_input.ready && next_input.pending, "invalid new input cannot become an accepted native sequence");

    Scene failed_copy;
    failed_copy.Freeze(false);
    failed_copy.BoundedWrite(true, false);
    Require(!failed_copy.WatchesSource() && failed_copy.pending && !failed_copy.completed.Find(failed_copy.output),
            "failed private copies keep reset pending and never manufacture a completed candidate");
    failed_copy.NextCamera();
    failed_copy.NativePass();
    Require(failed_copy.reset, "failed freeze remains a history break on recovery");

    Scene unbounded;
    unbounded.Freeze();
    unbounded.UnknownWrite();
    Require(unbounded.pending && !unbounded.completed.Find(unbounded.output),
            "unbounded or explicit scene cancellation remains conservative after freeze");

    Scene skipped;
    skipped.NextCamera();
    skipped.NativePass(false);
    skipped.NextCamera();
    skipped.NativePass();
    Require(skipped.reset, "failed native evaluation before freeze retains reset");

    Scene untouched;
    untouched.BoundedWrite(false, false);
    Require(untouched.WatchesSource() && untouched.cancellations == 0, "unrelated bounded writes do not cancel open source");
}

void ResetPolicy() {
    const std::array<float, 8> intervals {0.0f, -1.0f, 0.01f, 16.0f, 100.0f, 100.01f,
        std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
    for (unsigned flags = 0; flags < 8; ++flags) for (float dt : intervals) {
        const bool sr = flags & 1, prior = flags & 2, context = flags & 4;
        const bool original = sr || prior || context || dt <= 0.0f || dt > 100.0f;
        Require((ProducerResetReasons(sr, prior, context, dt) != 0) == original,
                "producer reason mask is equivalent to the previous reset policy including boundary values");
    }
    Require(ProducerResetReasons(true, false, false, 16) == SuperResolutionReset, "SR reset is attributable");
    Require(ProducerResetReasons(false, true, false, 16) == PriorIncomplete, "prior unfinished scene is attributable");
    Require(ProducerResetReasons(false, false, true, 16) == ContextChanged, "context change is attributable");
    Require(ProducerResetReasons(false, false, false, 0) == NonpositiveInterval, "first/invalid interval is attributable");
    Require(ProducerResetReasons(false, false, false, 101) == LongInterval, "long interval is attributable");
    for (unsigned flags = 0; flags < 64; ++flags) {
        const bool discontinuity = flags & 1, input = flags & 2, previous = !(flags & 4);
        const uint64_t guest = flags & 8 ? 42 : 41;
        const bool render = flags & 16, display = flags & 32;
        const bool original = discontinuity || input || !previous || guest != 40 + 1 || render || display;
        const auto mask = ConsumerResetReasons(discontinuity, input, previous, guest, 40, render, display);
        Require(mask == flags && (mask != 0) == original, "all consumer reset combinations preserve the previous policy");
    }
    Require(ConsumerResetReasons(false, false, true, 0, UINT64_MAX, false, false) == 0,
            "diagnostic extraction preserves existing unsigned scene arithmetic");

    // Exactly the atomic operations used by presentation: accumulate concurrent
    // discontinuity sources and consume the nonzero latch once per preparation.
    std::atomic<uint32_t> discontinuity {Initial};
    discontinuity.fetch_or(Dropped);
    discontinuity.fetch_or(RuntimeFailure);
    Require(discontinuity.exchange(0) == (Initial | Dropped | RuntimeFailure), "discontinuity source bits accumulate without losing reset intent");
    Require(discontinuity.exchange(0) == 0, "discontinuity latch is consumed exactly once");
}

void BoundedDiagnostics() {
    ResetDiagnostics reports;
    Require(reports.Report(0, 100), "first stable-history sample is visible");
    Require(!reports.Report(0, 101), "same mask is throttled");
    Require(reports.Report(PriorIncomplete, 102), "first distinct cause is visible");
    Require(!reports.Report(PriorIncomplete, 5101), "five-second bound follows last report");
    Require(reports.Report(PriorIncomplete, 5102), "same cause can report after five seconds");
    Require(!reports.Report(64, 10000), "invalid mask cannot shift outside bounded storage");
    ResetDiagnostics all;
    unsigned emitted = 0;
    for (unsigned repetition = 0; repetition < 10; ++repetition)
        for (uint32_t mask = 0; mask < 64; ++mask) emitted += all.Report(mask, 100);
    Require(emitted == 64, "a burst can emit at most one line per finite cause combination");
}
} // namespace

int main() {
    Lifecycle();
    ResetPolicy();
    BoundedDiagnostics();
    std::printf("GT7 frame generation lifecycle: %d checks passed\n", checks);
}
