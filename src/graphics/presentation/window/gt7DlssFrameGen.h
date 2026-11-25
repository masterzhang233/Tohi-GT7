#pragma once

#include "graphics/presentation/window/gt7FrameGen.h"

#include <cstdint>
#include <vector>

struct SDL_Window;

namespace Libs::Graphics::Gt7DlssFrameGen {

enum class StatusReason : uint8_t {
    Off, Loading, BuildUnsupported, RuntimeMissing, RuntimeUntrusted, Unsupported,
    WaitingSwapchain, WaitingInputs, InvalidInputs, SceneUnverified, CameraUnverified,
    HudlessUnverified, DuplicateFrame, HistoryReset, WaitingPresent, Active,
    DroppedFrame, RuntimeError, Fixed2xUnsupported, SuperResolutionUnavailable,
};

struct StatusSnapshot {
    StatusReason reason = StatusReason::Off;
    // Counts reported by slDLSSGGetState, not requested multipliers or monitor FPS.
    uint64_t sdk_presented_frames = 0;
    uint64_t application_presents = 0;
    // Conservative cumulative excess over application presents. SDK reporting
    // can lag the CPU; this may undercount but cannot assert a requested frame.
    uint64_t generated_frames = 0;
    uint32_t sdk_status = 0;
    uint32_t last_presented_count = 0;
    // Runtime capability only; requested generation remains exactly one.
    uint32_t max_generated_frames = 0;
};

// Explicit opt-in for both legacy and NativePSSR SR. Each frame must still pass
// the same real camera/depth/motion/HUD-less contract before fixed 2x is enabled.
[[nodiscard]] bool Requested();
// Call before creating ANY Vulkan instance. On initialization failure returns
// real_proc_addr. Keep the returned dispatch installed for the device lifetime.
[[nodiscard]] PFN_vkGetInstanceProcAddr Initialize(PFN_vkGetInstanceProcAddr real_proc_addr);
// SDL owns the native window, but its Vulkan loader bypasses the interposer.
// Native FG requires surface creation through SL to register the surface/HWND
// association. Falls back to SDL's ordinary surface creation when SL is off.
[[nodiscard]] bool CreateSurface(SDL_Window* window, vk::Instance instance, vk::SurfaceKHR* surface);
// The interposer's vkCreateInstance/vkCreateDevice add these requirements and
// private queues automatically. These helpers are informational/optional.
void AppendInstanceExtensions(std::vector<const char*>& extensions);
void AppendDeviceExtensions(std::vector<const char*>& extensions);
[[nodiscard]] bool OnDevice(GraphicContext& graphics, uint32_t graphics_queue_index = 0);
[[nodiscard]] bool DeviceReady();
[[nodiscard]] bool IsGenerating();
[[nodiscard]] StatusSnapshot GetStatus();
[[nodiscard]] const char* StatusReasonText(StatusReason reason);

// Once after each successful proxy swapchain create/recreate, before BeginFrame.
// LL2 options set before swapchain creation are not applied by Streamline.
// Caller must not hold the renderer or graphics queue mutex; this does not sleep.
void OnSwapchainCreated();

// Exactly once after a successful acquire and before recording that present.
// Reflex can block: caller must NOT hold the renderer or graphics queue mutex.
// A failed acquire must not start a frame; a present retry starts a new frame.
void BeginFrame();

// Present-thread API; caller must NOT hold graphics.queue_mutex. The command
// buffer must follow ready input copies. Input images arrive/leave shader-read
// layout. HUD-less color is display-sized, same format/conversion as backbuffer.
// The backbuffer arrives in PRESENT_SRC_KHR. Real camera/depth/motion/HUD-less
// are required; unavailable input suspends generation, never supplies fake data.
// eOnlyValidNow causes Streamline-owned copies on this command buffer so input
// reuse on the render queue is safe after the present submission has retired.
// BeginFrame must have supplied this present's token before entering renderer locks.
void RecordPrepare(GraphicContext& graphics, vk::CommandBuffer command,
                   const Gt7FrameGen::FrameInputs* inputs,
                   vk::Image backbuffer, vk::ImageView backbuffer_view,
                   vk::Extent2D display, vk::Format format, vk::Rect2D region,
                   const VulkanImage* hudless = nullptr);
void BeforePresent();
void AfterPresent(vk::Result result);
void NoteDroppedFrame();

// Stop submissions before calling. Drains device work and releases cached
// input views BEFORE presenter input images are destroyed/reallocated.
void WaitForPresents(GraphicContext& graphics);
void OnSwapchainDestroyed();
// Call after draining/destroying presentation, before destroying Vulkan device.
// The interposer DLL remains loaded until process exit: Vulkan dispatch still
// references it while vkDestroyDevice and vkDestroyInstance run.
void Shutdown();

} // namespace Libs::Graphics::Gt7DlssFrameGen
