#pragma once

#include <cstdint>

namespace Libs::Graphics::Gt7FrameGenLifecycle {

// The tone-map has consumed the original SR source when a scene closes. The
// completed candidate owns private color/depth/MV copies; its display-output
// watches are separate and must survive retirement of this scratch source.
// Keep ready itself intact: it also records whether the native sequence ended
// successfully, which the next scene uses to preserve/reset history.
[[nodiscard]] constexpr bool WatchesNativeSource(bool ready, bool closed) {
    return ready && !closed;
}

enum ProducerReset : uint32_t {
    SuperResolutionReset = 1u << 0,
    PriorIncomplete = 1u << 1,
    ContextChanged = 1u << 2,
    NonpositiveInterval = 1u << 3,
    LongInterval = 1u << 4,
};

// These masks describe the existing reset policy; they add no acceptance or
// reset conditions. In particular, preserve the original float comparisons.
[[nodiscard]] constexpr uint32_t ProducerResetReasons(bool sr_reset, bool prior_reset,
                                                      bool context_changed, float interval_ms) {
    return (sr_reset ? SuperResolutionReset : 0u) |
           (prior_reset ? PriorIncomplete : 0u) |
           (context_changed ? ContextChanged : 0u) |
           (interval_ms <= 0.0f ? NonpositiveInterval : 0u) |
           (interval_ms > 100.0f ? LongInterval : 0u);
}

enum PendingSource : uint32_t {
    PendingInitial = 1u << 0,
    PendingUnfinished = 1u << 1,
    PendingSceneBoundary = 1u << 2,
    PendingCancelled = 1u << 3,
};

enum ConsumerReset : uint32_t {
    Discontinuity = 1u << 0,
    InputReset = 1u << 1,
    NoPrevious = 1u << 2,
    SceneGap = 1u << 3,
    RenderChanged = 1u << 4,
    DisplayChanged = 1u << 5,
};

[[nodiscard]] constexpr uint32_t ConsumerResetReasons(bool discontinuity, bool input_reset,
    bool previous_valid, uint64_t scene, uint64_t previous_scene,
    bool render_changed, bool display_changed) {
    return (discontinuity ? Discontinuity : 0u) |
           (input_reset ? InputReset : 0u) |
           (!previous_valid ? NoPrevious : 0u) |
           (scene != previous_scene + 1 ? SceneGap : 0u) |
           (render_changed ? RenderChanged : 0u) |
           (display_changed ? DisplayChanged : 0u);
}

enum DiscontinuitySource : uint32_t {
    Initial = 1u << 0,
    RuntimeFailure = 1u << 1,
    Suspended = 1u << 2,
    PresentFailure = 1u << 3,
    Dropped = 1u << 4,
    Drained = 1u << 5,
    SwapchainDestroyed = 1u << 6,
};

// Both masks have at most six bits. Print a new combination once and then at
// most every five seconds, including mask zero so stable history is visible.
// The recording lane / presenter mutex owns its respective instance.
struct ResetDiagnostics {
    uint64_t seen = 0;
    int64_t next_report_ms = 0;

    [[nodiscard]] bool Report(uint32_t mask, int64_t now_ms) {
        if (mask >= 64) return false;
        const uint64_t bit = uint64_t {1} << mask;
        const bool first = !(seen & bit);
        seen |= bit;
        if (!first && now_ms < next_report_ms) return false;
        next_report_ms = now_ms + 5000;
        return true;
    }
};

} // namespace Libs::Graphics::Gt7FrameGenLifecycle
