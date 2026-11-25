  // Explicit opt-in only: production PM4 -> texture cache -> PSSR renderer ->
  // real native NGX. No game files, window, save data or shader captures.
  void CheckPssrNative1080To4K() {
    constexpr const char *name = "PssrNative1080To4K";
    constexpr uintptr_t base = 0x000000020b000000ull;
    constexpr uint64_t allocation_size = 0x8000000;
    constexpr uint64_t input_slot_size = 0x1000000;
    constexpr u32 input_width = 1920, input_height = 1080;
    constexpr u32 output_width = 3840, output_height = 2160;
    constexpr u32 frames = 8;
    constexpr auto linear = Prospero::TileMode::kLinear;
    constexpr auto packed = Prospero::BufferFormat::k11_11_10Float;
    const auto descriptor = [](uint64_t address, u32 width, u32 height,
                               Prospero::BufferFormat format, Prospero::TileMode tile) {
      ShaderTextureResource result{};
      result.fields[0] = static_cast<u32>(address >> 8);
      result.fields[1] = static_cast<u32>(address >> 40) |
          (static_cast<u32>(format) << 20) | (((width - 1) & 3) << 30);
      result.fields[2] = ((width - 1) >> 2) | ((height - 1) << 14);
      result.fields[3] = DstSel(4, 5, 6, 7) | (static_cast<u32>(tile) << 20) |
          (static_cast<u32>(Prospero::ImageType::kColor2D) << 28);
      result.fields[5] = 0x00700000;
      return result;
    };
    const auto half = [](float value) {
      const auto bits = std::bit_cast<u32>(value);
      const auto exponent = static_cast<int>((bits >> 23u) & 255u) - 127 + 15;
      return static_cast<uint16_t>((static_cast<u32>(exponent) << 10u) |
                                   ((bits >> 13u) & 1023u));
    };
    const auto unpack = [](u32 word, u32 channel) {
      const auto bits = channel == 2 ? 5u : 6u;
      const auto shifted = word >> (channel == 0 ? 0u : channel == 1 ? 11u : 22u);
      const auto exponent = (shifted >> bits) & 31u;
      const auto mantissa = shifted & ((1u << bits) - 1u);
      return exponent == 31u ? std::numeric_limits<float>::quiet_NaN() :
          std::ldexp(static_cast<float>(mantissa + (exponent ? 1u << bits : 0u)),
                     static_cast<int>(exponent ? exponent : 1u) - 15 - static_cast<int>(bits));
    };
    EnsureRuntimeContext();
    Require(name, "native host contract", m_runtime_context.gt7_dlss_device_enabled &&
        Gt7DlssHost::ApiPssr() != nullptr, "native device/ABI was not enabled");
    int64_t physical = -1;
    Require(name, "allocate", LibKernel::Memory::KernelAllocateDirectMemory(
        0, LibKernel::Memory::KernelGetDirectMemorySize(), allocation_size, 0x10000, 0, &physical) == 0,
        "1080p fixture allocation failed");
    void *mapped = reinterpret_cast<void *>(base);
    Require(name, "map", LibKernel::Memory::KernelMapDirectMemory(
        &mapped, allocation_size, 0x3, 0x10, physical, 0x10000) == 0 &&
        mapped == reinterpret_cast<void *>(base), "1080p fixture mapping failed");
    std::memset(mapped, 0, allocation_size);
    RenderContext context(m_runtime_context);
    context.InitializeGpu(nullptr);
    LibKernel::Memory::InstallGpuResources(&context);
    context.GetGpu().SendCommandSync([&] {
      CommandProcessor processor(context, 0);
      processor.BufferInit();
      context.MapMemory(base, allocation_size);
      auto &executor = context.GetRenderExecutor();
      auto &cache = context.GetTextureCache();
      auto &scheduler = context.GetCommandScheduler();
      const auto input = descriptor(base, input_width, input_height, packed, linear);
      const auto depth = descriptor(base + input_slot_size, input_width, input_height,
                                    Prospero::BufferFormat::k32Float, linear);
      const auto motion = descriptor(base + 2 * input_slot_size, input_width, input_height,
                                     Prospero::BufferFormat::k32_32Float, linear);
      const auto output = descriptor(base + 4 * input_slot_size, output_width, output_height,
                                     packed, Prospero::TileMode::kStandard256B);
      const auto write_linear = [&](const ShaderTextureResource &texture, auto fill) {
        TileSizeAlign total{};
        TileSizeOffset offsets[16]{};
        TilePaddedSize padded[16]{};
        TileGetTextureTotalSize(texture.Format(), input_width, input_height, 1, 1,
                                linear, false, total);
        TileGetTextureSize(texture.Format(), input_width, input_height, 1,
                           linear, nullptr, offsets, padded);
        Require(name, "fixture bounds", total.size != 0 && total.size <= input_slot_size,
                "1080p input exceeds its independent guest allocation slot");
        std::vector<uint8_t> bytes(total.size);
        const auto bytes_per_texel = Prospero::NumBytesPerElement(texture.Format());
        for (u32 y = 0; y < input_height; ++y) for (u32 x = 0; x < input_width; ++x)
          fill(bytes.data() + (uint64_t{y} * padded[0].width + x) * bytes_per_texel, x, y);
        Require(name, "invalidate fixture", context.InvalidateMemory(texture.Base40(), total.size),
                "fixture is outside mapped guest memory");
        LibKernel::Memory::WriteBacking(texture.Base40(), bytes.data(), total.size);
      };
      const auto resolve = [&](const ShaderTextureResource &texture) {
        ShaderRecompiler::IR::ImageResource resource{};
        resource.resource_class = ShaderRecompiler::IR::ImageResourceClass::Sampled;
        resource.numeric_class = Prospero::TextureNumericClass::Float;
        resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
        resource.read = true;
        ShaderRecompiler::IR::DescriptorValue value{};
        value.dword_count = 8;
        std::copy_n(texture.fields, 8, value.dwords.begin());
        return RenderExecutorTestAccess::ResolveTexture(executor, resource, value);
      };
      write_linear(depth, [](uint8_t *target, u32, u32) {
        constexpr float reversed_depth = .5f;
        std::memcpy(target, &reversed_depth, sizeof(reversed_depth));
      });
      Pssr::Request request{};
      std::copy_n(input.fields, 8, request.input);
      std::copy_n(depth.fields, 8, request.depth);
      std::copy_n(motion.fields, 8, request.motion);
      std::copy_n(output.fields, 8, request.output);
      request.context = 0x50535352344b3130ull;
      request.width = input_width;
      request.height = input_height;
      request.pre_exposure = 1;
      // Match the game's normalized motion convention. The actual pixel motion
      // below is zero, then -1 input pixel towards the previous frame, without jitter.
      request.motion_scale[0] = -.5f * input_width;
      request.motion_scale[1] = .5f * input_height;

      // Observe the real ABI boundary without mocking NGX execution. The wrapper
      // delegates every record to the loaded backend and checks its real result.
      // ProcessPssr initializes lazily on its first request. Hold one explicit,
      // balanced production session reference so the observer covers that first
      // evaluation too; the renderer still acquires and releases its own owner.
      Dlss::Init(context.GetGraphics(), VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
                 VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr);
      struct NativeSessionReference {
        ~NativeSessionReference() { Dlss::Shutdown(); }
      } native_session_reference;
      auto *capabilities = Dlss::Capabilities();
      Require(name, "native session", capabilities && capabilities->api && capabilities->session,
              "native renderer did not establish a backend session");
      static const KytyDlssPssrApi *backend_api = nullptr;
      static const Pssr::Request *expected_request = nullptr;
      static uint64_t checked_evaluations = 0;
      backend_api = capabilities->api;
      expected_request = &request;
      checked_evaluations = 0;
      auto observed_api = *backend_api;
      observed_api.record_evaluate = +[](KytyDlssPssrFeature *feature,
                                       const KytyDlssPssrEvaluateInfo *evaluate,
                                       KytyDlssRecordResult *result, KytyDlssError *error) {
        constexpr const char *label = "PssrNative1080To4K";
        const auto &r = *expected_request;
        Require(label, "actual NGX extent", evaluate && evaluate->x == 0 && evaluate->y == 0 &&
                    evaluate->width == 1920 && evaluate->height == 1080 &&
                    evaluate->color.width == 1920 && evaluate->color.height == 1080 &&
                    evaluate->depth.width == 1920 && evaluate->depth.height == 1080 &&
                    evaluate->motion.width == 1920 && evaluate->motion.height == 1080 &&
                    evaluate->output.width == 3840 && evaluate->output.height == 2160,
                "NGX received a cropped larger backing or an incorrect upscaling ratio");
        Require(label, "actual NGX temporal inputs",
                    evaluate->jitter_x == r.jitter[0] && evaluate->jitter_y == r.jitter[1] &&
                    evaluate->motion_scale_x == r.motion_scale[0] &&
                    evaluate->motion_scale_y == r.motion_scale[1] &&
                    evaluate->pre_exposure == 1 && evaluate->exposure_scale == 1 &&
                    evaluate->exposure == nullptr,
                "jitter, low-resolution motion or exposure changed before the native ABI");
        Require(label, "actual NGX resource formats",
                    evaluate->color.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 &&
                    evaluate->output.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 &&
                    evaluate->depth.format == VK_FORMAT_R32_SFLOAT &&
                    evaluate->motion.format == VK_FORMAT_R32G32_SFLOAT,
                "native color, depth or motion format differs from the generated fixture");
        const auto status = backend_api->record_evaluate(feature, evaluate, result, error);
        Require(label, "actual NGX record", status == KYTY_DLSS_OK,
                "real backend rejected 1080p-to-4K evaluation");
        Require(label, "actual NGX reset", result->reset_used ==
                    (checked_evaluations == 0 || (r.flags & Pssr::kReset) != 0) &&
                    result->feature_created == (checked_evaluations == 0) &&
                    result->evaluations_recorded == checked_evaluations + 1,
                "backend history creation/reset/evaluation sequence differs from the request");
        ++checked_evaluations;
        return status;
      };
      capabilities->api = &observed_api;
      struct RestoreApi {
        Dlss::Parameter *capabilities;
        const KytyDlssPssrApi *original;
        ~RestoreApi() { capabilities->api = original; }
      } restore_api {capabilities, backend_api};

      constexpr std::array<std::array<float, 2>, frames> jitter {{
          {.125f, -.125f}, {-.125f, .375f}, {.375f, .125f}, {-.375f, -.375f},
          {.1875f, .01851848f}, {-.0625f, -.31481481f}, {.4375f, .24074070f},
          {-.46875f, -.09259260f}}};
      Dlss::Feature *history = nullptr;
      Dlss::Creation creation{};
      for (u32 frame = 0; frame < frames; ++frame) {
        request.flags = Pssr::kReverseDepth | ((frame == 0 || frame == 4) ? Pssr::kReset : 0u);
        request.jitter[0] = jitter[frame][0];
        request.jitter[1] = jitter[frame][1];
        const float displacement = frame > 4 ? static_cast<float>(frame - 4) : 0.f;
        write_linear(motion, [&](uint8_t *target, u32, u32) {
          const std::array<float, 2> value {frame > 4 ? -1.f / request.motion_scale[0] : 0.f, 0.f};
          std::memcpy(target, value.data(), sizeof(value));
        });
        write_linear(input, [&](uint8_t *target, u32 x, u32 y) {
          // Produce the full scene at 1920x1080. There is no 4K input image,
          // crop, active-subrect reduction or hidden downsample in this fixture.
          const float u = (float(x) + .5f - request.jitter[0] - displacement) / input_width;
          const float v = (float(y) + .5f - request.jitter[1]) / input_height;
          const auto r = half(.2f + .4f * u), g = half(.25f + .3f * v), b = half(.3f);
          const u32 value = (u32{r} >> 4u) | ((u32{g} >> 4u) << 11u) | ((u32{b} >> 5u) << 22u);
          std::memcpy(target, &value, sizeof(value));
        });
        std::vector<u32> packet(Pssr::RequestDwords + 1);
        packet[0] = KYTY_PM4(packet.size(), Pm4::IT_NOP, Pm4::R_PSSR);
        std::memcpy(packet.data() + 1, &request, sizeof(request));
        Pm4Execution execution;
        Require(name, "native packet", processor.Process(execution, packet) == Pm4ProcessResult::Complete,
                "1080p native PM4 packet did not complete");
        const uint64_t count = frame + 1;
        Require(name, "real NGX count", checked_evaluations == count &&
                    RenderExecutorTestAccess::PssrCounts(executor) ==
                        std::array<uint64_t, 3>{count, count, 0},
                "1080p request did not execute native DLSS or used linear fallback");
        if (!history) {
          history = RenderExecutorTestAccess::PssrFeature(executor);
          creation = RenderExecutorTestAccess::PssrCreation(executor);
          KytyDlssOptimalSettings optimal {sizeof(optimal)};
          KytyDlssError error{};
          Require(name, "runtime range", backend_api->query_optimal(capabilities->session,
                      output_width, output_height, creation.mode, &optimal, &error) == KYTY_DLSS_OK &&
                      optimal.min_width <= input_width && optimal.max_width >= input_width &&
                      optimal.min_height <= input_height && optimal.max_height >= input_height,
                  "chosen feature does not support the real 1080p input");
          Require(name, "native creation", creation.output_width == output_width &&
                      creation.output_height == output_height && creation.flags == Pssr::kReverseDepth,
                  "4K output or reversed low-resolution depth contract changed");
          std::printf("[native4k] runtime mode=%d optimal=%ux%u range=%ux%u..%ux%u actual=1920x1080 output=3840x2160\n",
                      creation.mode, optimal.optimal_width, optimal.optimal_height,
                      optimal.min_width, optimal.min_height, optimal.max_width, optimal.max_height);
          for (const auto *texture : {&input, &depth, &motion, &output}) {
            const auto binding = resolve(*texture);
            const auto &image = cache.GetImage(binding.image_id);
            const bool is_output = texture == &output;
            Require(name, "physical backing extent",
                        image.backing.extent.width == (is_output ? output_width : input_width) &&
                        image.backing.extent.height == (is_output ? output_height : input_height) &&
                        binding.desc.view_info.base_level == 0 && binding.desc.view_info.base_layer == 0,
                    "physical image is a larger backing, cropped view or wrong subresource");
          }
        }
        Require(name, "stable native history", history &&
                    RenderExecutorTestAccess::PssrFeature(executor) == history &&
                    RenderExecutorTestAccess::PssrCreation(executor) == creation,
                "fixed-resolution evaluation unexpectedly replaced its native history");
        const auto binding = resolve(output);
        const auto pixels = ReadCachedTexel(name, context, binding.image_id, {},
            {output_width, output_height, 1}, binding.desc.view_info.base_layer,
            binding.desc.view_info.base_level);
        Require(name, "4K readback size", pixels.size() == size_t{output_width} * output_height,
                "native output did not contain the complete 4K image");
        double error = 0;
        float max_error = 0;
        uint64_t measured = 0;
        for (u32 y = 0; y < output_height; ++y) for (u32 x = 0; x < output_width; ++x)
          for (u32 channel = 0; channel < 3; ++channel) {
            const float value = unpack(pixels[size_t{y} * output_width + x], channel);
            Require(name, "finite output", std::isfinite(value), "4K output contains NaN/Inf");
            if (x < 16 || y < 16 || x + 16 >= output_width || y + 16 >= output_height) continue;
            const float expected = channel == 0 ? .2f + .4f *
                ((float(x) + .5f) / output_width - displacement / input_width) :
                channel == 1 ? .25f + .3f * (float(y) + .5f) / output_height : .3f;
            const float delta = std::abs(value - expected);
            error += delta;
            max_error = std::max(max_error, delta);
            ++measured;
          }
        const double mean_error = error / measured;
        std::printf("[native4k] frame=%u recorded=%llu reset=%u jitter=%.9g,%.9g motion_pixels=%.9g,0 MAE=%.9g max=%.9g\n",
                    frame, static_cast<unsigned long long>(checked_evaluations),
                    frame == 0 || frame == 4, request.jitter[0], request.jitter[1],
                    frame > 4 ? -1.f : 0.f, mean_error, max_error);
        Require(name, "4K HDR ramp", mean_error < .05 && max_error < .2f,
                "native 1080p-to-4K output does not reproduce the known scene brightness");
      }
      Require(name, "final count", checked_evaluations == frames,
              "missing real 1080p-to-4K evaluations");
      context.UnmapMemory(base, allocation_size);
      scheduler.Finish();
      expected_request = nullptr;
    });
    LibKernel::Memory::InstallGpuResources(nullptr);
    context.ShutdownGpu();
    Require(name, "release", LibKernel::Memory::KernelCheckedReleaseDirectMemory(
        physical, allocation_size) == 0, "1080p fixture allocation release failed");
    std::printf("[gpu]     %-32s ok (8 native 1920x1080 -> 3840x2160 evaluations, no crop/fallback, jitter/motion/reset/HDR)\n", name);
  }

  void CheckPssrNative() {
    constexpr const char *name = "PssrNative";
    constexpr uintptr_t base = 0x0000000207000000ull;
    constexpr uint64_t allocation_size = 0x1000000;
    constexpr u32 input_width = 656, input_height = 376;
    constexpr u32 output_width = 640, output_height = 360;
    constexpr auto linear = Prospero::TileMode::kLinear;
    constexpr auto tiled = Prospero::TileMode::kStandard256B;
    constexpr auto packed = Prospero::BufferFormat::k11_11_10Float;
    const auto descriptor = [](uint64_t address, u32 width, u32 height,
                                Prospero::BufferFormat format, Prospero::TileMode tile) {
      ShaderTextureResource result{};
      result.fields[0] = static_cast<u32>(address >> 8);
      result.fields[1] = static_cast<u32>(address >> 40) |
          (static_cast<u32>(format) << 20) | (((width - 1) & 3) << 30);
      result.fields[2] = ((width - 1) >> 2) | ((height - 1) << 14);
      result.fields[3] = DstSel(4, 5, 6, 7) | (static_cast<u32>(tile) << 20) |
          (static_cast<u32>(Prospero::ImageType::kColor2D) << 28);
      result.fields[5] = 0x00700000;
      return result;
    };
    // Positive finite fixtures, comfortably within half precision's normal range.
    const auto half = [](float value) {
      const auto bits = std::bit_cast<u32>(value);
      const auto exponent = static_cast<int>((bits >> 23u) & 255u) - 127 + 15;
      return static_cast<uint16_t>((static_cast<u32>(exponent) << 10u) |
                                    ((bits >> 13u) & 1023u));
    };
    const auto unpack = [](u32 word, u32 channel) {
      const auto bits = channel == 2 ? 5u : 6u;
      const auto shifted = word >> (channel == 0 ? 0u : channel == 1 ? 11u : 22u);
      const auto exponent = (shifted >> bits) & 31u;
      const auto mantissa = shifted & ((1u << bits) - 1u);
      return exponent == 31u ? std::numeric_limits<float>::quiet_NaN() :
          std::ldexp(static_cast<float>(mantissa + (exponent ? 1u << bits : 0u)),
                     static_cast<int>(exponent ? exponent : 1u) - 15 - static_cast<int>(bits));
    };
    EnsureRuntimeContext();
    Require(name, "native host contract", m_runtime_context.gt7_dlss_device_enabled &&
        Gt7DlssHost::ApiPssr() != nullptr, "native device/ABI was not enabled");
    int64_t physical = -1;
    Require(name, "allocate", LibKernel::Memory::KernelAllocateDirectMemory(
        0, LibKernel::Memory::KernelGetDirectMemorySize(), allocation_size, 0x10000, 0, &physical) == 0,
        "native fixture allocation failed");
    void *mapped = reinterpret_cast<void *>(base);
    Require(name, "map", LibKernel::Memory::KernelMapDirectMemory(
        &mapped, allocation_size, 0x3, 0x10, physical, 0x10000) == 0 &&
        mapped == reinterpret_cast<void *>(base), "native fixture mapping failed");
    std::memset(mapped, 0, allocation_size);
    RenderContext context(m_runtime_context);
    context.InitializeGpu(nullptr);
    LibKernel::Memory::InstallGpuResources(&context);
    context.GetGpu().SendCommandSync([&] {
      CommandProcessor processor(context, 0);
      processor.BufferInit();
      context.MapMemory(base, allocation_size);
      auto &executor = context.GetRenderExecutor();
      auto &cache = context.GetTextureCache();
      auto &scheduler = context.GetCommandScheduler();
      auto input = descriptor(base, input_width, input_height, packed, linear);
      auto depth = descriptor(base + 0x200000, input_width, input_height,
                                     Prospero::BufferFormat::k32Float, linear);
      auto motion = descriptor(base + 0x400000, input_width, input_height,
                                      Prospero::BufferFormat::k32_32Float, linear);
      const auto exposure = descriptor(base + 0x600000, 1, 1,
                                        Prospero::BufferFormat::k32Float, linear);
      const auto output = descriptor(base + 0x800000, output_width, output_height, packed, tiled);
      const auto write_linear = [&](const ShaderTextureResource &texture, auto fill) {
        const auto format = texture.Format();
        const auto width = texture.Width5() + 1u, height = texture.Height5() + 1u;
        const auto bytes_per_texel = Prospero::NumBytesPerElement(format);
        TileSizeAlign total{};
        TileGetTextureTotalSize(format, width, height, 1, 1, linear, false, total);
        TileSizeOffset offsets[16]{};
        TilePaddedSize padded[16]{};
        TileGetTextureSize(format, width, height, 1, linear, nullptr, offsets, padded);
        Require(name, "fixture size", total.size != 0 && total.size <= 0x200000,
                "linear fixture exceeds its reserved slot");
        std::vector<uint8_t> bytes(total.size);
        for (u32 y = 0; y < height; ++y) for (u32 x = 0; x < width; ++x)
          fill(bytes.data() + (uint64_t{y} * padded[0].width + x) * bytes_per_texel, x, y);
        Require(name, "invalidate fixture", context.InvalidateMemory(texture.Base40(), total.size),
                "fixture is outside mapped guest memory");
        LibKernel::Memory::WriteBacking(texture.Base40(), bytes.data(), total.size);
      };
      write_linear(depth, [](uint8_t *target, u32, u32) {
        constexpr float value = .5f;
        std::memcpy(target, &value, sizeof(value));
      });
      write_linear(motion, [](uint8_t *target, u32, u32) {
        constexpr std::array<float, 2> value {0, 0};
        std::memcpy(target, value.data(), sizeof(value));
      });
      Pssr::Request request{};
      std::copy_n(input.fields, 8, request.input);
      std::copy_n(output.fields, 8, request.output);
      std::copy_n(depth.fields, 8, request.depth);
      std::copy_n(motion.fields, 8, request.motion);
      std::copy_n(exposure.fields, 8, request.exposure);
      request.context = 0x505353524e415449ull;
      request.x = request.y = 8;
      request.width = 320;
      request.height = 180;
      Dlss::Feature *history = nullptr;
      Dlss::Creation creation{};
      uint64_t recorded = 0;
      const auto dispatch = [&] {
        std::vector<u32> packet(Pssr::RequestDwords + 1);
        packet[0] = KYTY_PM4(packet.size(), Pm4::IT_NOP, Pm4::R_PSSR);
        std::memcpy(packet.data() + 1, &request, sizeof(request));
        Pm4Execution execution;
        Require(name, "native packet", processor.Process(execution, packet) == Pm4ProcessResult::Complete,
                "native PM4 packet did not complete");
        ++recorded;
        Require(name, "real NGX execution", RenderExecutorTestAccess::PssrCounts(executor) ==
                    std::array<uint64_t, 3>{recorded, recorded, 0},
                "request fell back; this test requires actual native DLSS evaluation");
        if (!history) {
          history = RenderExecutorTestAccess::PssrFeature(executor);
          creation = RenderExecutorTestAccess::PssrCreation(executor);
        }
        Require(name, "same-context history", history != nullptr &&
                    RenderExecutorTestAccess::PssrFeature(executor) == history &&
                    RenderExecutorTestAccess::PssrCreation(executor) == creation,
                "compatible dynamic inputs unexpectedly replaced the feature history");
      };
      const auto read_and_check = [&](u32 frame, float exposure_value) {
        ShaderRecompiler::IR::ImageResource resource{};
        resource.resource_class = ShaderRecompiler::IR::ImageResourceClass::Sampled;
        resource.numeric_class = Prospero::TextureNumericClass::Float;
        resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
        resource.read = true;
        ShaderRecompiler::IR::DescriptorValue descriptor_value{};
        descriptor_value.dword_count = 8;
        std::copy_n(output.fields, 8, descriptor_value.dwords.begin());
        const auto binding = RenderExecutorTestAccess::ResolveTexture(executor, resource, descriptor_value);
        const auto pixels = ReadCachedTexel(name, context, binding.image_id, {},
            {output_width, output_height, 1}, binding.desc.view_info.base_layer,
            binding.desc.view_info.base_level);
        Require(name, "output size", pixels.size() == size_t{output_width} * output_height,
                "native output readback has an unexpected size");
        double error = 0;
        float max_error = 0;
        uint64_t measured = 0;
        for (u32 y = 0; y < output_height; ++y) for (u32 x = 0; x < output_width; ++x)
          for (u32 channel = 0; channel < 3; ++channel) {
            const float value = unpack(pixels[size_t{y} * output_width + x], channel);
            Require(name, "finite output", std::isfinite(value), "native renderer produced NaN/Inf");
            if (x < 16 || y < 16 || x + 16 >= output_width || y + 16 >= output_height) continue;
            const float expected = channel == 0 ? .2f + .4f * (float(x) + .5f) / output_width :
                                   channel == 1 ? .25f + .3f * (float(y) + .5f) / output_height : .3f;
            const float delta = std::abs(value / request.pre_exposure - expected);
            error += delta;
            max_error = std::max(max_error, delta);
            ++measured;
          }
        const double mean_error = error / measured;
        std::printf("[native]  frame=%u recorded=%llu input=%ux%u pre=%.9g exposure=%.9g MAE=%.9g max=%.9g\n",
                    frame, static_cast<unsigned long long>(recorded), request.width, request.height,
                    request.pre_exposure, exposure_value, mean_error, max_error);
        Require(name, "HDR region", mean_error < .05 && max_error < .2f,
                "active rectangle/input refresh/exposure produced incorrect HDR brightness");
      };
      constexpr std::array<float, 4> exposure_values {.25f, 1.f, 4.f, 16.f};
      float exposure_value = 1;
      for (u32 frame = 0; frame < 16; ++frame) {
        if (frame) {
          request.width = frame % 2 ? creation.max_width : creation.min_width;
          request.height = frame % 2 ? creation.max_height : creation.min_height;
        }
        Require(name, "active rectangle", request.width + 8 <= input_width &&
                    request.height + 8 <= input_height,
                "runtime dynamic range exceeds fixture allocation");
        request.pre_exposure = exposure_values[frame % 4];
        request.motion_scale[0] = static_cast<float>(request.width);
        request.motion_scale[1] = static_cast<float>(request.height);
        request.flags = Pssr::kReverseDepth | (frame == 0 ? Pssr::kReset : 0u);
        exposure_value = exposure_values[(frame / 4) % 4];
        write_linear(input, [&](uint8_t *target, u32 x, u32 y) {
          const bool inside = x >= 8 && y >= 8 && x < 8 + request.width && y < 8 + request.height;
          const float u = (float(x) - 8.f + .5f) / request.width;
          const float v = (float(y) - 8.f + .5f) / request.height;
          const auto r = half(request.pre_exposure * (inside ? .2f + .4f * u : 32.f));
          const auto g = half(request.pre_exposure * (inside ? .25f + .3f * v : 32.f));
          const auto b = half(request.pre_exposure * (inside ? .3f : 32.f));
          const u32 value = (u32{r} >> 4u) | ((u32{g} >> 4u) << 11u) | ((u32{b} >> 5u) << 22u);
          std::memcpy(target, &value, sizeof(value));
        });
        write_linear(exposure, [&](uint8_t *target, u32, u32) {
          std::memcpy(target, &exposure_value, sizeof(exposure_value));
        });
        dispatch();
        read_and_check(frame, exposure_value);
      }
      // Reuse one native history several times in a single in-flight command
      // buffer; no CPU readback or Finish between these three evaluations.
      for (u32 i = 0; i < 3; ++i) dispatch();
      read_and_check(16, exposure_value);
      Require(name, "initial count", recorded == 19, "missing original integration evaluations");

      // Change the actual color/depth/motion descriptors at the same guest bases,
      // rather than just their active rectangle. Hold each new backing for eight
      // evaluations: the captured game's bright inputs appeared four or five
      // requests after resizing. Two small->large->small cycles retain the same
      // native history without injecting extra reset flags. CPU producers and
      // per-frame readback are deliberate: this phase tests cache recreation and
      // retained NGX history, not overlapping game command queues.
      constexpr u32 resize_phases = 5;
      constexpr u32 frames_per_resize = 8;
      Require(name, "resize dimensions", creation.min_width <= 320 &&
                  creation.min_height <= 180 && creation.max_width >= 640 &&
                  creation.max_height >= 360,
              "runtime dynamic range does not support the bounded 320x180/640x360 fixture");
      request.x = request.y = 0;
      request.pre_exposure = exposure_value = 1;
      request.flags = Pssr::kReverseDepth;
      write_linear(exposure, [&](uint8_t *target, u32, u32) {
        std::memcpy(target, &exposure_value, sizeof(exposure_value));
      });
      std::array<ImageId, 3> previous_images{};
      std::vector<ImageId> previous_aliases;
      size_t retired_aliases = 0;
      for (u32 phase = 0; phase < resize_phases; ++phase) {
        request.width = phase % 2 ? 640 : 320;
        request.height = phase % 2 ? 360 : 180;
        request.motion_scale[0] = static_cast<float>(request.width);
        request.motion_scale[1] = static_cast<float>(request.height);
        input = descriptor(base, request.width, request.height, packed, linear);
        depth = descriptor(base + 0x200000, request.width, request.height,
                           Prospero::BufferFormat::k32Float, linear);
        motion = descriptor(base + 0x400000, request.width, request.height,
                            Prospero::BufferFormat::k32_32Float, linear);
        std::copy_n(input.fields, 8, request.input);
        std::copy_n(depth.fields, 8, request.depth);
        std::copy_n(motion.fields, 8, request.motion);
        write_linear(depth, [](uint8_t *target, u32, u32) {
          constexpr float value = .5f;
          std::memcpy(target, &value, sizeof(value));
        });
        write_linear(motion, [](uint8_t *target, u32, u32) {
          constexpr std::array<float, 2> value {0, 0};
          std::memcpy(target, value.data(), sizeof(value));
        });
        std::printf("[native]  backing phase=%u dimensions=%ux%u origin=0,0 held=%u reset=0\n",
                    phase, request.width, request.height, frames_per_resize);
        for (u32 held = 0; held < frames_per_resize; ++held) {
          write_linear(input, [&](uint8_t *target, u32 x, u32 y) {
            const auto r = half(.2f + .4f * (float(x) + .5f) / request.width);
            const auto g = half(.25f + .3f * (float(y) + .5f) / request.height);
            const auto b = half(.3f);
            const u32 value = (u32{r} >> 4u) | ((u32{g} >> 4u) << 11u) |
                              ((u32{b} >> 5u) << 22u);
            std::memcpy(target, &value, sizeof(value));
          });
          dispatch();
          if (held == 0) {
            // Verify that this phase really exercises different physical image
            // extents, not a cached large image with a smaller active rectangle.
            const std::array<const ShaderTextureResource *, 3> textures {&input, &depth, &motion};
            for (size_t i = 0; i < textures.size(); ++i) {
              ShaderRecompiler::IR::ImageResource resource{};
              resource.resource_class = ShaderRecompiler::IR::ImageResourceClass::Sampled;
              resource.numeric_class = Prospero::TextureNumericClass::Float;
              resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
              resource.read = true;
              ShaderRecompiler::IR::DescriptorValue value{};
              value.dword_count = 8;
              std::copy_n(textures[i]->fields, 8, value.dwords.begin());
              const auto binding = RenderExecutorTestAccess::ResolveTexture(executor, resource, value);
              const auto &image = cache.GetImage(binding.image_id);
              Require(name, "recreated backing extent", image.backing.extent.width == request.width &&
                          image.backing.extent.height == request.height &&
                          binding.desc.view_info.base_level == 0 && binding.desc.view_info.base_layer == 0,
                      "resized descriptor reused a differently sized image or subresource");
              Require(name, "recreated backing identity", phase == 0 ||
                          binding.image_id != previous_images[i],
                      "resized descriptor did not replace its image identity");
              previous_images[i] = binding.image_id;
            }
          }
          read_and_check(17 + phase * frames_per_resize + held, exposure_value);
          const auto aliases = TextureCacheTestAccess::FindImages(cache, base, 1, false);
          for (const auto id : previous_aliases)
            retired_aliases += !TextureCacheTestAccess::Contains(cache, id);
          previous_aliases = aliases;
          std::printf("[native]  backing age phase=%u held=%u presented=%llu tick=%llu color_owners=%zu retired=%zu\n",
                      phase, held,
                      static_cast<unsigned long long>(m_runtime_context.presented_frames.load()),
                      static_cast<unsigned long long>(scheduler.CurrentTick()),
                      aliases.size(), retired_aliases);
          // ResolveOverlap's age gate needs both >4 presented frames and >32
          // submissions. The game submits many times per frame. Exercise that
          // gate with real empty submissions, without modifying cached ages or
          // forcing collection. SameBacking may legitimately retain an alias;
          // the trace above records whether a later overlap actually retired it.
          for (u32 submission = 0; submission < 8; ++submission) scheduler.Flush();
          BufferCacheTestAccess::PresentFrames(context.GetBufferCache(), 1);
        }
      }
      Require(name, "aged alias coverage", retired_aliases != 0,
              "resizing did not retire an old overlapping color image; age path was not exercised");
      Require(name, "final count", recorded == 19 + resize_phases * frames_per_resize,
              "missing bounded backing-recreation evaluations");
      context.UnmapMemory(base, allocation_size);
      scheduler.Finish();
    });
    LibKernel::Memory::InstallGpuResources(nullptr);
    context.ShutdownGpu();
    Require(name, "release", LibKernel::Memory::KernelCheckedReleaseDirectMemory(
        physical, allocation_size) == 0, "native fixture allocation release failed");
    std::printf("[gpu]     %-32s ok (59 native evaluations, no fallback, HDR/dynamic/exposure/history/backing recreation)\n", name);
  }
