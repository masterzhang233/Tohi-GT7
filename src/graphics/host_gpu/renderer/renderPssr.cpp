#include "common/assert.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "gpu_blit_shaders/gpu_pssr_linear_spv.h"
#include "graphics/guest_gpu/pssr.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/gt7Dlss.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Libs::Graphics {
namespace {

struct PssrParameters {
	uint32_t input_size[2];
	uint32_t output_size[2];
	uint32_t output_channels[4];
	uint32_t input_origin[2];
};

// Opt-in evidence, not an image correction. Copy only 16 individual pixels from
// the selected view, and the exposure texel, before/after the same native request.
// This can expose finite brightness spikes as well as sampled NaNs, without a
// whole 4K download or a synchronous GPU wait. Unsampled pixels are not validated.
constexpr uint64_t PssrDiagnosticRequests = 8192;
constexpr uint32_t PssrDiagnosticPending = 96;
std::atomic<uint32_t> pssr_diagnostic_pending {0};

bool PssrDiagnosticsEnabled() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_PSSR_DIAGNOSTICS");
		return value != nullptr && std::string_view(value) == "1";
	}();
	return enabled;
}

std::string PssrDiagnosticPrefix(uint64_t sequence, const Pssr::Request& request) {
	return fmt::format("PSSR diagnostic: seq={} context={:016x}", sequence, request.context);
}

float DiagnosticUnsignedFloat(uint32_t bits, uint32_t mantissa_bits) {
	const auto mantissa = bits & ((1u << mantissa_bits) - 1u);
	const auto exponent = (bits >> mantissa_bits) & 31u;
	if (exponent == 31u) {
		return mantissa != 0 ? std::numeric_limits<float>::quiet_NaN()
		                     : std::numeric_limits<float>::infinity();
	}
	return std::ldexp(static_cast<float>(mantissa + (exponent ? 1u << mantissa_bits : 0u)),
	                  static_cast<int>(exponent ? exponent : 1u) - 15 -
	                      static_cast<int>(mantissa_bits));
}

float DiagnosticHalf(uint16_t bits) {
	const auto value = DiagnosticUnsignedFloat(bits & 0x7fffu, 10);
	return (bits & 0x8000u) != 0 ? -value : value;
}

std::array<float, 4> DiagnosticTexel(const uint8_t* data, vk::Format format,
                                    vk::ComponentMapping mapping) {
	std::array<float, 4> rgba {0, 0, 0, 1};
	if (format == vk::Format::eB10G11R11UfloatPack32) {
		uint32_t packed = 0;
		std::memcpy(&packed, data, sizeof(packed));
		rgba = {DiagnosticUnsignedFloat(packed & 0x7ffu, 6),
		        DiagnosticUnsignedFloat((packed >> 11u) & 0x7ffu, 6),
		        DiagnosticUnsignedFloat(packed >> 22u, 5), 1};
	} else if (format == vk::Format::eR32Sfloat) {
		std::memcpy(&rgba[0], data, sizeof(float));
	} else {
		const uint32_t channels = format == vk::Format::eR16Sfloat ? 1u : 4u;
		for (uint32_t i = 0; i < channels; ++i) {
			uint16_t half = 0;
			std::memcpy(&half, data + i * sizeof(half), sizeof(half));
			rgba[i] = DiagnosticHalf(half);
		}
	}
	const auto source = rgba;
	const auto channel = [&](vk::ComponentSwizzle swizzle, uint32_t identity) {
		switch (swizzle) {
			case vk::ComponentSwizzle::eIdentity: return source[identity];
			case vk::ComponentSwizzle::eZero: return 0.f;
			case vk::ComponentSwizzle::eOne: return 1.f;
			case vk::ComponentSwizzle::eR: return source[0];
			case vk::ComponentSwizzle::eG: return source[1];
			case vk::ComponentSwizzle::eB: return source[2];
			case vk::ComponentSwizzle::eA: return source[3];
			default: return std::numeric_limits<float>::quiet_NaN();
		}
	};
	return {channel(mapping.r, 0), channel(mapping.g, 1), channel(mapping.b, 2),
	        channel(mapping.a, 3)};
}

