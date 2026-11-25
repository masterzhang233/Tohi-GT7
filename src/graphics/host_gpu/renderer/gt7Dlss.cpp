#include "graphics/host_gpu/renderer/gt7Dlss.h"
#include "common/emulatorConfig.h"
#include "graphics/guest_gpu/pssr.h"
#include "graphics/host_gpu/renderer/gt7NativePssrInputs.h"

#include "graphics/host_gpu/gt7DlssDevice.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/frameGraph.h"
#include "graphics/host_gpu/renderer/gt7FrameGenCamera.h"
#include "graphics/host_gpu/renderer/gt7FrameGenInputWitness.h"
#include "graphics/host_gpu/renderer/gt7FrameGenLifecycle.h"
#include "graphics/host_gpu/renderer/gt7NativeAttachmentPolicy.h"
#include "graphics/host_gpu/renderer/gt7NativeDepthProjection.h"
#include "graphics/host_gpu/renderer/gt7NativeCameraPair.h"
#include "graphics/host_gpu/renderer/gt7FrameGenTonemapInputs.h"
#include "graphics/host_gpu/renderer/gt7FrameGenAssociation.h"
#include "graphics/host_gpu/renderer/gt7FrameGenDcc.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/shader.h"
#include "graphics/presentation/window/gt7FrameGen.h"
#include "kernel/memory.h"
#include "gpu_dlss_shaders/gt7_dlss_prepare_spv.h"
#include "gpu_dlss_shaders/gt7_dlss_composite_spv.h"
#include "gpu_dlss_shaders/gt7_native_fg_prepare_spv.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace Libs::Graphics {
namespace {
constexpr uint32_t InputWidth = 1920, InputHeight = 1080;
constexpr uint32_t OutputWidth = 2560, OutputHeight = 1440;
// The game's history allocation and tone-map/UI chain keep their native extent.
// They are independent of the private DLSS reconstruction resolution.
constexpr uint32_t GuestWidth = 3840, GuestHeight = 2160;
constexpr auto SampledLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
constexpr auto ShaderRead = vk::AccessFlagBits2::eShaderRead;
constexpr auto ShaderWrite = vk::AccessFlagBits2::eShaderWrite;

bool Report(uint64_t n) { return n <= 4 || n % 300 == 0; }

bool Shape(const Image& image, uint32_t width, uint32_t height, vk::Format format) {
    const auto& b = image.backing;
    return b.image != nullptr && b.image_type == vk::ImageType::e2D &&
           b.extent.width == width && b.extent.height == height && b.extent.depth == 1 &&
           b.format == format && b.layers == 1 && b.mip_levels == 1 && b.samples == 1;
}

bool BaseView(const TextureCache::ImageDesc& desc) {
    const auto& view = desc.view_info;
    return view.type == vk::ImageViewType::e2D && view.base_level == 0 &&
           view.base_layer == 0 && view.level_count == 1 && view.layer_count == 1;
}

bool Overlap(GuestRange a, GuestRange b) {
    return a.Valid() && b.Valid() && a.address < b.End() && b.address < a.End();
}

vk::ImageView View(Image& image, bool storage = false) {
    ImageViewInfo view {};
    view.type = vk::ImageViewType::e2D;
    view.format = image.backing.format;
    view.aspect = image.info.IsDepth() ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor;
    view.usage = storage ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlagBits::eSampled;
    return image.FindView(view);
}

// Draw hooks run inside GuestImageUses. These accesses are emulator work and
// must never inherit the guest's lean-barrier assumption.
void Transit(Image& image, vk::ImageLayout layout, vk::AccessFlags2 access, vk::CommandBuffer command) {
    image.internal_use = true;
    image.Transit(layout, access, {}, command);
}

KytyDlssImage BackendImage(Image& image, bool storage = false) {
    return {static_cast<VkImage>(image.backing.image), static_cast<VkImageView>(View(image, storage)),
            static_cast<VkFormat>(image.backing.format), image.backing.extent.width, image.backing.extent.height};
}

struct ComputePass {
    vk::DescriptorSetLayout descriptors;
    vk::PipelineLayout layout;
    vk::Pipeline pipeline;
};

Gt7FrameGenInputWitness::Stamp InputWitnessStamp(ImageId id, GuestRange range, vk::Image image,
                                                 uint64_t version, uint64_t epoch) {
    return {uint64_t(id.index) | (uint64_t(id.generation) << 32),
        std::bit_cast<uint64_t>(static_cast<VkImage>(image)), range.address, range.size, version, epoch};
}

Gt7FrameGenInputWitness::Stamp CurrentInputWitness(const Image& image, GuestRange range) {
    // Marks advance epoch before version. Read version first so a concurrent
    // invalidation cannot be accepted as completion of the prepared old state.
    const auto version = image.ExternalWriteVersion();
    const auto epoch = image.ExternalWriteEpoch();
    return InputWitnessStamp(image.self_id, range, image.backing.image, version, epoch);
}

struct FrameGenerationCamera {
    ImageId depth_id {}, motion_id {};
    GuestRange depth_range {}, motion_range {};
    Gt7FrameGenCamera::DepthRange viewport_depth {};
    vk::Image depth_image {}, motion_image {};
    uint64_t depth_external_version = 0, motion_external_version = 0;
    uint64_t depth_external_epoch = 0, motion_external_epoch = 0;
    bool native_witness_pending = false;
    Gt7FrameGenCamera::Matrix view_projection {}, view {}, projection {};
    Gt7FrameGenCamera::Parameters parameters {};
    // Already-read producer values, including a matrix that failed Decode.
    // These diagnostics never stand in for the verified parameters above.
    bool diagnostic_matrices_read = false;
    float diagnostic_projection_jitter_x = 0, diagnostic_projection_jitter_y = 0;
    Gt7FrameGenCamera::DepthRange diagnostic_viewport_depth {};
    bool valid = false, rejected = false;
    const char* reject_reason = nullptr;

    Gt7FrameGenInputWitness::Stamp DepthWitness() const {
        return InputWitnessStamp(depth_id, depth_range, depth_image, depth_external_version, depth_external_epoch);
    }
    Gt7FrameGenInputWitness::Stamp MotionWitness() const {
        return InputWitnessStamp(motion_id, motion_range, motion_image, motion_external_version, motion_external_epoch);
    }
};

// Recording-thread diagnostics only. Keep each frame's last meaningful HUD
// transition without retaining caller-owned strings or allocating per draw.
struct HudlessDiagnostic {
    const char* stage = "no tone-map candidate";
    std::array<char, 192> reason {};
    uint32_t tonemap_calls = 0, capture_attempts = 0;
    bool captured = false;
    uint64_t writer_vs = 0, writer_ps = 0;
    uint32_t writer_mask = 0;
    bool writer_overlay = false;
    size_t writer_targets = 0;

    void Note(const char* where, const char* why) {
        stage = where; // All stage labels below are string literals.
        std::snprintf(reason.data(), reason.size(), "%s", why ? why : "none");
    }
};

// The first operation that invalidates a scene is more useful than a later
// cascade of cancellations. Owned text and fixed storage keep this bounded;
// this state has the same recording-thread lifetime as the scene itself.
struct SceneDiagnostic {
    const char* stage = "none";
    std::array<char, 192> reason {};
    ImageId depth_id {}, motion_id {};
    uint64_t writer_vs = 0;
    uint64_t writer_ps = 0;
    bool scene_writer = false;
    std::array<char, 2048> writer_details {};

    void NoteFirst(const char* where, const char* why, ImageId depth = {},
                   ImageId motion = {}, uint64_t vertex_shader = 0) {
        if (reason[0]) return;
        stage = where; // All stage labels are string literals.
        std::snprintf(reason.data(), reason.size(), "%s", why ? why : "none");
        depth_id = depth;
        motion_id = motion;
        writer_vs = vertex_shader;
    }
};

// Metadata only, on the recording thread. A boundary is the producer's flip
// preparation, not the later asynchronous host present. Keep the two preceding
// boundaries and at most three TAA descriptions per boundary; never read back
// images or retain pointers into caller-owned binding data for this diagnostic.
struct FgAssociationDiagnostics {
    using Clock = std::chrono::steady_clock;
    struct Taa {
        uint64_t serial = 0, at_us = 0, evaluations_before = 0, composites_before = 0;
        // depth, motion, color, history input, history output, scene output.
        std::array<ImageId, 6> images {};
        float jitter_x = 0, jitter_y = 0;
        bool constants_valid = false, evaluated = false, camera_matched = false, reset = false;
    };
    struct Frame {
        uint64_t serial = 0, at_us = 0, evaluations = 0, composites = 0;
        uint64_t snapshot_epoch_us = 0, boundary_epoch_us = 0, presented_address = 0, presented_size = 0;
        uint32_t presented_format = 0;
        std::array<Taa, 3> taa {};
        uint32_t attempts = 0, snapshots = 0, scene_mask = 0, hud_mask = UINT32_MAX;
        uint32_t late_writers = 0, tonemaps = 0;
        bool accepted = false, camera_valid = false, reset = true;
        ImageId presented {}, output {}, scene {};
        const char* stage = "none";
        uint64_t writer_vs = 0;
        uint32_t result = 0;
    } current;

