#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DLSS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DLSS_H_
#include "graphics/host_gpu/vulkanCommon.h"
#include "../../../../tools/dlss_backend/kyty_dlss_pssr.h"
#include <optional>

namespace Libs::Graphics {
struct GraphicContext;
namespace Pssr { struct Request; }
namespace Dlss {
struct Feature;
// Session availability only: Ready does not prove an evaluation or displayed frame.
// Safe to query from the presenter without touching the renderer-owned session.
enum class SessionState : uint8_t { NotStarted, Ready, Failed };
[[nodiscard]] SessionState GetSessionState();
// A typed borrowed capability interface, not a second raw NGX client.
struct Parameter { const KytyDlssPssrApi* api; KytyDlssPssrSession* session; };
struct Resource {
    VkImageView view;
    VkImage image;
    VkImageSubresourceRange range;
    VkFormat format;
    uint32_t width, height;
    int32_t type = 0;
    uint8_t read_write = 0;
};
struct Inputs { Resource color, depth, motion; Resource* exposure; Resource output; };
struct Creation {
    uint32_t output_width, output_height, flags;
    int32_t mode;
    uint32_t min_width, min_height, max_width, max_height;
    bool operator==(const Creation&) const = default;
};
[[nodiscard]] bool Accepts(const Pssr::Request& request);
[[nodiscard]] bool Takes(const Creation&,const Pssr::Request&,const Inputs&);
[[nodiscard]] std::optional<Creation> Choose(Parameter&,const Pssr::Request&,const Inputs&);
// Exactly one Init/Shutdown pair per RenderExecutor. Device initialization is
// shared; features must be released after GPU completion before last Shutdown.
void Init(const GraphicContext&,PFN_vkGetInstanceProcAddr,PFN_vkGetDeviceProcAddr);
void Shutdown();
[[nodiscard]] Parameter* Capabilities();
[[nodiscard]] Feature* CreateFeature(vk::CommandBuffer,const Creation&);
// False is safe pre-record validation failure only. Any potentially partial NGX
// command failure terminates before the shared command buffer can be submitted.
[[nodiscard]] bool Evaluate(vk::CommandBuffer,const Feature*,const Pssr::Request&,Inputs&,bool reset);
void ReleaseFeature(Feature*);
}
}
#endif