void PssrDiagnosticBinding(TextureCache& cache, const TextureBinding& binding,
                            const std::string& prefix, const char* role) {
	const auto& image = cache.GetImage(binding.image_id);
	const auto& view = binding.desc.view_info;
	Log::WriteToConsoleAndLog(fmt::format(
	    "{} role={} id={}:{} guest={:012x}+{:x} image={} view={} backing={}x{} levels={} layers={} "
	    "mip={} layer={} format={}/{} map={},{},{},{} serial={} registered={} rebind={}\n",
	    prefix, role, binding.image_id.index, binding.image_id.generation,
	    binding.desc.info.data.address, binding.desc.info.data.size,
	    fmt::ptr(static_cast<VkImage>(image.backing.image)),
	    fmt::ptr(static_cast<VkImageView>(binding.image_view)),
	    image.backing.extent.width, image.backing.extent.height,
	    image.backing.mip_levels, image.backing.layers, view.base_level, view.base_layer,
	    static_cast<int>(view.format), static_cast<int>(image.backing.format),
	    static_cast<int>(view.mapping.r), static_cast<int>(view.mapping.g),
	    static_cast<int>(view.mapping.b), static_cast<int>(view.mapping.a),
	    image.content_serial, image.registered, image.binding.needs_rebind));
}

