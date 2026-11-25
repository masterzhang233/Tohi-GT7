// Shared, opt-in diagnostics only. Never mutate the caller's Vulkan structures.
#pragma once
#include <vulkan/vulkan_core.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace kyty_device_diagnostic {
using Json = nlohmann::ordered_json;
inline Json CoreFeatures(const VkPhysicalDeviceFeatures& value) {
    Json fields = Json::object();
    fields["robustBufferAccess"] = value.robustBufferAccess;
    fields["fullDrawIndexUint32"] = value.fullDrawIndexUint32;
    fields["imageCubeArray"] = value.imageCubeArray;
    fields["independentBlend"] = value.independentBlend;
    fields["geometryShader"] = value.geometryShader;
    fields["tessellationShader"] = value.tessellationShader;
    fields["sampleRateShading"] = value.sampleRateShading;
    fields["dualSrcBlend"] = value.dualSrcBlend;
    fields["logicOp"] = value.logicOp;
    fields["multiDrawIndirect"] = value.multiDrawIndirect;
    fields["drawIndirectFirstInstance"] = value.drawIndirectFirstInstance;
    fields["depthClamp"] = value.depthClamp;
    fields["depthBiasClamp"] = value.depthBiasClamp;
    fields["fillModeNonSolid"] = value.fillModeNonSolid;
    fields["depthBounds"] = value.depthBounds;
    fields["wideLines"] = value.wideLines;
    fields["largePoints"] = value.largePoints;
    fields["alphaToOne"] = value.alphaToOne;
    fields["multiViewport"] = value.multiViewport;
    fields["samplerAnisotropy"] = value.samplerAnisotropy;
    fields["textureCompressionETC2"] = value.textureCompressionETC2;
    fields["textureCompressionASTC_LDR"] = value.textureCompressionASTC_LDR;
    fields["textureCompressionBC"] = value.textureCompressionBC;
    fields["occlusionQueryPrecise"] = value.occlusionQueryPrecise;
    fields["pipelineStatisticsQuery"] = value.pipelineStatisticsQuery;
    fields["vertexPipelineStoresAndAtomics"] = value.vertexPipelineStoresAndAtomics;
    fields["fragmentStoresAndAtomics"] = value.fragmentStoresAndAtomics;
    fields["shaderTessellationAndGeometryPointSize"] = value.shaderTessellationAndGeometryPointSize;
    fields["shaderImageGatherExtended"] = value.shaderImageGatherExtended;
    fields["shaderStorageImageExtendedFormats"] = value.shaderStorageImageExtendedFormats;
    fields["shaderStorageImageMultisample"] = value.shaderStorageImageMultisample;
    fields["shaderStorageImageReadWithoutFormat"] = value.shaderStorageImageReadWithoutFormat;
    fields["shaderStorageImageWriteWithoutFormat"] = value.shaderStorageImageWriteWithoutFormat;
    fields["shaderUniformBufferArrayDynamicIndexing"] = value.shaderUniformBufferArrayDynamicIndexing;
    fields["shaderSampledImageArrayDynamicIndexing"] = value.shaderSampledImageArrayDynamicIndexing;
    fields["shaderStorageBufferArrayDynamicIndexing"] = value.shaderStorageBufferArrayDynamicIndexing;
    fields["shaderStorageImageArrayDynamicIndexing"] = value.shaderStorageImageArrayDynamicIndexing;
    fields["shaderClipDistance"] = value.shaderClipDistance;
    fields["shaderCullDistance"] = value.shaderCullDistance;
    fields["shaderFloat64"] = value.shaderFloat64;
    fields["shaderInt64"] = value.shaderInt64;
    fields["shaderInt16"] = value.shaderInt16;
    fields["shaderResourceResidency"] = value.shaderResourceResidency;
    fields["shaderResourceMinLod"] = value.shaderResourceMinLod;
    fields["sparseBinding"] = value.sparseBinding;
    fields["sparseResidencyBuffer"] = value.sparseResidencyBuffer;
    fields["sparseResidencyImage2D"] = value.sparseResidencyImage2D;
    fields["sparseResidencyImage3D"] = value.sparseResidencyImage3D;
    fields["sparseResidency2Samples"] = value.sparseResidency2Samples;
    fields["sparseResidency4Samples"] = value.sparseResidency4Samples;
    fields["sparseResidency8Samples"] = value.sparseResidency8Samples;
    fields["sparseResidency16Samples"] = value.sparseResidency16Samples;
    fields["sparseResidencyAliased"] = value.sparseResidencyAliased;
    fields["variableMultisampleRate"] = value.variableMultisampleRate;
    fields["inheritedQueries"] = value.inheritedQueries;
    return fields;
}
inline Json NextChain(const void* pointer) {
    Json nodes = Json::array();
    const auto* base = static_cast<const VkBaseInStructure*>(pointer);
    for (unsigned count = 0; base && count < 64; ++count, base = base->pNext) {
        Json node = {{"sType", static_cast<int>(base->sType)}};
        Json fields = Json::object();
        switch (base->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR: {
            const auto& value = *reinterpret_cast<const VkPhysicalDevicePresentIdFeaturesKHR*>(base);
            node["type"] = "VkPhysicalDevicePresentIdFeaturesKHR";
            fields["presentId"] = value.presentId;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceVulkan11Features*>(base);
            node["type"] = "VkPhysicalDeviceVulkan11Features";
            fields["storageBuffer16BitAccess"] = value.storageBuffer16BitAccess;
            fields["uniformAndStorageBuffer16BitAccess"] = value.uniformAndStorageBuffer16BitAccess;
            fields["storagePushConstant16"] = value.storagePushConstant16;
            fields["storageInputOutput16"] = value.storageInputOutput16;
            fields["multiview"] = value.multiview;
            fields["multiviewGeometryShader"] = value.multiviewGeometryShader;
            fields["multiviewTessellationShader"] = value.multiviewTessellationShader;
            fields["variablePointersStorageBuffer"] = value.variablePointersStorageBuffer;
            fields["variablePointers"] = value.variablePointers;
            fields["protectedMemory"] = value.protectedMemory;
            fields["samplerYcbcrConversion"] = value.samplerYcbcrConversion;
            fields["shaderDrawParameters"] = value.shaderDrawParameters;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(base);
            node["type"] = "VkPhysicalDeviceVulkan12Features";
            fields["samplerMirrorClampToEdge"] = value.samplerMirrorClampToEdge;
            fields["drawIndirectCount"] = value.drawIndirectCount;
            fields["storageBuffer8BitAccess"] = value.storageBuffer8BitAccess;
            fields["uniformAndStorageBuffer8BitAccess"] = value.uniformAndStorageBuffer8BitAccess;
            fields["storagePushConstant8"] = value.storagePushConstant8;
            fields["shaderBufferInt64Atomics"] = value.shaderBufferInt64Atomics;
            fields["shaderSharedInt64Atomics"] = value.shaderSharedInt64Atomics;
            fields["shaderFloat16"] = value.shaderFloat16;
            fields["shaderInt8"] = value.shaderInt8;
            fields["descriptorIndexing"] = value.descriptorIndexing;
            fields["shaderInputAttachmentArrayDynamicIndexing"] = value.shaderInputAttachmentArrayDynamicIndexing;
            fields["shaderUniformTexelBufferArrayDynamicIndexing"] = value.shaderUniformTexelBufferArrayDynamicIndexing;
            fields["shaderStorageTexelBufferArrayDynamicIndexing"] = value.shaderStorageTexelBufferArrayDynamicIndexing;
            fields["shaderUniformBufferArrayNonUniformIndexing"] = value.shaderUniformBufferArrayNonUniformIndexing;
            fields["shaderSampledImageArrayNonUniformIndexing"] = value.shaderSampledImageArrayNonUniformIndexing;
            fields["shaderStorageBufferArrayNonUniformIndexing"] = value.shaderStorageBufferArrayNonUniformIndexing;
            fields["shaderStorageImageArrayNonUniformIndexing"] = value.shaderStorageImageArrayNonUniformIndexing;
            fields["shaderInputAttachmentArrayNonUniformIndexing"] = value.shaderInputAttachmentArrayNonUniformIndexing;
            fields["shaderUniformTexelBufferArrayNonUniformIndexing"] = value.shaderUniformTexelBufferArrayNonUniformIndexing;
            fields["shaderStorageTexelBufferArrayNonUniformIndexing"] = value.shaderStorageTexelBufferArrayNonUniformIndexing;
            fields["descriptorBindingUniformBufferUpdateAfterBind"] = value.descriptorBindingUniformBufferUpdateAfterBind;
            fields["descriptorBindingSampledImageUpdateAfterBind"] = value.descriptorBindingSampledImageUpdateAfterBind;
            fields["descriptorBindingStorageImageUpdateAfterBind"] = value.descriptorBindingStorageImageUpdateAfterBind;
            fields["descriptorBindingStorageBufferUpdateAfterBind"] = value.descriptorBindingStorageBufferUpdateAfterBind;
            fields["descriptorBindingUniformTexelBufferUpdateAfterBind"] = value.descriptorBindingUniformTexelBufferUpdateAfterBind;
            fields["descriptorBindingStorageTexelBufferUpdateAfterBind"] = value.descriptorBindingStorageTexelBufferUpdateAfterBind;
            fields["descriptorBindingUpdateUnusedWhilePending"] = value.descriptorBindingUpdateUnusedWhilePending;
            fields["descriptorBindingPartiallyBound"] = value.descriptorBindingPartiallyBound;
            fields["descriptorBindingVariableDescriptorCount"] = value.descriptorBindingVariableDescriptorCount;
            fields["runtimeDescriptorArray"] = value.runtimeDescriptorArray;
            fields["samplerFilterMinmax"] = value.samplerFilterMinmax;
            fields["scalarBlockLayout"] = value.scalarBlockLayout;
            fields["imagelessFramebuffer"] = value.imagelessFramebuffer;
            fields["uniformBufferStandardLayout"] = value.uniformBufferStandardLayout;
            fields["shaderSubgroupExtendedTypes"] = value.shaderSubgroupExtendedTypes;
            fields["separateDepthStencilLayouts"] = value.separateDepthStencilLayouts;
            fields["hostQueryReset"] = value.hostQueryReset;
            fields["timelineSemaphore"] = value.timelineSemaphore;
            fields["bufferDeviceAddress"] = value.bufferDeviceAddress;
            fields["bufferDeviceAddressCaptureReplay"] = value.bufferDeviceAddressCaptureReplay;
            fields["bufferDeviceAddressMultiDevice"] = value.bufferDeviceAddressMultiDevice;
            fields["vulkanMemoryModel"] = value.vulkanMemoryModel;
            fields["vulkanMemoryModelDeviceScope"] = value.vulkanMemoryModelDeviceScope;
            fields["vulkanMemoryModelAvailabilityVisibilityChains"] = value.vulkanMemoryModelAvailabilityVisibilityChains;
            fields["shaderOutputViewportIndex"] = value.shaderOutputViewportIndex;
            fields["shaderOutputLayer"] = value.shaderOutputLayer;
            fields["subgroupBroadcastDynamicId"] = value.subgroupBroadcastDynamicId;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceVulkan13Features*>(base);
            node["type"] = "VkPhysicalDeviceVulkan13Features";
            fields["robustImageAccess"] = value.robustImageAccess;
            fields["inlineUniformBlock"] = value.inlineUniformBlock;
            fields["descriptorBindingInlineUniformBlockUpdateAfterBind"] = value.descriptorBindingInlineUniformBlockUpdateAfterBind;
            fields["pipelineCreationCacheControl"] = value.pipelineCreationCacheControl;
            fields["privateData"] = value.privateData;
            fields["shaderDemoteToHelperInvocation"] = value.shaderDemoteToHelperInvocation;
            fields["shaderTerminateInvocation"] = value.shaderTerminateInvocation;
            fields["subgroupSizeControl"] = value.subgroupSizeControl;
            fields["computeFullSubgroups"] = value.computeFullSubgroups;
            fields["synchronization2"] = value.synchronization2;
            fields["textureCompressionASTC_HDR"] = value.textureCompressionASTC_HDR;
            fields["shaderZeroInitializeWorkgroupMemory"] = value.shaderZeroInitializeWorkgroupMemory;
            fields["dynamicRendering"] = value.dynamicRendering;
            fields["shaderIntegerDotProduct"] = value.shaderIntegerDotProduct;
            fields["maintenance4"] = value.maintenance4;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceColorWriteEnableFeaturesEXT*>(base);
            node["type"] = "VkPhysicalDeviceColorWriteEnableFeaturesEXT";
            fields["colorWriteEnable"] = value.colorWriteEnable;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceDepthClipEnableFeaturesEXT*>(base);
            node["type"] = "VkPhysicalDeviceDepthClipEnableFeaturesEXT";
            fields["depthClipEnable"] = value.depthClipEnable;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceImageViewMinLodFeaturesEXT*>(base);
            node["type"] = "VkPhysicalDeviceImageViewMinLodFeaturesEXT";
            fields["minLod"] = value.minLod;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceDepthClipControlFeaturesEXT*>(base);
            node["type"] = "VkPhysicalDeviceDepthClipControlFeaturesEXT";
            fields["depthClipControl"] = value.depthClipControl;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR*>(base);
            node["type"] = "VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR";
            fields["workgroupMemoryExplicitLayout"] = value.workgroupMemoryExplicitLayout;
            fields["workgroupMemoryExplicitLayoutScalarBlockLayout"] = value.workgroupMemoryExplicitLayoutScalarBlockLayout;
            fields["workgroupMemoryExplicitLayout8BitAccess"] = value.workgroupMemoryExplicitLayout8BitAccess;
            fields["workgroupMemoryExplicitLayout16BitAccess"] = value.workgroupMemoryExplicitLayout16BitAccess;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR*>(base);
            node["type"] = "VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR";
            fields["fragmentShaderBarycentric"] = value.fragmentShaderBarycentric;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceRobustness2FeaturesEXT*>(base);
            node["type"] = "VkPhysicalDeviceRobustness2FeaturesEXT";
            fields["robustBufferAccess2"] = value.robustBufferAccess2;
            fields["robustImageAccess2"] = value.robustImageAccess2;
            fields["nullDescriptor"] = value.nullDescriptor;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceMeshShaderFeaturesEXT*>(base);
            node["type"] = "VkPhysicalDeviceMeshShaderFeaturesEXT";
            fields["taskShader"] = value.taskShader;
            fields["meshShader"] = value.meshShader;
            fields["multiviewMeshShader"] = value.multiviewMeshShader;
            fields["primitiveFragmentShadingRateMeshShader"] = value.primitiveFragmentShadingRateMeshShader;
            fields["meshShaderQueries"] = value.meshShaderQueries;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT*>(base);
            node["type"] = "VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT";
            fields["attachmentFeedbackLoopDynamicState"] = value.attachmentFeedbackLoopDynamicState;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT*>(base);
            node["type"] = "VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT";
            fields["attachmentFeedbackLoopLayout"] = value.attachmentFeedbackLoopLayout;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceProvokingVertexFeaturesEXT*>(base);
            node["type"] = "VkPhysicalDeviceProvokingVertexFeaturesEXT";
            fields["provokingVertexLast"] = value.provokingVertexLast;
            fields["transformFeedbackPreservesProvokingVertex"] = value.transformFeedbackPreservesProvokingVertex;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT*>(base);
            node["type"] = "VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT";
            fields["shaderImageInt64Atomics"] = value.shaderImageInt64Atomics;
            fields["sparseImageInt64Atomics"] = value.sparseImageInt64Atomics;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceShaderClockFeaturesKHR*>(base);
            node["type"] = "VkPhysicalDeviceShaderClockFeaturesKHR";
            fields["shaderSubgroupClock"] = value.shaderSubgroupClock;
            fields["shaderDeviceClock"] = value.shaderDeviceClock;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COVERAGE_REDUCTION_MODE_FEATURES_NV: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceCoverageReductionModeFeaturesNV*>(base);
            node["type"] = "VkPhysicalDeviceCoverageReductionModeFeaturesNV";
            fields["coverageReductionMode"] = value.coverageReductionMode;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceFaultFeaturesEXT*>(base);
            node["type"] = "VkPhysicalDeviceFaultFeaturesEXT";
            fields["deviceFault"] = value.deviceFault;
            fields["deviceFaultVendorBinary"] = value.deviceFaultVendorBinary;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DIAGNOSTICS_CONFIG_FEATURES_NV: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceDiagnosticsConfigFeaturesNV*>(base);
            node["type"] = "VkPhysicalDeviceDiagnosticsConfigFeaturesNV";
            fields["diagnosticsConfig"] = value.diagnosticsConfig;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceAddressBindingReportFeaturesEXT*>(base);
            node["type"] = "VkPhysicalDeviceAddressBindingReportFeaturesEXT";
            fields["reportAddressBinding"] = value.reportAddressBinding;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2: {
            const auto& value = *reinterpret_cast<const VkPhysicalDeviceFeatures2*>(base);
            node["type"] = "VkPhysicalDeviceFeatures2";
            fields = CoreFeatures(value.features);
            break;
        }
        case VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV: {
            const auto& value = *reinterpret_cast<const VkDeviceDiagnosticsConfigCreateInfoNV*>(base);
            node["type"] = "VkDeviceDiagnosticsConfigCreateInfoNV";
            fields["flags"] = value.flags;
            break;
        }
        default:
            node["type"] = "unknown";
            node["fields_complete"] = false;
            break;
        }
        node["fields"] = std::move(fields);
        nodes.push_back(std::move(node));
    }
    if (base) nodes.push_back({{"error", "pNext chain exceeds diagnostic limit"}});
    return nodes;
}
inline Json Names(uint32_t count, const char* const* names, bool sort = true) {
    std::vector<std::string> ordered;
    for (uint32_t index = 0; index < count; ++index)
        ordered.emplace_back(names && names[index] ? names[index] : "<null>");
    if (sort) std::sort(ordered.begin(), ordered.end());
    return ordered; // Keep duplicates; stable ordering ignores semantically irrelevant order.
}
#if defined(_WIN32)
inline Json AddressSpace() {
    Json map = Json::array();
    SYSTEM_INFO system {};
    GetSystemInfo(&system);
    const auto limit = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
    uintptr_t next = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    for (unsigned count = 0; next <= limit && count < 65536; ++count) {
        MEMORY_BASIC_INFORMATION info {};
        if (!VirtualQuery(reinterpret_cast<const void*>(next), &info, sizeof(info))) break;
        const auto base = reinterpret_cast<uintptr_t>(info.BaseAddress);
        map.push_back({{"base", base}, {"size", info.RegionSize},
                       {"allocation_base", reinterpret_cast<uintptr_t>(info.AllocationBase)},
                       {"state", info.State}, {"type", info.Type}, {"protect", info.Protect}});
        const auto end = base + info.RegionSize;
        if (end <= next) break;
        next = end;
    }
    return {{"image_base", reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))},
            {"regions", map}};
}
#endif
inline void WriteDeviceCreateReport(const VkDeviceCreateInfo& info) noexcept {
#if defined(_WIN32)
    wchar_t requested[32768] {};
    const auto requested_length = GetEnvironmentVariableW(L"KYTY_DEVICE_CREATE_REPORT", requested, 32768);
    if (!requested_length || requested_length >= 32768) return;
#else
    const char* requested = std::getenv("KYTY_DEVICE_CREATE_REPORT");
    if (!requested || !*requested) return;
#endif
    try {
        Json report = {{"schema", 1}, {"kind", "VkDeviceCreateInfo before vkCreateDevice"},
                       {"sType", static_cast<int>(info.sType)}, {"flags", info.flags}};
        report["extensions"] = Names(info.enabledExtensionCount, info.ppEnabledExtensionNames);
        report["extension_order"] = Names(info.enabledExtensionCount, info.ppEnabledExtensionNames, false);
        report["layers"] = Names(info.enabledLayerCount, info.ppEnabledLayerNames);
        report["core_features"] = info.pEnabledFeatures ? CoreFeatures(*info.pEnabledFeatures) : Json(nullptr);
        report["pNext"] = NextChain(info.pNext);
        Json queues = Json::array();
        for (uint32_t index = 0; index < info.queueCreateInfoCount; ++index) {
            const auto& queue = info.pQueueCreateInfos[index];
            Json priorities = Json::array();
            for (uint32_t q = 0; q < queue.queueCount; ++q)
                priorities.push_back(queue.pQueuePriorities ? Json(queue.pQueuePriorities[q]) : Json(nullptr));
            queues.push_back({{"sType", static_cast<int>(queue.sType)}, {"flags", queue.flags},
                              {"family", queue.queueFamilyIndex}, {"count", queue.queueCount},
                              {"priorities", priorities}, {"pNext", NextChain(queue.pNext)}});
        }
        report["queues"] = std::move(queues);
#if defined(_WIN32)
        report["address_space"] = AddressSpace();
        const auto output = std::filesystem::path(requested);
#else
        const auto output = std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(requested)));
#endif
        if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
        std::ofstream file(output, std::ios::trunc);
        file << report.dump(2) << '\n';
        if (!file) throw std::runtime_error("could not write report");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Vulkan device diagnostic: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "Vulkan device diagnostic: unknown report error\n");
    }
}
} // namespace kyty_device_diagnostic