    const bool enabled = [] {
        const auto* value = std::getenv("KYTY_GT7_PERF_DIAGNOSTICS");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    Clock::time_point origin {}, next_report {}, next_incident {};
    uint64_t taa_serial = 0, incident = 0, frames = 0, snapshots = 0, accepted = 0;
    uint64_t boundary_evaluations = 0, boundary_composites = 0;
    uint64_t no_snapshot = 0, two_to_zero = 0, zero_to_two = 0;
    std::array<uint64_t, 4> taa_counts {};
    std::array<uint64_t, 6> scene_bits {};
    std::array<uint64_t, 7> hud_bits {};
    std::array<Frame, 2> previous {};
    uint32_t previous_count = 0, previous_attempts = 0, trailing = 0;
    bool previous_valid = false, seen_accepted = false;

    uint64_t At(Clock::time_point now) {
        if (origin.time_since_epoch().count() == 0) origin = now;
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - origin).count());
    }

    Taa* BeginTaa(const PreparedBindings& pixel, std::span<const RenderColorInfo> colors,
                  uint64_t evaluations, uint64_t composites) {
        if (!enabled) return nullptr;
        ++taa_serial;
        const auto attempt = ++current.attempts;
        if (attempt == 0 || attempt > current.taa.size()) return nullptr;
        auto& item = current.taa[attempt - 1];
        item.serial = taa_serial;
        item.at_us = At(Clock::now());
        item.evaluations_before = evaluations;
        item.composites_before = composites;
        for (size_t i = 0; i < std::min(pixel.images.size(), size_t(4)); ++i)
            item.images[i] = pixel.images[i].image_id;
        for (size_t i = 0; i < std::min(colors.size(), size_t(2)); ++i)
            item.images[4 + i] = colors[i].image_id;
        return &item;
    }

    void Snapshot(const Image& presented, uint32_t scene_mask, uint32_t hud_mask,
                  bool ready, Gt7FrameGen::StatusReason reason) {
        if (!enabled) return;
        ++current.snapshots;
        current.snapshot_epoch_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
        current.presented = presented.self_id;
        current.presented_address = presented.info.data.address;
        current.presented_size = presented.info.data.size;
        current.presented_format = static_cast<uint32_t>(presented.backing.format);
        current.scene_mask = scene_mask;
        current.hud_mask = hud_mask; // UINT32_MAX means the HUD guard was not reached.
        current.accepted = ready;
        current.result = static_cast<uint32_t>(reason);
    }

    void PrintFrame(const Frame& frame, const char* relation) const {
        // A whole frame is emitted once, with a hard upper bound on log size.
        std::array<char, 2304> text {};
        size_t used = 0;
        const auto append = [&](const char* format, auto... args) {
            if (used >= text.size() - 1) return;
            const auto count = std::snprintf(text.data() + used, text.size() - used, format, args...);
            if (count > 0) used += std::min(static_cast<size_t>(count), text.size() - used - 1);
        };
        const auto ull = [](uint64_t n) { return static_cast<unsigned long long>(n); };
        append("GT7 FG association frame: incident=%llu relation=%s frame=%llu at_us=%llu snapshot_epoch_us=%llu boundary_epoch_us=%llu snapshot_requests=%u accepted=%u result=%u taa=%u eval=%llu composite=%llu scene_mask=0x%x hud_mask=0x%x camera=%u reset=%u late=%u tones=%u presented=%u:%u presented_range=0x%llx+0x%llx format=%u output=%u:%u scene=%u:%u stage=%s writer_vs=%016llx",
            ull(incident), relation, ull(frame.serial), ull(frame.at_us), ull(frame.snapshot_epoch_us), ull(frame.boundary_epoch_us), frame.snapshots,
            frame.accepted ? 1u : 0u, frame.result, frame.attempts, ull(frame.evaluations),
            ull(frame.composites), frame.scene_mask, frame.hud_mask, frame.camera_valid ? 1u : 0u,
            frame.reset ? 1u : 0u, frame.late_writers, frame.tonemaps,
            frame.presented.index, frame.presented.generation, ull(frame.presented_address), ull(frame.presented_size), frame.presented_format,
            frame.output.index, frame.output.generation,
            frame.scene.index, frame.scene.generation, frame.stage, ull(frame.writer_vs));
        for (size_t i = 0; i < std::min(static_cast<size_t>(frame.attempts), frame.taa.size()); ++i) {
            const auto& taa = frame.taa[i];
            append(" T%zu={seq:%llu us:%llu pre:%llu/%llu d:%u:%u m:%u:%u c:%u:%u hi:%u:%u ho:%u:%u so:%u:%u j:%.9g,%.9g constants:%u eval:%u cam:%u reset:%u}",
                i + 1, ull(taa.serial), ull(taa.at_us), ull(taa.evaluations_before), ull(taa.composites_before),
                taa.images[0].index, taa.images[0].generation, taa.images[1].index, taa.images[1].generation,
                taa.images[2].index, taa.images[2].generation, taa.images[3].index, taa.images[3].generation,
                taa.images[4].index, taa.images[4].generation, taa.images[5].index, taa.images[5].generation,
                taa.jitter_x, taa.jitter_y, taa.constants_valid ? 1u : 0u, taa.evaluated ? 1u : 0u,
                taa.camera_matched ? 1u : 0u, taa.reset ? 1u : 0u);
        }
        if (frame.attempts > frame.taa.size()) append(" omitted_taa=%u", frame.attempts - static_cast<uint32_t>(frame.taa.size()));
        std::printf("%s\n", text.data());
    }

    void Report(bool final = false) {
        if (!enabled || frames == 0) return;
        const auto now = Clock::now();
        if (!final && now < next_report) return;
        next_report = now + std::chrono::seconds(5);
        const auto ull = [](uint64_t n) { return static_cast<unsigned long long>(n); };
        const auto last_frame = current.serial ? current.serial : previous_count ? previous[previous_count - 1].serial : 0;
        std::printf("GT7 FG association histogram: frame=%llu final=%u boundaries=%llu snapshot_requests=%llu accepted=%llu no_snapshot=%llu taa=[%llu,%llu,%llu,%llu] two_to_zero=%llu zero_to_two=%llu scene_bits=[%llu,%llu,%llu,%llu,%llu,%llu] hud_bits=[%llu,%llu,%llu,%llu,%llu,%llu,%llu]\n",
            ull(last_frame), final ? 1u : 0u, ull(frames), ull(snapshots), ull(accepted), ull(no_snapshot),
            ull(taa_counts[0]), ull(taa_counts[1]), ull(taa_counts[2]), ull(taa_counts[3]),
            ull(two_to_zero), ull(zero_to_two),
            ull(scene_bits[0]), ull(scene_bits[1]), ull(scene_bits[2]), ull(scene_bits[3]), ull(scene_bits[4]), ull(scene_bits[5]),
            ull(hud_bits[0]), ull(hud_bits[1]), ull(hud_bits[2]), ull(hud_bits[3]), ull(hud_bits[4]), ull(hud_bits[5]), ull(hud_bits[6]));
        frames = snapshots = accepted = no_snapshot = two_to_zero = zero_to_two = 0;
        taa_counts.fill(0); scene_bits.fill(0); hud_bits.fill(0);
    }

    void Boundary() {
        if (!enabled) return;
        const auto now = Clock::now();
        current.at_us = At(now);
        current.boundary_epoch_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
        if (next_report.time_since_epoch().count() == 0) next_report = now + std::chrono::seconds(5);
        ++frames;
        snapshots += current.snapshots;
        accepted += current.accepted;
        no_snapshot += current.snapshots == 0;
        ++taa_counts[std::min(current.attempts, 3u)];
        if (previous_valid) {
            two_to_zero += previous_attempts == 2 && current.attempts == 0;
            zero_to_two += previous_attempts == 0 && current.attempts == 2;
        }
        previous_attempts = current.attempts;
        previous_valid = true;
        if (current.snapshots) {
            for (size_t i = 0; i < scene_bits.size(); ++i) scene_bits[i] += (current.scene_mask >> i) & 1u;
            if (current.hud_mask != UINT32_MAX)
                for (size_t i = 0; i < hud_bits.size(); ++i) hud_bits[i] += (current.hud_mask >> i) & 1u;
        }
        if (trailing) {
            PrintFrame(current, "after");
            --trailing;
        } else if (seen_accepted && current.snapshots && !current.accepted && now >= next_incident) {
            ++incident;
            next_incident = now + std::chrono::seconds(5);
            for (uint32_t i = 0; i < previous_count; ++i) PrintFrame(previous[i], "before");
            PrintFrame(current, "failure");
            trailing = 2;
        }
        seen_accepted |= current.accepted;
        if (previous_count < previous.size()) previous[previous_count++] = current;
        else { previous[0] = previous[1]; previous[1] = current; }
        Report();
        current = {};
    }
};

