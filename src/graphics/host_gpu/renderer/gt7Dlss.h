#pragma once

#include <cstdint>
#include <memory>
#include <span>

namespace Libs::Graphics {
class RenderContext;
class CommandBuffer;
class Image;
struct PreparedBindings;
struct RenderColorInfo;
struct RenderDepthInfo;
struct ShaderStageRuntime;
struct ShaderVertexInputInfo;
struct ShaderComputeInputInfo;
struct TextureBinding;
struct GuestRange;
namespace Gt7FrameGen { struct FrameInputs; }
namespace Gt7FrameGenCamera { struct RasterState; }
namespace Pssr { struct Request; }

// Experimental, opt-in GT7 1080p -> 1440p reconstruction. The owner must finish
// all GPU work before destruction. No guest pixels are read back by this helper.
class Gt7Dlss {
public:
    explicit Gt7Dlss(RenderContext& context);
    ~Gt7Dlss();
    Gt7Dlss(const Gt7Dlss&) = delete;
    Gt7Dlss& operator=(const Gt7Dlss&) = delete;
    // Native SR owns reconstruction; this path only freezes verified FG inputs.
    void BeginNativePssr();
    void RecordNativePssr(CommandBuffer& buffer, const Pssr::Request& request,
                          const TextureBinding& depth, const TextureBinding& motion,
                          const TextureBinding& output, bool reset_used);
    bool BeginTaa(CommandBuffer& buffer, const PreparedBindings& pixel,
                  std::span<const RenderColorInfo> colors);
    [[nodiscard]] bool Active() const;
    [[nodiscard]] bool NativeFrameGenerationInputs() const;
    [[nodiscard]] bool WatchesFrameGenerationInputs() const;
    [[nodiscard]] bool MatchesFrameGenerationInput(const RenderColorInfo& target) const;
    [[nodiscard]] bool MatchesScene(const RenderColorInfo& color) const;
    // Includes metadata and cached aliases, so an unrelated clear may proceed
    // without accepting a write that changes a watched scene indirectly.
    [[nodiscard]] bool MatchesWatchedWriteRange(GuestRange range) const;
    [[nodiscard]] bool PreservesFrameGenerationMetadata(const ShaderComputeInputInfo& input,
        const PreparedBindings& bindings, uint32_t group_x, uint32_t group_y,
        uint32_t group_z, uint32_t mode);
    [[nodiscard]] Image* EffectLayer();
    bool Composite(CommandBuffer& buffer, const TextureBinding& low_final,
                   const RenderColorInfo& high_target, uint32_t effect_count);
    void CancelFrame(const char* reason, bool invalidate_history = false);
    // Cancel only the in-progress reconstruction. Completed, independently
    // copied outputs remain watched until their own flip or a proven write.
    void CancelLiveFrame(const char* reason, bool invalidate_history = false);
    void CancelFrameForWrite(GuestRange range, const char* reason);
    void CancelFrameForWrite(const RenderColorInfo& target, const char* reason);
    // Same cancellation, with bounded owned diagnostics for the actual writer.
    // Details are reported only by the existing rate-limited scene gate.
    void CancelSceneWriterFrame(const char* reason, uint64_t vertex_shader,
                                uint64_t pixel_shader, const char* details);
    // GPU recording thread only. Associate a verified ordinary motion producer's
    // camera with its depth/MV targets, then snapshot at the actual guest flip.
    // Accept the original independently verified producers and programs whose
    // final position calculation has been proved by the shader compiler.
    [[nodiscard]] static bool IsFrameGenerationCameraShader(const ShaderStageRuntime& vertex);
    bool ObserveFrameGenerationCamera(const ShaderStageRuntime& vertex,
                                      const RenderDepthInfo& depth,
                                      std::span<const RenderColorInfo> colors,
                                      const Gt7FrameGenCamera::RasterState& raster,
                                      const ShaderStageRuntime* trusted_depth_vertex = nullptr);
    // Commit a first witness only after attachment initialization; existing witnesses
    // are validated without rebasing their external-write versions.
    bool FinalizeFrameGenerationCamera(const RenderDepthInfo& depth,
                                        std::span<const RenderColorInfo> colors);
    // Called after attachment acquisition, with the actual depth load clear.
    // Native PSSR owns the final input pair; ordered exact attachment writes
    // need not each establish a camera. A rejected association cannot be
    // repaired by a later camera observation in the same scene.
    struct NativeInputDrawRejection {
        // Bits 0..5: depth unverified/id/clear; color unverified/slot/id. Bit 6: depth mapping.
        uint32_t reason_mask = 0, color_touch_mask = 0, depth_touch_relation = 0;
        // Relations: exact depth/MV ID, depth/MV byte overlap, cache alias/unknown.
        uint32_t color_touch_relation[8] {};
        uint64_t frame = 0, scene = 0, depth_id = 0, motion_id = 0;
        uint64_t depth_address = 0, depth_size = 0, motion_address = 0, motion_size = 0;
        float saved_viewport_min_depth = 0.0f, saved_viewport_max_depth = 0.0f;
        const char* depth_projection_status = "not-attempted";
    };
    [[nodiscard]] NativeInputDrawRejection CheckNativeFrameGenerationInputDraw(const RenderDepthInfo& depth,
        std::span<const RenderColorInfo> colors, bool verified_camera, bool clear_depth,
        float clear_value, float viewport_min_depth, float viewport_max_depth, bool viewport_depths_match,
        const ShaderStageRuntime* trusted_depth_vertex = nullptr);
    // Bounded diagnostics only; never accepts an additional producer shader.
    void DescribeFrameGenerationDraw(const ShaderStageRuntime& vertex, uint64_t pixel_shader,
                                     const RenderDepthInfo& depth,
                                     std::span<const RenderColorInfo> colors,
                                     bool full_viewport, bool storage_writes,
                                     const Gt7FrameGenCamera::RasterState& raster);
    void NoteFrameGenerationSceneTarget(const RenderColorInfo& target);
    [[nodiscard]] bool FrameGenerationTonemapMatches(const ShaderVertexInputInfo& vertex,
                                                     const PreparedBindings& pixel,
                                                     const RenderColorInfo& target);
    void CaptureFrameGenerationHudless(CommandBuffer& buffer, const RenderColorInfo& target);
    [[nodiscard]] bool WatchesFrameGenerationOutput() const;
    [[nodiscard]] bool MatchesFrameGenerationOutput(const RenderColorInfo& target) const;
    // A draw-local ticket for the existing bounded first-SR-writer report.
    // Attachment and raster context is appended after its actual state is resolved.
    struct OutputDrawDiagnostic {
        bool report = false;
        uint32_t target_slot = 0;
        uint64_t frame = 0, scene = 0;
    };
    void CheckFrameGenerationOutputDraw(std::span<const RenderColorInfo> targets,
                                        const PreparedBindings* pixel, uint64_t vertex_shader,
                                        uint64_t pixel_shader, bool ordinary_alpha_overlay,
                                        bool proven_opaque_overlay,
                                        OutputDrawDiagnostic* diagnostic = nullptr);
    // Call before FrameBoundary, after waiting for this frame's prior present.
    // Allocations belong to the caller and must outlive its GPU present work.
    bool CopyFrameGenerationInputs(CommandBuffer& buffer, Gt7FrameGen::FrameInputs& inputs,
                                    const Image& presented_source);
    void FrameBoundary(bool discard_completed = false);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