void PssrDiagnosticReadback(RenderContext& context, const TextureBinding& binding,
                             const std::string& prefix, const char* role,
                             uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                             bool exposure = false) {
	auto& image = context.GetTextureCache().GetImage(binding.image_id);
	const auto& view = binding.desc.view_info;
	const auto format = view.format;
	const uint32_t texel_bytes = format == vk::Format::eR16G16B16A16Sfloat ? 8u :
	                             format == vk::Format::eR16Sfloat ? 2u :
	                             format == vk::Format::eR32Sfloat ||
	                             format == vk::Format::eB10G11R11UfloatPack32 ? 4u : 0u;
	if (view.base_level >= 32 || view.base_level >= image.backing.mip_levels ||
	    view.base_layer >= image.backing.layers) {
		Log::WriteToConsoleAndLog(fmt::format("{} role={} sample=skipped-subresource-contract\n", prefix, role));
		return;
	}
	const uint32_t view_width = std::max(image.backing.extent.width >> view.base_level, 1u);
	const uint32_t view_height = std::max(image.backing.extent.height >> view.base_level, 1u);
	if (texel_bytes == 0 || image.info.IsDepth() || image.backing.samples != 1 ||
	    !(image.backing.usage & vk::ImageUsageFlagBits::eTransferSrc) ||
	    image.backing.image_type != vk::ImageType::e2D ||
	    view.aspect != vk::ImageAspectFlagBits::eColor || width == 0 || height == 0 ||
	    x > view_width || y > view_height || width > view_width - x || height > view_height - y ||
	    (exposure && (view_width != 1 || view_height != 1 ||
	                  (format != vk::Format::eR16Sfloat && format != vk::Format::eR32Sfloat)))) {
		Log::WriteToConsoleAndLog(fmt::format("{} role={} sample=skipped-resource-contract\n", prefix, role));
		return;
	}
	if (pssr_diagnostic_pending.fetch_add(1, std::memory_order_relaxed) >= PssrDiagnosticPending) {
		pssr_diagnostic_pending.fetch_sub(1, std::memory_order_relaxed);
		Log::WriteToConsoleAndLog(fmt::format("{} role={} sample=skipped-pending-budget\n", prefix, role));
		return;
	}
	// A shared token bounds pending allocations and remains alive until the GPU
	// completion callback. It also releases the claim on an early host-side failure.
	auto pending = std::shared_ptr<void>(nullptr, [](void*) {
		pssr_diagnostic_pending.fetch_sub(1, std::memory_order_relaxed);
	});
	const uint32_t samples = exposure ? 1u : 16u;
	const uint32_t stride = std::max(texel_bytes, 4u); // Vulkan copy offset alignment.
	const uint64_t bytes = uint64_t {samples} * stride;
	auto& scheduler = context.GetCommandScheduler();
	auto download = std::make_shared<Buffer>(context.GetGraphics(), scheduler,
	                                         MemoryUsage::Download, 0, AllFlags, bytes);
	if (download->Mapped().data() == nullptr) return;
	std::array<vk::BufferImageCopy, 16> regions {};
	for (uint32_t i = 0; i < samples; ++i) {
		auto& region = regions[i];
		region.bufferOffset = uint64_t {i} * stride;
		region.imageSubresource = {vk::ImageAspectFlagBits::eColor, view.base_level, view.base_layer, 1};
		region.imageOffset = vk::Offset3D {
		    static_cast<int32_t>(x + (exposure ? 0u : (width - 1u) * (i % 4u) / 3u)),
		    static_cast<int32_t>(y + (exposure ? 0u : (height - 1u) * (i / 4u) / 3u)), 0};
		region.imageExtent = vk::Extent3D {1, 1, 1};
	}
	image.Download(std::span {regions.data(), samples}, download->Handle(), 0, bytes);
	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = download->Handle();
	barrier.size = bytes;
	scheduler.Current().Recorder().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                                               vk::PipelineStageFlagBits::eHost, {}, 0,
	                                               nullptr, 1, &barrier, 0, nullptr);
	const auto label = fmt::format("{} role={} mip={} layer={} rect={},{},{}x{} format={}", prefix,
	                                role, view.base_level, view.base_layer, x, y, width, height,
	                                static_cast<int>(format));
	scheduler.DeferPriorityOperation([download, pending, bytes, samples, stride, format,
	                                  mapping = view.mapping, label, exposure] {
		download->Invalidate(0, bytes);
		const auto* data = download->Mapped().data();
		if (exposure) {
			uint32_t bits = 0;
			std::memcpy(&bits, data, format == vk::Format::eR16Sfloat ? 2u : 4u);
			const auto value = DiagnosticTexel(data, format, mapping)[0];
			Log::WriteToConsoleAndLog(fmt::format("{} value={:.9g} bits={:08x} finite={} positive={}\n",
			                                       label, value, bits, std::isfinite(value), value > 0));
			return;
		}
		float minimum = std::numeric_limits<float>::infinity();
		float maximum = -std::numeric_limits<float>::infinity();
		double sum = 0;
		uint32_t finite = 0, nonfinite = 0;
		for (uint32_t i = 0; i < samples; ++i) {
			const auto rgba = DiagnosticTexel(data + i * stride, format, mapping);
			if (!std::isfinite(rgba[0]) || !std::isfinite(rgba[1]) || !std::isfinite(rgba[2])) {
				++nonfinite;
				continue;
			}
			const float luminance = .2126f * rgba[0] + .7152f * rgba[1] + .0722f * rgba[2];
			minimum = std::min(minimum, luminance);
			maximum = std::max(maximum, luminance);
			sum += luminance;
			++finite;
		}
		Log::WriteToConsoleAndLog(fmt::format(
		    "{} grid=4x4 finite={} nonfinite={} luminance_min={:.9g} max={:.9g} mean={:.9g} scope=sampled-only\n",
		    label, finite, nonfinite, minimum, maximum,
		    finite ? sum / finite : std::numeric_limits<double>::quiet_NaN()));
	});
}