#include "gt7FrameGenCameraProbe.inl"

enum CameraObservation : size_t {
    CameraCalls, CameraDepth, CameraMotion, CameraShape, CameraLate, CameraLimit,
    CameraAlreadyRejected, CameraUser, CameraAddress, CameraUnmapped, CameraGpuDirty,
    CameraRead, CameraMatrix, CameraChanged, CameraAccepted, CameraObservationCount
};

// Fixed storage on the recording thread. A reason gets one immediate report;
// alternating known reasons cannot bypass the five-second session limit.
struct NativeInputDiagnostics {
    using Clock = std::chrono::steady_clock;
    struct Reason {
        std::array<char, 192> text {};
        uint64_t count = 0;
    };
    std::array<Reason, 24> reasons {};
    uint64_t rejected = 0, overflow = 0;
    Clock::time_point next_rejection {}, next_capture {};

    bool Reject(const char* why, Clock::time_point now, uint64_t& reason_count) {
        ++rejected;
        const auto key = std::string_view(why ? why : "none").substr(0, reasons[0].text.size() - 1);
        Reason* slot = nullptr;
        for (auto& reason : reasons) {
            if (reason.count && std::string_view(reason.text.data()) == key) { slot = &reason; break; }
            if (!reason.count && !slot) slot = &reason;
        }
        bool first = false;
        if (slot) {
            first = slot->count == 0;
            if (first) std::copy(key.begin(), key.end(), slot->text.begin());
            reason_count = ++slot->count;
        } else {
            reason_count = ++overflow;
        }
        if (!first && now < next_rejection) return false;
        next_rejection = now + std::chrono::seconds(5);
        return true;
    }

    bool Capture(uint64_t captures, Clock::time_point now) {
        if (captures != 1 && now < next_capture) return false;
        next_capture = now + std::chrono::seconds(5);
        return true;
    }
};

bool ConfigureFrameGenerationImage(GraphicContext& graphics, VulkanImage& image, vk::Format format,
                                    uint32_t width = InputWidth, uint32_t height = InputHeight) {
    const auto usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc |
                       vk::ImageUsageFlagBits::eSampled;
    if (image.image && image.extent == vk::Extent3D {width, height, 1} &&
        image.format == format && image.image_type == vk::ImageType::e2D &&
        image.layers == 1 && image.mip_levels == 1 && image.samples == 1 &&
        (image.usage & usage) == usage) return true;
    // The presenter has retired all uses of the frame before calling us.
    if (image.image) graphics.DeleteImage(image);
    const auto required = vk::FormatFeatureFlagBits::eTransferDst | vk::FormatFeatureFlagBits::eTransferSrc |
                          vk::FormatFeatureFlagBits::eSampledImage;
    if ((graphics.GetFormatProperties(format).optimalTilingFeatures & required) != required) return false;
    vk::ImageCreateInfo create {};
    create.imageType = vk::ImageType::e2D;
    create.format = format;
    create.extent = vk::Extent3D {width, height, 1};
    create.mipLevels = create.arrayLayers = 1;
    create.samples = vk::SampleCountFlagBits::e1;
    create.tiling = vk::ImageTiling::eOptimal;
    create.usage = usage;
    create.sharingMode = vk::SharingMode::eExclusive;
    create.initialLayout = vk::ImageLayout::eUndefined;
    return graphics.CreateImage(create, image);
}

Gt7FrameGenAssociation::OutputIdentity OutputIdentity(const Image& image) {
    return {uint64_t(image.self_id.index) | (uint64_t(image.self_id.generation) << 32),
        std::bit_cast<uint64_t>(static_cast<VkImage>(image.backing.image)),
        image.info.data.address, image.info.data.size, image.backing.extent.width,
        image.backing.extent.height, static_cast<uint32_t>(image.backing.format)};
}

ImageId OutputId(const Gt7FrameGenAssociation::OutputIdentity& output) {
    return {static_cast<uint32_t>(output.image_id), static_cast<uint32_t>(output.image_id >> 32)};
}

// Candidate allocations never change shape during their owner's lifetime.
// In particular, do not delete an image that a prior render submission may use.
bool ConfigureCompletedImage(GraphicContext& graphics, VulkanImage& image, vk::Format format,
                             uint32_t width, uint32_t height) {
    if (!image.image) return ConfigureFrameGenerationImage(graphics, image, format, width, height);
    const auto usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc |
                       vk::ImageUsageFlagBits::eSampled;
    return image.extent == vk::Extent3D {width, height, 1} && image.format == format &&
        image.image_type == vk::ImageType::e2D && image.layers == 1 && image.mip_levels == 1 &&
        image.samples == 1 && (image.usage & usage) == usage;
}

void TransitFrameGenerationImage(vk::CommandBuffer command, VulkanImage& image,
                                 vk::ImageLayout layout, vk::AccessFlags2 access) {
    const auto stage = access == vk::AccessFlagBits2::eTransferWrite ?
        vk::PipelineStageFlagBits2::eTransfer : vk::PipelineStageFlagBits2::eAllCommands;
    vk::ImageMemoryBarrier2 barrier {};
    barrier.srcStageMask = image.state.pl_stage;
    barrier.srcAccessMask = image.state.access_mask;
    barrier.dstStageMask = stage;
    barrier.dstAccessMask = access;
    barrier.oldLayout = image.state.layout;
    barrier.newLayout = layout;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = vk::ImageSubresourceRange {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    vk::DependencyInfo dependency {};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    command.pipelineBarrier2(dependency);
    image.state = {stage, access, layout};
    image.subresource_states.clear();
}

bool CreatePass(GraphicContext& graphics, ComputePass& pass, std::span<const uint32_t> code,
                uint32_t sampled, uint32_t storage) {
    std::vector<vk::DescriptorSetLayoutBinding> bindings(sampled + storage);
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorCount = 1;
        bindings[i].descriptorType = i < sampled ? vk::DescriptorType::eCombinedImageSampler : vk::DescriptorType::eStorageImage;
        bindings[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
    }
    vk::DescriptorSetLayoutCreateInfo set {};
    set.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
    set.bindingCount = static_cast<uint32_t>(bindings.size());
    set.pBindings = bindings.data();
    if (graphics.device.createDescriptorSetLayout(&set, nullptr, &pass.descriptors) != vk::Result::eSuccess) return false;
    const vk::PushConstantRange push {vk::ShaderStageFlagBits::eCompute, 0, 16};
    vk::PipelineLayoutCreateInfo layout {};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &pass.descriptors;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (graphics.device.createPipelineLayout(&layout, nullptr, &pass.layout) != vk::Result::eSuccess) return false;
    const auto module = CompileSPV(code, graphics.device);
    vk::ComputePipelineCreateInfo pipeline {};
    pipeline.stage.stage = vk::ShaderStageFlagBits::eCompute;
    pipeline.stage.module = module;
    pipeline.stage.pName = "main";
    pipeline.layout = pass.layout;
    const auto result = graphics.device.createComputePipelines(nullptr, 1, &pipeline, nullptr, &pass.pipeline);
    graphics.device.destroyShaderModule(module, nullptr);
    return result == vk::Result::eSuccess;
}

void DestroyPass(vk::Device device, ComputePass& pass) {
    if (pass.pipeline) device.destroyPipeline(pass.pipeline, nullptr);
    if (pass.layout) device.destroyPipelineLayout(pass.layout, nullptr);
    if (pass.descriptors) device.destroyDescriptorSetLayout(pass.descriptors, nullptr);
}

void Dispatch(vk::CommandBuffer command, const ComputePass& pass,
              std::span<Image* const> sampled, std::span<Image* const> storage,
              vk::Sampler sampler, const void* push, uint32_t width, uint32_t height,
              std::span<const vk::ImageView> sampled_views = {}) {
    std::array<vk::DescriptorImageInfo, 7> infos {};
    std::array<vk::WriteDescriptorSet, 7> writes {};
    const auto count = sampled.size() + storage.size();
    EXIT_IF(count > infos.size());
    for (size_t i = 0; i < count; ++i) {
        const bool is_storage = i >= sampled.size();
        auto& image = *(is_storage ? storage[i - sampled.size()] : sampled[i]);
        infos[i].sampler = is_storage ? vk::Sampler {} : sampler;
        infos[i].imageView = !is_storage && i < sampled_views.size() ? sampled_views[i] : View(image, is_storage);
        infos[i].imageLayout = is_storage ? vk::ImageLayout::eGeneral : SampledLayout;
        writes[i].dstBinding = static_cast<uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eCombinedImageSampler;
        writes[i].pImageInfo = &infos[i];
    }
    command.bindPipeline(vk::PipelineBindPoint::eCompute, pass.pipeline);
    command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pass.layout, 0,
                                 static_cast<uint32_t>(count), writes.data());
    command.pushConstants(pass.layout, vk::ShaderStageFlagBits::eCompute, 0, 16, push);
    command.dispatch((width + 7) / 8, (height + 7) / 8, 1);
}
}

struct Gt7Dlss::Impl {
    explicit Impl(RenderContext& c) : context(c), graphics(c.GetGraphics()), scheduler(c.GetCommandScheduler()) {}
    RenderContext& context;
    GraphicContext& graphics;
    CommandScheduler& scheduler;
    const KytyDlssApi* api = nullptr;
    KytyDlssContext* ngx = nullptr;
    ComputePass prepare, composite;
    ComputePass native_prepare;
    const bool native_mode = Config::PssrDlssEnabled();
    bool native_initialized = false, native_ready = false, native_reset_pending = true;
    bool native_prior_reset = true;
    uint32_t native_pending_sources = Gt7FrameGenLifecycle::PendingInitial;
    uint32_t native_prior_reset_sources = Gt7FrameGenLifecycle::PendingInitial;
    Gt7FrameGenLifecycle::ResetDiagnostics native_reset_diagnostics;
    uint64_t native_context = 0, native_output_serial = 0, native_output_write_version = 0;
    uint64_t native_input_captures = 0;
    NativeInputDiagnostics native_input_diagnostics;
    vk::Image native_output_image {};
    GuestRange native_output_range {};
    vk::Sampler nearest, linear;
    std::unique_ptr<Image> color, depth, motion, invalid, output, effect, composed;
    ImageId scene_id {};
    GuestRange scene_range {};
    uint64_t evaluations = 0, composites = 0, cancellations = 0, rejected = 0;
    // Diagnostic buckets 0, 1, 2, 3+: the number of verified source-over draws
    // is not a validity condition for the premultiplied effect layer.
    std::array<uint64_t, 4> composites_by_effect_count {};
    std::chrono::steady_clock::time_point next_effect_report {};
    uint64_t boundary_evaluations = 0;
    uint64_t boundary_composites = 0, fg_frame = 1, fg_copied = 0, fg_snapshots = 0, fg_rejected = 0;
    uint32_t fg_temporal_attempts = 0;
    std::vector<FrameGenerationCamera> fg_cameras;
    CameraProbeState fg_camera_probe;
    std::array<uint32_t, CameraObservationCount> fg_camera_observations {};
    std::array<uint64_t, CameraObservationCount> native_camera_observations {};
    std::vector<std::array<uint64_t, 2>> fg_draw_signatures;
    uint32_t fg_candidate_draws = 0;
    uint64_t fg_camera_match_failures = 0, fg_hudless_captures = 0, fg_hudless_rejections = 0;
    std::array<std::pair<std::string, uint64_t>, 16> fg_cancel_reasons {};
    uint64_t fg_tonemap_rejections = 0;
    ImageId fg_scaled_scene_id {}, fg_output_id {};
    GuestRange fg_output_range {};
    vk::Image fg_output_image {};
    VulkanImage fg_hudless {};
    std::vector<uint8_t> fg_dcc_snapshot;
    bool fg_hudless_valid = false;
    HudlessDiagnostic fg_hudless_diagnostic;
    SceneDiagnostic fg_scene_diagnostic;
    FgAssociationDiagnostics fg_association;
    struct CompletedScene {
        Gt7FrameGen::FrameInputs inputs;
        uint64_t external_write_version = 0;
        Gt7FrameGen::StatusReason rejection = Gt7FrameGen::StatusReason::SceneUnverified;
    };
    Gt7FrameGenAssociation::CandidateState fg_completed_state;
    std::array<CompletedScene, Gt7FrameGenAssociation::CandidateState::Capacity> fg_completed;
    uint64_t fg_scene_serial = 1, fg_completed_captures = 0, fg_completed_consumed = 0;
    bool fg_scene_closed = false, fg_boundary_seen = false;
    uint32_t fg_tonemap_attempts = 0, fg_ui_draws = 0;
    Gt7FrameGenCamera::Parameters fg_camera {};
    float fg_jitter_x = 0.0f, fg_jitter_y = 0.0f, fg_frame_time_ms = 0.0f;
    bool fg_camera_valid = false, fg_reset = true, fg_input_ambiguous = false;
    bool active = false, reset = true, disabled = false, initialized = false;
    std::chrono::steady_clock::time_point last_time {};
    std::chrono::steady_clock::time_point fg_next_camera_report {};
    std::chrono::steady_clock::time_point fg_next_hudless_report {};
    std::chrono::steady_clock::time_point fg_next_scene_report {};
    // Complete snapshot counts, rather than a single sampled failure every five
    // seconds. These distinguish race coverage from occasional camera/menu cuts.
    std::array<uint64_t, 6> fg_input_results {};
    std::chrono::steady_clock::time_point fg_next_input_report {};

    void ReportNativeCameraObservations() const {
        const auto& o = native_camera_observations;
        std::printf("GT7 native FG camera totals: calls=%llu depth=%llu motion=%llu shape=%llu late=%llu limit=%llu already_rejected=%llu user=%llu address=%llu unmapped=%llu gpu_dirty=%llu read=%llu matrix=%llu changed=%llu accepted=%llu\n",
            static_cast<unsigned long long>(o[CameraCalls]), static_cast<unsigned long long>(o[CameraDepth]),
            static_cast<unsigned long long>(o[CameraMotion]), static_cast<unsigned long long>(o[CameraShape]),
            static_cast<unsigned long long>(o[CameraLate]), static_cast<unsigned long long>(o[CameraLimit]),
            static_cast<unsigned long long>(o[CameraAlreadyRejected]), static_cast<unsigned long long>(o[CameraUser]),
            static_cast<unsigned long long>(o[CameraAddress]), static_cast<unsigned long long>(o[CameraUnmapped]),
            static_cast<unsigned long long>(o[CameraGpuDirty]), static_cast<unsigned long long>(o[CameraRead]),
            static_cast<unsigned long long>(o[CameraMatrix]), static_cast<unsigned long long>(o[CameraChanged]),
            static_cast<unsigned long long>(o[CameraAccepted]));
    }

    void ReportFrameInputResults(bool final = false) {
        if (std::all_of(fg_input_results.begin(), fg_input_results.end(), [](auto n) { return n == 0; })) return;
        const auto now = std::chrono::steady_clock::now();
        if (!final && now < fg_next_input_report) return;
        fg_next_input_report = now + std::chrono::seconds(5);
        std::printf("GT7 FG input coverage: frame=%llu final=%u ready=%llu scene=%llu camera=%llu hudless=%llu duplicate=%llu unavailable=%llu\n",
            static_cast<unsigned long long>(fg_frame), final ? 1u : 0u,
            static_cast<unsigned long long>(fg_input_results[0]),
            static_cast<unsigned long long>(fg_input_results[1]),
            static_cast<unsigned long long>(fg_input_results[2]),
            static_cast<unsigned long long>(fg_input_results[3]),
            static_cast<unsigned long long>(fg_input_results[4]),
            static_cast<unsigned long long>(fg_input_results[5]));
    }

    void NoteFrameInputResult(bool accepted, Gt7FrameGen::StatusReason reason) {
        using Reason = Gt7FrameGen::StatusReason;
        const size_t bucket = accepted ? 0 : reason == Reason::SceneUnverified ? 1 :
            reason == Reason::CameraUnverified ? 2 : reason == Reason::HudlessUnverified ? 3 :
            reason == Reason::DuplicateFrame ? 4 : 5;
        ++fg_input_results[bucket];
        ReportFrameInputResults();
    }

    ~Impl() {
        // RenderContext owns this object and has already finished its scheduler.
        ReportFrameInputResults(true);
        fg_association.Report(true);
        if (native_input_captures || native_input_diagnostics.rejected) {
            std::printf("GT7 native FG input totals: captured=%llu rejected=%llu reason_overflow=%llu\n",
                static_cast<unsigned long long>(native_input_captures),
                static_cast<unsigned long long>(native_input_diagnostics.rejected),
                static_cast<unsigned long long>(native_input_diagnostics.overflow));
            ReportNativeCameraObservations();
            for (const auto& reason : native_input_diagnostics.reasons) if (reason.count)
                std::printf("GT7 native FG reject total: count=%llu reason=%s\n",
                    static_cast<unsigned long long>(reason.count), reason.text.data());
        }
        LOGF("GT7 DLSS: session summary: evaluated=%llu composited=%llu cancelled=%llu rejected=%llu\n",
            static_cast<unsigned long long>(evaluations), static_cast<unsigned long long>(composites),
            static_cast<unsigned long long>(cancellations), static_cast<unsigned long long>(rejected));
        if (Gt7FrameGen::InputsRequested(graphics))
            LOGF("GT7 FSR FG: input summary: captured=%llu rejected=%llu\n",
                 static_cast<unsigned long long>(fg_snapshots), static_cast<unsigned long long>(fg_rejected));
        if (ngx) {
            KytyDlssError error {};
            const auto result = api->destroy_context(ngx, 1, &error);
            if (result != KYTY_DLSS_OK) LOGF("GT7 DLSS: context destruction failed: %s\n", error.message);
        }
        DestroyPass(graphics.device, prepare);
        DestroyPass(graphics.device, composite);
        DestroyPass(graphics.device, native_prepare);
        if (nearest) graphics.device.destroySampler(nearest, nullptr);
        if (linear) graphics.device.destroySampler(linear, nullptr);
        if (fg_hudless.image) graphics.DeleteImage(fg_hudless);
        for (auto& completed : fg_completed)
            for (auto* image : {&completed.inputs.depth, &completed.inputs.motion, &completed.inputs.hudless_color})
                if (image->image) graphics.DeleteImage(*image);
    }