// DLSS reads and writes with stages and accesses of its own.
void MemoryBarrier(vk::CommandBuffer command) {
	const vk::MemoryBarrier2 barrier {.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands,
	                                  .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
	                                  .dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands,
	                                  .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
	                                                   vk::AccessFlagBits2::eMemoryWrite};
	command.pipelineBarrier2(
	    vk::DependencyInfo {.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}

} // namespace

void RenderExecutor::ShutdownPssr() {
	// RenderContext has already drained and shut down its scheduler. Do not access
	// that member here: reverse member destruction destroys it before the executor.
	for (const auto& context: m_dlss_contexts) {
		if (context.feature != nullptr) {
			Dlss::ReleaseFeature(context.feature);
		}
	}
	m_dlss_contexts.clear();
	if (m_pssr_requests != 0) {
		Dlss::Shutdown();
		std::printf("PSSR summary: requests=%llu dlss=%llu linear=%llu\n",
		            static_cast<unsigned long long>(m_pssr_requests),
		            static_cast<unsigned long long>(m_pssr_dlss_evaluations),
		            static_cast<unsigned long long>(m_pssr_linear_fallbacks));
	}
	const auto device = m_context.GetGraphics().device;
	if (m_pssr_pipeline != nullptr) device.destroyPipeline(m_pssr_pipeline, nullptr);
	if (m_pssr_pipeline_layout != nullptr) device.destroyPipelineLayout(m_pssr_pipeline_layout, nullptr);
	if (m_pssr_descriptor_layout != nullptr) device.destroyDescriptorSetLayout(m_pssr_descriptor_layout, nullptr);
}

bool RenderExecutor::Upscale(vk::CommandBuffer command, const Pssr::Request& request,
                             Dlss::Inputs& inputs, bool& reset_used) {
	const auto release = [&](Dlss::Feature* feature) {
		if (feature != nullptr) {
			m_context.GetCommandScheduler().DeferOperation(
			    [feature] { Dlss::ReleaseFeature(feature); });
		}
	};
	auto context = std::ranges::find(m_dlss_contexts, request.context, &DlssContext::context);
	bool reset   = (request.flags & Pssr::kReset) != 0;
	if (context == m_dlss_contexts.end() || context->feature == nullptr ||
	    !Dlss::Takes(context->creation, request, inputs)) {
		// A render size no quality mode takes scales linearly.
		const auto creation = Dlss::Choose(*Dlss::Capabilities(), request, inputs);
		if (!creation) {
			return false;
		}
		if (context != m_dlss_contexts.end()) {
			release(context->feature);
		} else {
			// A library has at most four contexts.
			if (m_dlss_contexts.size() == 4) {
				release(m_dlss_contexts.front().feature);
				m_dlss_contexts.erase(m_dlss_contexts.begin());
			}
			context = m_dlss_contexts.insert(m_dlss_contexts.end(), {request.context, {}, nullptr});
		}
		context->creation = *creation;
		context->feature  = Dlss::CreateFeature(command, *creation);
		reset             = true;
	}
	if (context->feature == nullptr) {
		return false;
	}
	reset |= context->reset_pending;
	MemoryBarrier(command);
	// Evaluate(false) is only a safe preflight rejection. The backend terminates
	// on an NGX failure after recording may have begun, never submitting a poisoned
	// command buffer with a linear fallback appended to it.
	if (!Dlss::Evaluate(command, context->feature, request, inputs, reset)) {
		// INVALID_ARGUMENT guarantees no NGX commands or history changes. Retain
		// this compatible feature so repeated bad inputs cannot trigger six
		// capability queries and parameter allocation on every fallback frame.
		context->reset_pending = true;
		return false;
	}
	context->reset_pending = false;
	reset_used = reset;
	MemoryBarrier(command);
	return true;
}

Dlss::Resource RenderExecutor::PssrResource(const TextureBinding& binding, bool write) const {
	const auto& image = m_context.GetTextureCache().GetImage(binding.image_id);
	const auto& view = binding.desc.view_info;
	// FindImage rebases the view into an existing parent allocation, while the
	// descriptor keeps its original standalone extent. Applying the rebased mip
	// to that extent again would halve it twice.
	const vk::Extent2D extent {std::max(image.info.extent.width >> view.base_level, 1u),
	                          std::max(image.info.extent.height >> view.base_level, 1u)};
	const bool depth = image.info.IsDepth();
	const auto aspect = depth ? vk::ImageAspectFlagBits::eDepth : view.aspect;
	return Dlss::Resource {
	    .view = static_cast<VkImageView>(binding.image_view),
	    .image = static_cast<VkImage>(image.backing.image),
	    .range = {static_cast<VkImageAspectFlags>(aspect), view.base_level, 1, view.base_layer, 1},
	    .format = static_cast<VkFormat>(depth ? image.backing.format : view.format),
	    .width = extent.width, .height = extent.height, .read_write = write};
}

void RenderExecutor::ProcessPssr(CommandBuffer& buffer, const Pssr::Request& request) {
	m_context.GetCommandScheduler().PopPendingOperations();
	Common::LockGuard lock(m_context.GetMutex());
	auto* frame_generation = m_context.GetGt7Dlss();
	if (frame_generation) frame_generation->BeginNativePssr();
	++m_pssr_requests;
	if (m_pssr_requests == 1) {
		Dlss::Init(m_context.GetGraphics(), VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
		           VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr);
	}
	ShaderTextureResource input_descriptor {}, output_descriptor {};
	std::copy_n(request.input, 8, input_descriptor.fields);
	std::copy_n(request.output, 8, output_descriptor.fields);
	const auto is_2d = [](const ShaderTextureResource& descriptor) {
		return !descriptor.IsNull() &&
		       (descriptor.Type() == Prospero::ImageType::kColor2D ||
		        descriptor.Type() == Prospero::ImageType::kColor2DArray) &&
		       descriptor.TileMode() != Prospero::TileMode::kDepth;
	};
	EXIT_IF(!is_2d(input_descriptor) || !is_2d(output_descriptor) ||
	        !Pssr::IsColorFormat(input_descriptor.Format()) ||
	        output_descriptor.Format() != Prospero::BufferFormat::k11_11_10Float ||
	        output_descriptor.TileMode() != Prospero::TileMode::kStandard256B ||
	        output_descriptor.BaseLevel() != 0 || output_descriptor.LastLevel() != 0 ||
	        output_descriptor.MaxMip() != 0 || output_descriptor.BaseArray5() != 0 ||
	        output_descriptor.Depth() != 0 || request.width == 0 || request.height == 0);
	const auto input_width = std::max(1u, (input_descriptor.Width5() + 1u) >>
	                                         input_descriptor.BaseLevel());
	const auto input_height = std::max(1u, (input_descriptor.Height5() + 1u) >>
	                                          input_descriptor.BaseLevel());
	EXIT_IF(request.x > input_width || request.y > input_height ||
	        request.width > input_width - request.x ||
	        request.height > input_height - request.y);
	PssrParameters parameters {{request.width, request.height},
	                           {output_descriptor.Width5() + 1u, output_descriptor.Height5() + 1u},
	                           {4, 4, 4, 4}, {request.x, request.y}};
	for (uint32_t component = 0; component < 3; ++component) {
		for (uint32_t source = 0; source < 4; ++source) {
			if (GetDstSel(output_descriptor.DstSelXYZW(), source) == 4u + component) {
				parameters.output_channels[component] = source;
				break;
			}
		}
	}
	const auto resolve = [&](const uint32_t* words, bool output) {
		ShaderRecompiler::IR::ImageResource resource {};
		resource.resource_class = output ? ShaderRecompiler::IR::ImageResourceClass::Storage
		                                 : ShaderRecompiler::IR::ImageResourceClass::Sampled;
		resource.numeric_class = Prospero::TextureNumericClass::Float;
		resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
		resource.read = !output;
		resource.written = output;
		ShaderRecompiler::IR::DescriptorValue value {};
		std::copy_n(words, 8, value.dwords.begin());
		value.dword_count = 8;
		auto binding = ResolveTexture(resource, value);
		binding.desc.view_info.level_count = 1;
		binding.desc.view_info.min_lod = 0;
		return binding;
	};
	auto& cache = m_context.GetTextureCache();
	auto input = resolve(request.input, false);
	BindImage(input.image_id, false);
	auto output = resolve(request.output, true);
	BindImage(output.image_id, true);
	EXIT_IF(input.desc.info.data.address < output.desc.info.data.End() &&
	        output.desc.info.data.address < input.desc.info.data.End());
	// Depth, motion vectors and exposure for DLSS, which writes the output unswizzled.
	const std::array<const uint32_t*, 3>         dlss_words {request.depth, request.motion,
	                                                         request.exposure};
	std::array<std::optional<TextureBinding>, 3> dlss_bindings;
	bool temporal_descriptors = true;
	for (size_t i = 0; i < dlss_words.size(); ++i) {
		ShaderTextureResource descriptor {};
		std::copy_n(dlss_words[i], 8, descriptor.fields);
		if (descriptor.IsNull()) {
			temporal_descriptors &= i == 2; // Exposure alone is optional.
			continue;
		}
		const auto format = descriptor.Format();
		const bool shape = (descriptor.Type() == Prospero::ImageType::kColor2D ||
		                    descriptor.Type() == Prospero::ImageType::kColor2DArray) &&
		                   descriptor.BaseArray5() <= descriptor.Depth() &&
		                   descriptor.BaseLevel() <= descriptor.LastLevel();
		const bool supported = i == 0 ? (format == Prospero::BufferFormat::k32Float ||
		                                 format == Prospero::BufferFormat::k16Float ||
		                                 format == Prospero::BufferFormat::k16UNorm) :
		                       i == 1 ? (format == Prospero::BufferFormat::k16_16Float ||
		                                 format == Prospero::BufferFormat::k32_32Float ||
		                                 format == Prospero::BufferFormat::k16_16SNorm) :
		                                (format == Prospero::BufferFormat::k32Float ||
		                                 format == Prospero::BufferFormat::k16Float);
		// Check before ResolveTexture: unsupported descriptor types/encodings must
		// reject temporal reconstruction, not enter the general shader binding path
		// which can terminate on a format it cannot represent.
		temporal_descriptors &= shape && supported;
	}
	if (Dlss::Capabilities() != nullptr && Dlss::Accepts(request) &&
	    temporal_descriptors &&
	    parameters.output_channels[0] == 0 && parameters.output_channels[1] == 1 &&
	    parameters.output_channels[2] == 2) {
		for (size_t i = 0; i < dlss_words.size(); ++i) {
			ShaderTextureResource descriptor {};
			std::copy_n(dlss_words[i], 8, descriptor.fields);
			if (!descriptor.IsNull()) {
				dlss_bindings[i] = resolve(dlss_words[i], false);
				BindImage(dlss_bindings[i]->image_id, false);
			}
		}
	}
	const auto acquire = [&](TextureBinding& binding, const uint32_t* words, bool storage) {
		auto& previous = cache.GetImage(binding.image_id);
		if (!previous.registered || previous.binding.needs_rebind) {
			previous.binding = {};
			binding = resolve(words, storage);
			BindImage(binding.image_id, storage);
		}
		binding.image_view = cache.FindTexture(binding.image_id, binding.desc);
	};
	for (size_t i = 0; i < dlss_words.size(); ++i) {
		if (dlss_bindings[i]) {
			acquire(*dlss_bindings[i], dlss_words[i], false);
		}
	}
	acquire(input, request.input, false);
	acquire(output, request.output, true);
	auto& source = cache.GetImage(input.image_id);
	auto& destination = cache.GetImage(output.image_id);
	const auto& source_view = input.desc.view_info;
	const auto& destination_view = output.desc.view_info;
	EXIT_IF(source.info.IsDepth() || destination.info.IsDepth() ||
	        source.backing.samples != 1 || destination.backing.samples != 1 ||
	        (input.image_id == output.image_id &&
	         source_view.base_level == destination_view.base_level &&
	         source_view.base_layer == destination_view.base_layer));
	const bool diagnose = PssrDiagnosticsEnabled() && m_pssr_requests <= PssrDiagnosticRequests;
	std::string diagnostic_prefix;
	if (diagnose) {
		diagnostic_prefix = PssrDiagnosticPrefix(m_pssr_requests, request);
		if (m_pssr_requests == 1) {
			Log::WriteToConsoleAndLog(fmt::format(
			    "PSSR diagnostics enabled: first {} requests; exact exposure texel and 4x4 input/output samples; "
			    "at most {} pending readbacks; asynchronous, diagnostic overhead is not a performance benchmark.\n",
			    PssrDiagnosticRequests, PssrDiagnosticPending));
		}
		Log::WriteToConsoleAndLog(fmt::format(
		    "{} flags={:x} active={},{},{}x{} pre_exposure={:.9g} pre_bits={:08x} "
		    "jitter={:.9g},{:.9g} mv_scale={:.9g},{:.9g} exposure_bound={}\n",
		    diagnostic_prefix, request.flags, request.x, request.y, request.width, request.height,
		    request.pre_exposure, std::bit_cast<uint32_t>(request.pre_exposure),
		    request.jitter[0], request.jitter[1], request.motion_scale[0], request.motion_scale[1],
		    dlss_bindings[2].has_value()));
		if (m_pssr_requests <= 3 || m_pssr_requests % 60 == 0) {
			PssrDiagnosticBinding(cache, input, diagnostic_prefix, "input");
			PssrDiagnosticBinding(cache, output, diagnostic_prefix, "output");
			constexpr const char* names[] {"depth", "motion", "exposure"};
			for (size_t i = 0; i < dlss_bindings.size(); ++i) {
				if (dlss_bindings[i]) PssrDiagnosticBinding(cache, *dlss_bindings[i], diagnostic_prefix, names[i]);
			}
		}
		PssrDiagnosticReadback(m_context, input, diagnostic_prefix, "input",
		                         request.x, request.y, request.width, request.height);
		if (dlss_bindings[2]) {
			PssrDiagnosticReadback(m_context, *dlss_bindings[2], diagnostic_prefix, "exposure", 0, 0, 1, 1, true);
		}
	} else if (PssrDiagnosticsEnabled() && m_pssr_requests == PssrDiagnosticRequests + 1) {
		Log::WriteToConsoleAndLog("PSSR diagnostics: request budget exhausted; subsequent native requests are not sampled.\n");
	}

	buffer.EndRendering();
	// Diagnostic copies use Recorder(). Reacquire the raw handle only after them,
	// draining those commands and invalidating the renderer's cached GPU state.
	const auto command = buffer.Handle();
	const auto range   = [](const ImageViewInfo& view) {
		return ImageSubresourceRange {view.base_level, 1, view.base_layer, 1};
	};
	source.Transit(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderRead, range(source_view),
	               command);
	destination.Transit(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderWrite,
	                    range(destination_view), command);
	const bool temporal_single_sample = std::ranges::all_of(dlss_bindings, [&](const auto& binding) {
		return !binding || cache.GetImage(binding->image_id).backing.samples == 1;
	});
	if (dlss_bindings[0] && dlss_bindings[1] && temporal_single_sample) {
		for (const auto& binding: dlss_bindings) {
			if (binding) {
				cache.GetImage(binding->image_id)
				    .Transit(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderRead,
					         range(binding->desc.view_info), command);
			}
		}
		auto exposure = dlss_bindings[2] ? PssrResource(*dlss_bindings[2], false) : Dlss::Resource {};
		Dlss::Inputs inputs {PssrResource(input, false), PssrResource(*dlss_bindings[0], false),
		                     PssrResource(*dlss_bindings[1], false),
		                     dlss_bindings[2] ? &exposure : nullptr, PssrResource(output, true)};
		bool reset_used = true;
		if (Upscale(command, request, inputs, reset_used)) {
			if (frame_generation) frame_generation->RecordNativePssr(buffer, request,
			    *dlss_bindings[0], *dlss_bindings[1], output, reset_used);
			++m_pssr_dlss_evaluations;
			if (m_pssr_dlss_evaluations <= 3 || m_pssr_requests % 600 == 0) {
				std::printf("PSSR DLSS: requests=%llu dlss=%llu linear=%llu input=%ux%u output=%ux%u\n",
				            static_cast<unsigned long long>(m_pssr_requests),
				            static_cast<unsigned long long>(m_pssr_dlss_evaluations),
				            static_cast<unsigned long long>(m_pssr_linear_fallbacks),
				            request.width, request.height, parameters.output_size[0], parameters.output_size[1]);
			}
			if (diagnose) {
				PssrDiagnosticReadback(m_context, output, diagnostic_prefix, "output-dlss", 0, 0,
				                         parameters.output_size[0], parameters.output_size[1]);
				// Keep the upscaler's original output-state contract after the probe's
				// transfer read. Recorder commands follow the native DLSS evaluation.
				destination.Transit(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderWrite,
				                    range(destination_view), buffer.Recorder());
			}
			ResetBindings();
			return;
		}
	}
	// Missing/unsupported temporal inputs must not reconnect to older history on
	// a later valid request from the same context.
	for (auto& context: m_dlss_contexts) {
		if (context.context == request.context) context.reset_pending = true;
	}
	++m_pssr_linear_fallbacks;
	if (m_pssr_linear_fallbacks <= 3 || m_pssr_requests % 600 == 0) {
		std::printf("PSSR linear fallback: requests=%llu dlss=%llu linear=%llu reason=%s\n",
		            static_cast<unsigned long long>(m_pssr_requests),
		            static_cast<unsigned long long>(m_pssr_dlss_evaluations),
		            static_cast<unsigned long long>(m_pssr_linear_fallbacks),
		            Dlss::Capabilities() == nullptr ? "backend-unavailable" :
		            !Dlss::Accepts(request) ? "temporal-inputs" :
		            !temporal_descriptors ? "temporal-descriptor" :
		            !temporal_single_sample ? "multisample-temporal-input" :
		            !dlss_bindings[0] || !dlss_bindings[1] ? "output-channel-mapping" : "resource-contract");
	}
	auto& graphics = m_context.GetGraphics();
	if (m_pssr_pipeline == nullptr) {
		const std::array bindings {
		    vk::DescriptorSetLayoutBinding {0, vk::DescriptorType::eSampledImage, 1,
		                                    vk::ShaderStageFlagBits::eCompute},
		    vk::DescriptorSetLayoutBinding {1, vk::DescriptorType::eStorageImage, 1,
		                                    vk::ShaderStageFlagBits::eCompute}};
		vk::DescriptorSetLayoutCreateInfo descriptor_info {};
		descriptor_info.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		descriptor_info.bindingCount = bindings.size();
		descriptor_info.pBindings = bindings.data();
		RequireVulkanSuccess(graphics.device.createDescriptorSetLayout(
		                         &descriptor_info, nullptr, &m_pssr_descriptor_layout),
		                     "create upscaling descriptor layout");
		const vk::PushConstantRange push {vk::ShaderStageFlagBits::eCompute, 0,
		                                  sizeof(PssrParameters)};
		vk::PipelineLayoutCreateInfo layout_info {};
		layout_info.setLayoutCount = 1;
		layout_info.pSetLayouts = &m_pssr_descriptor_layout;
		layout_info.pushConstantRangeCount = 1;
		layout_info.pPushConstantRanges = &push;
		RequireVulkanSuccess(graphics.device.createPipelineLayout(
		                         &layout_info, nullptr, &m_pssr_pipeline_layout),
		                     "create upscaling pipeline layout");
		const auto shader = CompileSPV(GPU_PSSR_LINEAR_SPV, graphics.device);
		vk::ComputePipelineCreateInfo create {};
		create.stage.stage = vk::ShaderStageFlagBits::eCompute;
		create.stage.module = shader;
		create.stage.pName = "main";
		create.layout = m_pssr_pipeline_layout;
		const auto result = graphics.device.createComputePipelines(
		    nullptr, 1, &create, nullptr, &m_pssr_pipeline);
		graphics.device.destroyShaderModule(shader, nullptr);
		RequireVulkanSuccess(result, "create upscaling pipeline");
	}
	const std::array images {
	    vk::DescriptorImageInfo {nullptr, input.image_view, vk::ImageLayout::eGeneral},
	    vk::DescriptorImageInfo {nullptr, output.image_view, vk::ImageLayout::eGeneral}};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t i = 0; i < writes.size(); ++i) {
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = i == 0 ? vk::DescriptorType::eSampledImage
		                                 : vk::DescriptorType::eStorageImage;
		writes[i].pImageInfo = &images[i];
	}
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pssr_pipeline_layout,
	                             0, writes.size(), writes.data());
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pssr_pipeline);
	command.pushConstants(m_pssr_pipeline_layout, vk::ShaderStageFlagBits::eCompute,
	                      0, sizeof(parameters), &parameters);
	command.dispatch((parameters.output_size[0] + 7) / 8,
	                 (parameters.output_size[1] + 7) / 8, 1);
	ShaderAccessBarrier(command, vk::PipelineStageFlagBits::eComputeShader);
	if (diagnose) {
		PssrDiagnosticReadback(m_context, output, diagnostic_prefix, "output-linear", 0, 0,
		                         parameters.output_size[0], parameters.output_size[1]);
		destination.Transit(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderWrite,
		                    range(destination_view), buffer.Recorder());
	}
	ResetBindings();
}

} // namespace Libs::Graphics