    std::unique_ptr<Image> MakeImage(uint32_t w, uint32_t h, vk::Format format, uint32_t bytes) {
        ImageInfo info {};
        info.extent = vk::Extent3D {w, h, 1};
        info.pixel_format = format;
        info.bytes_per_block = bytes;
        info.pitch = w;
        return std::make_unique<Image>(graphics, scheduler, info);
    }

    bool Initialize() {
        if (initialized) return true;
        if (disabled || !graphics.gt7_dlss_device_enabled || !(api = Gt7DlssHost::Api())) return false;
        std::vector<const char*> extensions;
        for (const auto& name : graphics.gt7_dlss_device_extensions) extensions.push_back(name.c_str());
        KytyDlssCreateInfo create {};
        create.struct_size = sizeof(create);
        create.paths = Gt7DlssHost::Paths();
        create.instance = static_cast<VkInstance>(graphics.instance);
        create.physical_device = static_cast<VkPhysicalDevice>(graphics.physical_device);
        create.device = static_cast<VkDevice>(graphics.device);
        create.get_instance_proc_addr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
        create.get_device_proc_addr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
        create.queue_family_index = graphics.queue_family;
        create.enabled_feature_flags = graphics.gt7_dlss_feature_flags;
        create.enabled_device_extension_count = static_cast<uint32_t>(extensions.size());
        create.enabled_device_extensions = extensions.data();
        create.input_width = InputWidth; create.input_height = InputHeight;
        create.output_width = OutputWidth; create.output_height = OutputHeight;
        create.quality = KYTY_DLSS_QUALITY;
        KytyDlssError error {};
        if (api->create_context(&create, &ngx, &error) != KYTY_DLSS_OK) {
            LOGF("GT7 DLSS: backend unavailable; original renderer retained: %s\n", error.message);
            disabled = true;
            return false;
        }
        if (!CreatePass(graphics, prepare, GT7_DLSS_PREPARE_SPV, 3, 4) ||
            !CreatePass(graphics, composite, GT7_DLSS_COMPOSITE_SPV, 4, 1)) {
            LOGF("GT7 DLSS: compute pipeline unavailable; original renderer retained\n");
            disabled = true;
            return false;
        }
        vk::SamplerCreateInfo sampler {};
        sampler.magFilter = sampler.minFilter = vk::Filter::eNearest;
        sampler.mipmapMode = vk::SamplerMipmapMode::eNearest;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        RequireVulkanSuccess(graphics.device.createSampler(&sampler, nullptr, &nearest), "create GT7 DLSS point sampler");
        sampler.magFilter = sampler.minFilter = vk::Filter::eLinear;
        RequireVulkanSuccess(graphics.device.createSampler(&sampler, nullptr, &linear), "create GT7 DLSS linear sampler");
        color = MakeImage(InputWidth, InputHeight, vk::Format::eR16G16B16A16Sfloat, 8);
        depth = MakeImage(InputWidth, InputHeight, vk::Format::eR32Sfloat, 4);
        motion = MakeImage(InputWidth, InputHeight, vk::Format::eR32G32Sfloat, 8);
        invalid = MakeImage(InputWidth, InputHeight, vk::Format::eR8Unorm, 1);
        output = MakeImage(OutputWidth, OutputHeight, vk::Format::eR16G16B16A16Sfloat, 8);
        effect = MakeImage(InputWidth, InputHeight, vk::Format::eR16G16B16A16Sfloat, 8);
        composed = MakeImage(GuestWidth, GuestHeight, vk::Format::eR16G16B16A16Sfloat, 8);
        initialized = true;
        const auto* requested_preset = std::getenv("KYTY_GT7_DLSS_PRESET");
        LOGF("GT7 DLSS: experimental GPU helper ready, 1920x1080 -> 2560x1440, requested preset %s quality (fixed input); guest tone-map/UI 3840x2160\n",
             requested_preset && *requested_preset ? requested_preset : "K (default)");
        return true;
    }

    bool InitializeNativeInputs() {
        if (native_initialized) return true;
        if (disabled || !native_mode || !Gt7FrameGen::InputsRequested(graphics)) return false;
        if (!CreatePass(graphics, native_prepare, GT7_NATIVE_FG_PREPARE_SPV, 2, 2)) {
            disabled = true;
            LOGF("GT7 native FG: input conversion unavailable; ordinary presentation retained\n");
            return false;
        }
        vk::SamplerCreateInfo sampler {};
        sampler.magFilter = sampler.minFilter = vk::Filter::eNearest;
        sampler.mipmapMode = vk::SamplerMipmapMode::eNearest;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        RequireVulkanSuccess(graphics.device.createSampler(&sampler, nullptr, &nearest), "create native FG sampler");
        depth = MakeImage(InputWidth, InputHeight, vk::Format::eR32Sfloat, 4);
        motion = MakeImage(InputWidth, InputHeight, vk::Format::eR32G32Sfloat, 8);
        native_initialized = true;
        LOGF("GT7 native FG: capture-only inputs ready, 1920x1080 temporal inputs, 3840x2160 HUD-less output\n");
        return true;
    }

    bool SceneReady() const {
        return !active && !fg_input_ambiguous && fg_temporal_attempts == 1 &&
            (native_mode ? native_initialized && native_ready :
             initialized && evaluations == boundary_evaluations + 1 && composites == boundary_composites + 1);
    }

    bool Reject(const char* reason) {
        fg_scene_diagnostic.NoteFirst("temporal pass rejected", reason);
        reset = true;
        fg_camera_valid = false;
        fg_input_ambiguous = true;
        if (Report(++rejected)) LOGF("GT7 DLSS: rejected temporal pass #%llu: %s\n", static_cast<unsigned long long>(rejected), reason);
        return false;
    }

    bool WatchesNativeSource() const {
        return Gt7FrameGenLifecycle::WatchesNativeSource(native_ready, fg_scene_closed);
    }

    void ResetLiveScene() {
        // Only the recording lane owns this state. Completed resources have
        // independent copies and must never be reset by a delayed CPU flip.
        if (active || evaluations == boundary_evaluations) reset = true;
        if (native_mode && !native_ready) {
            native_reset_pending = true;
            native_pending_sources |= Gt7FrameGenLifecycle::PendingSceneBoundary;
        }
        native_ready = false;
        native_output_image = nullptr;
        native_output_range = {};
        active = false;
        boundary_evaluations = evaluations;
        boundary_composites = composites;
        fg_temporal_attempts = 0;
        fg_camera_valid = false;
        fg_input_ambiguous = false;
        fg_cameras.clear();
        fg_camera_probe.pending.clear();
        fg_camera_observations.fill(0);
        fg_candidate_draws = 0;
        fg_scaled_scene_id = {};
        fg_output_id = {};
        fg_output_range = {};
        fg_output_image = nullptr;
        fg_hudless_valid = false;
        fg_tonemap_attempts = fg_ui_draws = 0;
        fg_hudless_diagnostic = {};
        fg_scene_diagnostic = {};
        fg_scene_closed = fg_boundary_seen = false;
        ++fg_scene_serial;
    }

    bool HasCompletedOutputs() const {
        for (size_t i = 0; i < fg_completed.size(); ++i) {
            const auto& candidate = fg_completed_state.Get(i);
            if (candidate.ready && !candidate.consumed) return true;
        }
        return false;
    }

    void InvalidateCompleted(size_t slot, Gt7FrameGen::StatusReason reason) {
        fg_completed_state.InvalidateSlot(slot);
        fg_completed[slot].rejection = reason;
    }
};

Gt7Dlss::Gt7Dlss(RenderContext& context) : m_impl(std::make_unique<Impl>(context)) {}
Gt7Dlss::~Gt7Dlss() = default;
bool Gt7Dlss::Active() const {
    return m_impl->active;
}
bool Gt7Dlss::NativeFrameGenerationInputs() const {
    return m_impl->native_mode && Gt7FrameGen::InputsRequested(m_impl->graphics);
}
bool Gt7Dlss::WatchesFrameGenerationInputs() const {
    const auto& s = *m_impl;
    return s.native_mode && !s.fg_scene_closed && s.fg_temporal_attempts == 0 &&
        std::any_of(s.fg_cameras.begin(), s.fg_cameras.end(), [](const auto& camera) {
            return camera.valid && !camera.rejected;
        });
}
bool Gt7Dlss::MatchesFrameGenerationInput(const RenderColorInfo& target) const {
    if (!WatchesFrameGenerationInputs()) return false;
    const auto& s = *m_impl;
    auto& textures = s.context.GetTextureCache();
    return std::any_of(s.fg_cameras.begin(), s.fg_cameras.end(), [&](const auto& camera) {
        return camera.valid && !camera.rejected &&
            ((target.image_id && (target.image_id == camera.depth_id || target.image_id == camera.motion_id)) ||
             textures.WriteOverlapsImage(target.desc.info.data, camera.depth_id, camera.depth_range) ||
             textures.WriteOverlapsImage(target.desc.info.data, camera.motion_id, camera.motion_range));
    });
}
Image* Gt7Dlss::EffectLayer() { return Active() ? m_impl->effect.get() : nullptr; }
bool Gt7Dlss::MatchesScene(const RenderColorInfo& color) const {
    return Active() && ((color.image_id && color.image_id == m_impl->scene_id) ||
                        Overlap(color.desc.info.data, m_impl->scene_range));
}
bool Gt7Dlss::MatchesWatchedWriteRange(GuestRange range) const {
    const auto& s = *m_impl;
    auto& textures = s.context.GetTextureCache();
    if (WatchesFrameGenerationInputs()) for (const auto& camera : s.fg_cameras) {
        if (camera.valid && !camera.rejected &&
            (textures.WriteOverlapsImage(range, camera.depth_id, camera.depth_range) ||
             textures.WriteOverlapsImage(range, camera.motion_id, camera.motion_range))) return true;
    }
    if ((s.WatchesNativeSource() && textures.WriteOverlapsImage(range, s.fg_scaled_scene_id, s.native_output_range)) ||
        (s.active && textures.WriteOverlapsImage(range, s.scene_id, s.scene_range)) ||
        (s.fg_hudless_valid && textures.WriteOverlapsImage(range, s.fg_output_id, s.fg_output_range))) return true;
    for (size_t i = 0; i < s.fg_completed.size(); ++i) {
        const auto& candidate = s.fg_completed_state.Get(i);
        if (candidate.ready && !candidate.consumed && textures.WriteOverlapsImage(range,
            OutputId(candidate.output), {candidate.output.address, candidate.output.size})) return true;
    }
    return false;
}
void Gt7Dlss::CancelFrame(const char* reason, bool invalidate_history) {
    auto& s = *m_impl;
    if (s.native_mode && s.fg_temporal_attempts == 0) for (auto& camera : s.fg_cameras) {
        camera.valid = false;
        camera.rejected = true;
        camera.reject_reason = reason;
    }
    for (size_t i = 0; i < s.fg_completed.size(); ++i)
        s.InvalidateCompleted(i, Gt7FrameGen::StatusReason::SceneUnverified);
    CancelLiveFrame(reason, invalidate_history);
}

void Gt7Dlss::CancelFrameForWrite(GuestRange range, const char* reason) {
    auto& s = *m_impl;
    auto& textures = s.context.GetTextureCache();
    if (WatchesFrameGenerationInputs()) for (auto& camera : s.fg_cameras) {
        if (textures.WriteOverlapsImage(range, camera.depth_id, camera.depth_range) ||
            textures.WriteOverlapsImage(range, camera.motion_id, camera.motion_range)) {
            camera.valid = false;
            camera.rejected = true;
            camera.reject_reason = reason;
        }
    }
    for (size_t i = 0; i < s.fg_completed.size(); ++i) {
        const auto& candidate = s.fg_completed_state.Get(i);
        if (candidate.ready && !candidate.consumed && textures.WriteOverlapsImage(range,
            OutputId(candidate.output), {candidate.output.address, candidate.output.size}))
            s.InvalidateCompleted(i, Gt7FrameGen::StatusReason::HudlessUnverified);
    }
    if ((s.WatchesNativeSource() && textures.WriteOverlapsImage(range, s.fg_scaled_scene_id, s.native_output_range)) ||
        (s.active && textures.WriteOverlapsImage(range, s.scene_id, s.scene_range)) ||
        (s.fg_hudless_valid && textures.WriteOverlapsImage(range, s.fg_output_id, s.fg_output_range)))
        CancelLiveFrame(reason);
}

void Gt7Dlss::CancelFrameForWrite(const RenderColorInfo& target, const char* reason) {
    auto& s = *m_impl;
    if (WatchesFrameGenerationInputs() && target.image_id) for (auto& camera : s.fg_cameras) {
        if (target.image_id == camera.depth_id || target.image_id == camera.motion_id) {
            camera.valid = false;
            camera.rejected = true;
            camera.reject_reason = reason;
        }
    }
    const bool live = (s.WatchesNativeSource() && target.image_id == s.fg_scaled_scene_id) ||
        MatchesScene(target) || (s.fg_hudless_valid && target.image_id &&
                                               target.image_id == s.fg_output_id);
    // A resolved image can alias a requested guest view with a different range.
    // Preserve identity matches in addition to range/metadata overlap checks.
    if (target.image_id) for (size_t i = 0; i < s.fg_completed.size(); ++i) {
        const auto& candidate = s.fg_completed_state.Get(i);
        if (candidate.ready && !candidate.consumed && OutputId(candidate.output) == target.image_id)
            s.InvalidateCompleted(i, Gt7FrameGen::StatusReason::HudlessUnverified);
    }
    CancelFrameForWrite(target.desc.info.data, reason);
    if (live) CancelLiveFrame(reason);
}

void Gt7Dlss::CancelLiveFrame(const char* reason, bool invalidate_history) {
    auto& s = *m_impl;
    // Calls before the first temporal pass are often no-ops. Do not let those
    // hide the first cancellation which actually discarded scene state.
    if (s.active || s.native_ready || s.fg_hudless_valid || invalidate_history)
        s.fg_scene_diagnostic.NoteFirst("scene cancelled", reason);
    // A clear, unbounded writer or later unverified operation must also discard
    // the already captured display-color scene, even after SR has composited.
    if (s.fg_hudless_valid) {
        s.fg_hudless_diagnostic.Note("captured scene invalidated", reason);
        bool first_for_reason = false;
        // Title-scene rejections must not hide the first new race failure.
        // Bound both storage and logging; reason strings are copied once.
        for (auto& [known_reason, count] : s.fg_cancel_reasons) {
            if (known_reason.empty()) known_reason = reason;
            if (known_reason == reason) {
                first_for_reason = ++count <= 4;
                break;
            }
        }
        if (++s.fg_hudless_rejections <= 8 || s.fg_hudless_rejections % 300 == 0 || first_for_reason)
            LOGF("GT7 FSR FG: HUD-less rejected #%llu frame=%llu: %s\n",
                 static_cast<unsigned long long>(s.fg_hudless_rejections),
                 static_cast<unsigned long long>(s.fg_frame), reason);
    }
    s.fg_hudless_valid = false;
    if (s.native_mode) {
        s.native_ready = false;
        s.native_reset_pending = true;
        s.native_pending_sources |= Gt7FrameGenLifecycle::PendingCancelled;
        s.fg_camera_valid = false;
    }
    if (invalidate_history) {
        s.reset = true;
        s.fg_camera_valid = false;
        s.fg_input_ambiguous = true;
    }
    if (!s.active) return;
    s.active = false;
    s.fg_camera_valid = false;
    s.reset = true;
    if (Report(++s.cancellations)) LOGF("GT7 DLSS: cancelled frame #%llu: %s (evaluated=%llu composited=%llu)\n",
        static_cast<unsigned long long>(s.cancellations), reason,
        static_cast<unsigned long long>(s.evaluations), static_cast<unsigned long long>(s.composites));
}

void Gt7Dlss::CancelSceneWriterFrame(const char* reason, uint64_t vertex_shader,
                                    uint64_t pixel_shader, const char* details) {
    auto& s = *m_impl;
    auto& diagnostic = s.fg_scene_diagnostic;
    // The caller is the failed effect-writer branch. Keep the same first-cause
    // rule as CancelFrame; never attribute a prior rejection to this later draw.
    if (s.active && !diagnostic.reason[0]) {
        diagnostic.NoteFirst("scene writer cancelled", reason, {}, {}, vertex_shader);
        diagnostic.writer_ps = pixel_shader;
        diagnostic.scene_writer = true;
        std::snprintf(diagnostic.writer_details.data(), diagnostic.writer_details.size(),
                      "%s", details ? details : "details unavailable");
    }
    CancelLiveFrame(reason);
}

void Gt7Dlss::FrameBoundary(bool discard_completed) {
    auto& s = *m_impl;
    if (s.fg_association.enabled) {
        auto& frame = s.fg_association.current;
        frame.serial = s.fg_frame;
        // These are flip-window diagnostics, independent of the live scene
        // boundaries used for completed-output association.
        frame.evaluations = s.evaluations - s.fg_association.boundary_evaluations;
        frame.composites = s.composites - s.fg_association.boundary_composites;
        s.fg_association.boundary_evaluations = s.evaluations;
        s.fg_association.boundary_composites = s.composites;
        frame.camera_valid = s.fg_camera_valid;
        frame.reset = s.fg_reset;
        frame.late_writers = s.fg_camera_observations[CameraLate];
        frame.tonemaps = s.fg_tonemap_attempts;
        frame.output = s.fg_output_id;
        frame.scene = s.fg_scaled_scene_id;
        frame.stage = s.fg_scene_diagnostic.stage;
        frame.writer_vs = s.fg_scene_diagnostic.writer_vs;
        s.fg_association.Boundary();
    }
    if (Gt7FrameGen::InputsRequested(s.graphics) && !discard_completed) {
        // CPU flip preparation can arrive after the next scene has begun.
        // Leave that scene intact even when the requested output has no candidate.
        // A later producer starts a new scope only after a completed tone-map,
        // or after a failed temporal sequence has crossed a flip boundary.
        s.fg_boundary_seen = true;
        ++s.fg_frame;
        return;
    }
    if (discard_completed) {
        for (size_t i = 0; i < s.fg_completed.size(); ++i)
            s.InvalidateCompleted(i, Gt7FrameGen::StatusReason::SceneUnverified);
        s.reset = true;
    }
    // SR-only retains its original reset-at-flip behavior.
    // Retiring a completed frame is not an unverified writer rejection.
    if (!s.active) s.fg_hudless_valid = false;
    CancelLiveFrame("scene reached presentation without a verified composite");
    // Motion vectors refer to the previous guest frame, not the previous NGX call.
    if (s.evaluations == s.boundary_evaluations) s.reset = true;
    s.boundary_evaluations = s.evaluations;
    s.boundary_composites = s.composites;
    s.fg_temporal_attempts = 0;
    s.fg_camera_valid = false;
    s.fg_input_ambiguous = false;
    s.fg_cameras.clear();
    s.fg_camera_probe.pending.clear();
    s.fg_camera_observations.fill(0);
    s.fg_candidate_draws = 0;
    s.fg_scaled_scene_id = {};
    s.fg_output_id = {};
    s.fg_output_range = {};
    s.fg_output_image = nullptr;
    s.fg_hudless_valid = false;
    s.fg_tonemap_attempts = s.fg_ui_draws = 0;
    s.fg_hudless_diagnostic = {};
    s.fg_scene_diagnostic = {};
    s.fg_scene_closed = s.fg_boundary_seen = false;
    ++s.fg_scene_serial;
    ++s.fg_frame;
}

// The recording hooks and the GPU-only methods below share no descriptor-pool
// allocations: push descriptors are copied into each command buffer.

bool Gt7Dlss::BeginTaa(CommandBuffer& buffer, const PreparedBindings& pixel,
                       std::span<const RenderColorInfo> colors) {
    auto& s = *m_impl;
    if (s.native_mode) return false;
    ++s.fg_temporal_attempts;
    auto* association = s.fg_association.BeginTaa(pixel, colors, s.evaluations, s.composites);
    s.fg_camera_valid = false;
    if (s.active) CancelLiveFrame("new temporal pass before composite");
    if (!pixel.runtime || !*pixel.runtime || pixel.images.size() != 4 ||
        pixel.buffer_sources.size() != 1 || colors.size() != 2 ||
        !pixel.bindless_heaps.empty() || !pixel.bindless_patches.empty())
        return s.Reject("binding cardinality");
    const auto& program = *pixel.runtime->program;
    if (program.stage != ShaderType::Pixel || program.shader_hash != 0xc3113b114fb02e5dULL ||
        program.info.images.size() != 4 || program.info.buffers.size() != 1 ||
        program.info.buffers[0].written || !pixel.runtime->resources->gpu_fills.empty())
        return s.Reject("unverified temporal program");
    for (size_t i = 0; i < 4; ++i) {
        const auto& texture = pixel.images[i];
        const auto& resource = program.info.images[i];
        if (!texture.image_id || !texture.image_view)
            return s.Reject("unresolved temporal texture view");
        if (!BaseView(texture.desc))
            return s.Reject("unsupported temporal texture subresource");
        if (resource.written || resource.bindless || resource.runtime)
            return s.Reject("unverified temporal resource access");
    }
    for (const auto& target : colors) {
        if (!target.image_id || !BaseView(target.desc) || target.guest_mip_level || target.guest_array_layer)
            return s.Reject("unsupported temporal target view");
    }
    auto& textures = s.context.GetTextureCache();
    auto& source_depth = textures.GetImage(pixel.images[0].image_id);
    auto& source_motion = textures.GetImage(pixel.images[1].image_id);
    auto& source_color = textures.GetImage(pixel.images[2].image_id);
    auto& source_history = textures.GetImage(pixel.images[3].image_id);
    auto& history_target = textures.GetImage(colors[0].image_id);
    auto& scene_target = textures.GetImage(colors[1].image_id);
    if (!Shape(source_depth, InputWidth, InputHeight, vk::Format::eD32SfloatS8Uint) ||
        !Shape(source_motion, InputWidth, InputHeight, vk::Format::eR16G16Sfloat) ||
        !Shape(source_color, InputWidth, InputHeight, vk::Format::eB10G11R11UfloatPack32) ||
        !Shape(source_history, GuestWidth, GuestHeight, vk::Format::eR16G16B16A16Sfloat) ||
        !Shape(history_target, GuestWidth, GuestHeight, vk::Format::eR16G16B16A16Sfloat) ||
        !Shape(scene_target, InputWidth, InputHeight, vk::Format::eB10G11R11UfloatPack32) ||
        colors[0].target_slot != 0 || colors[1].target_slot != 1 ||
        colors[0].image_id == pixel.images[3].image_id ||
        colors[1].image_id == pixel.images[2].image_id)
        return s.Reject("not the verified 1080p temporal resources");

    // This hook runs after RebindImages but before CommitBindings, which is what
    // fills TextureBinding::layout for the guest draw. Use the tracked backing
    // state of the three images sampled here. History is only matched, not read
    // by this helper, and can still be uninitialized on the first guest frame.
    if (source_color.backing.state.layout == vk::ImageLayout::eUndefined ||
        source_depth.backing.state.layout == vk::ImageLayout::eUndefined ||
        source_motion.backing.state.layout == vk::ImageLayout::eUndefined)
        return s.Reject("uninitialized temporal input image state");

    const auto& source = pixel.buffer_sources[0];
    constexpr uint64_t ConstantBytes = 128;
    if (!source.address || source.address > std::numeric_limits<uint64_t>::max() - ConstantBytes ||
        source.size != ConstantBytes ||
        Libs::LibKernel::Memory::TryClampRangeSize(source.address, ConstantBytes) != ConstantBytes ||
        !s.context.GetBufferCache().IsCleanForConcurrentRead(source.address, ConstantBytes))
        return s.Reject("temporal constants are not clean CPU data");
    std::array<float, 32> constants {};
    if (!Libs::LibKernel::Memory::TryReadBacking(source.address, constants.data(), ConstantBytes) ||
        constants[14] != 960.0f || constants[15] != -540.0f ||
        constants[16] != 1920.0f || constants[17] != 1080.0f ||
        constants[18] != 1.0f || constants[19] != 1.0f ||
        constants[24] != 0.0f || constants[25] != 0.0f ||
        !std::isfinite(constants[20]) || !std::isfinite(constants[21]) ||
        std::abs(constants[20] * 960.0f) > 2.0f || std::abs(constants[21] * 540.0f) > 2.0f)
        return s.Reject("temporal dimensions or jitter contract changed");
    if (association) {
        association->constants_valid = true;
        association->jitter_x = constants[20];
        association->jitter_y = constants[21];
    }
    if (!s.Initialize()) return s.Reject("backend initialization unavailable");

    // Handle drains pending CPU recording, not GPU execution. Every subsequent
    // command here records synchronously, so NGX and the host do not race.
    s.scheduler.EndRendering();
    const auto command = buffer.Handle();
    const std::array<Image*, 3> inputs {&source_color, &source_depth, &source_motion};
    const std::array<size_t, 3> indices {2, 0, 1};
    std::array<VulkanImageState, 3> saved {};
    std::array<vk::ImageView, 3> views {};
    for (size_t i = 0; i < inputs.size(); ++i) {
        saved[i] = inputs[i]->backing.state;
        views[i] = pixel.images[indices[i]].image_view;
        Transit(*inputs[i], SampledLayout, ShaderRead, command);
    }
    const std::array<Image*, 4> prepared {s.color.get(), s.depth.get(), s.motion.get(), s.invalid.get()};
    for (auto* image : prepared) Transit(*image, vk::ImageLayout::eGeneral, ShaderWrite, command);
    const struct { uint32_t width, height; float x, y; } push {
        InputWidth, InputHeight, constants[20], constants[21]};
    Dispatch(command, s.prepare, inputs, prepared, s.nearest, &push, InputWidth, InputHeight, views);
    for (auto* image : prepared) Transit(*image, SampledLayout, ShaderRead, command);
    for (size_t i = 0; i < inputs.size(); ++i)
        Transit(*inputs[i], saved[i].layout, saved[i].access_mask | ShaderRead, command);
    Transit(*s.output, vk::ImageLayout::eGeneral, ShaderWrite, command);

    KytyDlssEvaluateInfo evaluate {};
    evaluate.struct_size = sizeof(evaluate);
    evaluate.command_buffer = static_cast<VkCommandBuffer>(command);
    evaluate.color = BackendImage(*s.color);
    evaluate.depth = BackendImage(*s.depth);
    evaluate.motion = BackendImage(*s.motion);
    evaluate.output = BackendImage(*s.output, true);
    evaluate.jitter_x = -960.0f * constants[20];
    evaluate.jitter_y = 540.0f * constants[21];
    const auto now = std::chrono::steady_clock::now();
    evaluate.frame_time_ms = s.last_time.time_since_epoch().count() == 0 ? 16.666667f :
        std::clamp(std::chrono::duration<float, std::milli>(now - s.last_time).count(), 1.0f, 100.0f);
    evaluate.pre_exposure = evaluate.exposure_scale = 1.0f;
    evaluate.reset_history = s.reset ? 1u : 0u;
    KytyDlssRecordResult result {};
    result.struct_size = sizeof(result);
    KytyDlssError error {};
    const auto status = s.api->record_evaluate(s.ngx, &evaluate, &result, &error);
    s.scheduler.StateShadow().InvalidateAll();
    if (status != KYTY_DLSS_OK) {
        if (status == KYTY_DLSS_INVALID_ARGUMENT) {
            LOGF("GT7 DLSS: evaluation rejected before NGX recording: %s\n", error.message);
            return s.Reject("backend input contract");
        }
        EXIT("GT7 DLSS: backend recording failed (%d, NGX 0x%x): %s. The shared command buffer may be partially recorded; terminating the experimental session without submitting it.\n",
             static_cast<int>(status), error.ngx_result, error.message);
    }
    Transit(*s.output, SampledLayout, ShaderRead, command);
    Transit(*s.effect, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, command);
    const vk::ClearColorValue zero {std::array<float, 4> {0, 0, 0, 0}};
    const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    command.clearColorImage(s.effect->backing.image, vk::ImageLayout::eTransferDstOptimal, &zero, 1, &range);
    Transit(*s.effect, vk::ImageLayout::eColorAttachmentOptimal,
            vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite, command);
    s.scene_id = colors[1].image_id;
    s.scene_range = colors[1].desc.info.data;
    ++s.evaluations;
    if (CameraProbeEnabled())
        WriteCameraProbes(s.fg_camera_probe, pixel.images[0].image_id, pixel.images[1].image_id,
                         s.fg_frame, constants[20], constants[21]);
    if (Gt7FrameGen::InputsRequested(s.graphics)) {
        // Require camera and temporal resources from this guest frame, including
        // the precise projection jitter used by the producer and resolve.
        for (const auto& camera : s.fg_cameras) {
            if (camera.valid && camera.depth_id == pixel.images[0].image_id &&
                camera.motion_id == pixel.images[1].image_id &&
                camera.parameters.jitter_ndc_x == constants[20] &&
                camera.parameters.jitter_ndc_y == constants[21]) {
                s.fg_camera = camera.parameters;
                s.fg_camera_valid = true;
                break;
            }
        }
        const bool failed_camera = !s.fg_camera_valid && s.fg_camera_observations[CameraCalls] != 0;
        if (now >= s.fg_next_camera_report) {
            s.fg_next_camera_report = now + std::chrono::seconds(5);
            const char* reason = s.fg_camera_valid ? "none" :
                s.fg_camera_observations[CameraCalls] == 0 ? "no proved producer with compatible raster" :
                "no producer for current TAA depth/motion pair";
            if (!s.fg_camera_valid) for (const auto& camera : s.fg_cameras) {
                if (camera.depth_id == pixel.images[0].image_id && camera.motion_id == pixel.images[1].image_id) {
                    reason = camera.reject_reason ? camera.reject_reason : "projection jitter mismatch";
                    break;
                }
            }
            std::printf("GT7 FSR FG camera: frame=%llu candidate_draws=%u observed=%u accepted=%u matched=%u reason=%s\n",
                static_cast<unsigned long long>(s.fg_frame), s.fg_candidate_draws,
                s.fg_camera_observations[CameraCalls], s.fg_camera_observations[CameraAccepted],
                s.fg_camera_valid ? 1u : 0u, reason);
        }
        const bool report_failure = failed_camera &&
            (++s.fg_camera_match_failures <= 8 || s.fg_camera_match_failures % 300 == 0);
        if (report_failure)
            LOGF("GT7 FSR FG: camera association failure #%llu frame=%llu (details follow)\n",
                 static_cast<unsigned long long>(s.fg_camera_match_failures),
                 static_cast<unsigned long long>(s.fg_frame));
        if (Report(s.evaluations) || report_failure) {
            uint32_t valid = 0, depth_matches = 0, motion_matches = 0, pairs = 0, jitter_matches = 0;
            for (const auto& camera : s.fg_cameras) {
                valid += camera.valid;
                const bool depth_match = camera.depth_id == pixel.images[0].image_id;
                const bool motion_match = camera.motion_id == pixel.images[1].image_id;
                depth_matches += depth_match; motion_matches += motion_match;
                pairs += depth_match && motion_match;
                jitter_matches += camera.parameters.jitter_ndc_x == constants[20] &&
                                  camera.parameters.jitter_ndc_y == constants[21];
            }
            const auto& o = s.fg_camera_observations;
            LOGF("GT7 FSR FG: camera match frame=%llu candidate_draws=%u observed=%u candidates=%zu valid=%u depth_match=%u motion_match=%u pair_match=%u jitter_match=%u target_depth=%u:%u target_motion=%u:%u jitter_ndc=(%.9g,%.9g) accepted=%u failures=[depth:%u motion:%u shape:%u late:%u limit:%u rejected:%u user:%u address:%u unmapped:%u gpu_dirty:%u read:%u matrix:%u changed:%u]\n",
                 static_cast<unsigned long long>(s.fg_frame), s.fg_candidate_draws, o[CameraCalls],
                 s.fg_cameras.size(), valid, depth_matches, motion_matches, pairs, jitter_matches,
                 pixel.images[0].image_id.index, pixel.images[0].image_id.generation,
                 pixel.images[1].image_id.index, pixel.images[1].image_id.generation, constants[20], constants[21],
                 o[CameraAccepted], o[CameraDepth], o[CameraMotion], o[CameraShape], o[CameraLate], o[CameraLimit],
                 o[CameraAlreadyRejected], o[CameraUser], o[CameraAddress], o[CameraUnmapped],
                 o[CameraGpuDirty], o[CameraRead], o[CameraMatrix], o[CameraChanged]);
            for (size_t i = 0; i < std::min(s.fg_cameras.size(), size_t(2)); ++i) {
                const auto& camera = s.fg_cameras[i];
                LOGF("GT7 FSR FG: camera candidate[%zu] depth=%u:%u motion=%u:%u valid=%u rejected=%u jitter_ndc=(%.9g,%.9g) reason=%s\n",
                     i, camera.depth_id.index, camera.depth_id.generation, camera.motion_id.index,
                     camera.motion_id.generation, camera.valid ? 1u : 0u, camera.rejected ? 1u : 0u,
                     camera.parameters.jitter_ndc_x, camera.parameters.jitter_ndc_y,
                     camera.reject_reason ? camera.reject_reason : "none");
            }
        }
        s.fg_jitter_x = evaluate.jitter_x;
        s.fg_jitter_y = evaluate.jitter_y;
        // Keep the unclamped duration so the FG backend can detect a pause;
        // the stable SR backend retains its existing clamped duration above.
        s.fg_frame_time_ms = s.last_time.time_since_epoch().count() == 0 ? 16.666667f :
            std::chrono::duration<float, std::milli>(now - s.last_time).count();
        s.fg_reset = result.reset_used != 0 || s.fg_frame_time_ms > 100.0f;
    }
    s.last_time = now;
    if (association) {
        association->evaluated = true;
        association->camera_matched = s.fg_camera_valid;
        association->reset = result.reset_used != 0;
    }
    s.reset = false;
    s.active = true;
    if (s.evaluations == 1) {
        const auto* trace = std::getenv("KYTY_GT7_DLSS_TRACE");
        if (trace && trace[0] == '1' && trace[1] == '\0') {
            FrameGraph::RequestCapture();
            LOGF("GT7 DLSS: requested frame-graph trace after first evaluation\n");
        }
    }
    if (Report(s.evaluations)) LOGF("GT7 DLSS: evaluated #%llu 1920x1080 -> 2560x1440, jitter=(%.6f,%.6f), reset=%u\n",
        static_cast<unsigned long long>(s.evaluations), evaluate.jitter_x, evaluate.jitter_y, result.reset_used);
    return true;
}

#include "gt7DlssComposite.inl"

#include "gt7NativePssrInputs.inl"

#include "gt7FrameGenInputs.inl"

} // namespace Libs::Graphics
