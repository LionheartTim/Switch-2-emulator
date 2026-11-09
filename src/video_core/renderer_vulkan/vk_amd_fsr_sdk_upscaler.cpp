// Native Vulkan bridge to AMD FidelityFX SDK 1.1.4. This code is a work-in-progress
// integration for game-temporal input; it is not the existing shader-based Lichii upscaler.
#include "video_core/renderer_vulkan/vk_amd_fsr_sdk_upscaler.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include "common/settings.h"
#include <stdexcept>
#include <string>
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {
void Check(FfxErrorCode code, const char* where) {
    if (code != FFX_OK) {
        throw std::runtime_error(std::string{"AMD FidelityFX SDK "} + where +
                                 " failed (" + std::to_string(static_cast<int>(code)) + ")");
    }
}

FfxResource Resource(VkImage image, VkFormat format, VkExtent2D extent,
                     FfxResourceStates state, bool writable) {
    FfxResourceDescription desc{};
    desc.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    desc.format = ffxGetSurfaceFormatVK(format);
    desc.width = extent.width;
    desc.height = extent.height;
    desc.depth = 1;
    desc.mipCount = 1;
    desc.flags = FFX_RESOURCE_FLAGS_NONE;
    desc.usage = writable ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY;
    return ffxGetResourceVK(reinterpret_cast<void*>(image), desc, nullptr, state);
}
}  // namespace

// LICHII_AMD_NATIVE_FRAME_GENERATION_EXPERIMENT_V1
// Manually-presented FidelityFX Frame Interpolation: Linux never imports the Win32 swapchain.
// Operates only on real DLSS-compatible game depth/motion input. No fake motion data is fabricated.

namespace {
std::mutex g_native_fg_registry_mutex;
std::weak_ptr<AmdFsrSdkUpscaler> g_native_fg_registry;

bool NativeFgOptIn() {
    // A dedicated GUI choice is the explicit opt-in. Do not silently replace
    // the separate built-in Optical Flow generator.
    return Settings::values.official_amd_fsr31.GetValue() &&
           Settings::values.frame_gen.GetValue() &&
           Settings::values.frame_gen_type.GetValue() == Settings::FrameGenType::AmdFfx3;
}

VkFormat VkFormatForFfx(FfxSurfaceFormat format) {
    switch (format) {
    case FFX_SURFACE_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case FFX_SURFACE_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case FFX_SURFACE_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
    case FFX_SURFACE_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case FFX_SURFACE_FORMAT_R16G16_UINT: return VK_FORMAT_R16G16_UINT;
    case FFX_SURFACE_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
    case FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case FFX_SURFACE_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
    case FFX_SURFACE_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
    case FFX_SURFACE_FORMAT_R8_UINT: return VK_FORMAT_R8_UINT;
    case FFX_SURFACE_FORMAT_R8G8_UINT: return VK_FORMAT_R8G8_UINT;
    case FFX_SURFACE_FORMAT_R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
    case FFX_SURFACE_FORMAT_R16_UINT: return VK_FORMAT_R16_UINT;
    case FFX_SURFACE_FORMAT_R16_UNORM: return VK_FORMAT_R16_UNORM;
    case FFX_SURFACE_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case FFX_SURFACE_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case FFX_SURFACE_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case FFX_SURFACE_FORMAT_R10G10B10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case FFX_SURFACE_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    case FFX_SURFACE_FORMAT_R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}

FfxResource MakeFfxResource(VkImage image, VkFormat fmt, VkExtent2D extent,
                            FfxResourceStates state, bool writable) {
    FfxResourceDescription desc{};
    desc.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    desc.format = ffxGetSurfaceFormatVK(fmt);
    desc.width = extent.width;
    desc.height = extent.height;
    desc.depth = 1;
    desc.mipCount = 1;
    desc.flags = FFX_RESOURCE_FLAGS_NONE;
    desc.usage = writable ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY;
    return ffxGetResourceVK(reinterpret_cast<void*>(image), desc, nullptr, state);
}
} // namespace

struct AmdFsrSdkUpscaler::NativeFrameGeneration {
    struct OwnedImage {
        vk::Image image;
        VkFormat format{VK_FORMAT_UNDEFINED};
        VkExtent2D extent{};

        void Create(MemoryAllocator& allocator, const FfxCreateResourceDescription& desc) {
            format = VkFormatForFfx(desc.resourceDescription.format);
            extent = {desc.resourceDescription.width, desc.resourceDescription.height};
            if (format == VK_FORMAT_UNDEFINED || !extent.width || !extent.height ||
                desc.resourceDescription.type != FFX_RESOURCE_TYPE_TEXTURE2D) {
                throw std::runtime_error("AMD FG shared resource format or size unsupported on Vulkan");
            }
            image = allocator.CreateImage(VkImageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = format,
                .extent = {extent.width, extent.height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            });
        }

        [[nodiscard]] FfxResource Resource(FfxResourceStates state) const {
            return MakeFfxResource(*image, format, extent, state, true);
        }
    };

    NativeFrameGeneration(Scheduler& scheduler, MemoryAllocator& allocator, FfxInterface& backend,
                          VkExtent2D render, VkExtent2D display, VkFormat display_format)
        : scheduler(scheduler), render(render), display(display) {
        try {
        const FfxSurfaceFormat ffx_format = ffxGetSurfaceFormatVK(display_format);
        if (ffx_format == FFX_SURFACE_FORMAT_UNKNOWN) {
            throw std::runtime_error("AMD FG unsupported presentation format");
        }
        interpolation = std::make_unique<FfxFrameInterpolationContext>();
        optical_flow = std::make_unique<FfxOpticalflowContext>();

        FfxFrameInterpolationContextDescription interp_desc{};
        interp_desc.flags = FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED;
        interp_desc.maxRenderSize = {render.width, render.height};
        interp_desc.displaySize = {display.width, display.height};
        interp_desc.backBufferFormat = ffx_format;
        interp_desc.previousInterpolationSourceFormat = ffx_format;
        interp_desc.backendInterface = backend;
        Check(ffxFrameInterpolationContextCreate(interpolation.get(), &interp_desc),
              "Frame Interpolation context");
        interpolation_live = true;

        FfxOpticalflowContextDescription flow_desc{};
        flow_desc.flags = 0;
        flow_desc.resolution = {display.width, display.height};
        flow_desc.backendInterface = backend;
        Check(ffxOpticalflowContextCreate(optical_flow.get(), &flow_desc),
              "Optical Flow context");
        optical_flow_live = true;

        FfxFrameInterpolationSharedResourceDescriptions interp_shared{};
        Check(ffxFrameInterpolationGetSharedResourceDescriptions(interpolation.get(), &interp_shared),
              "Frame Interpolation shared resources");
        dilated_depth.Create(allocator, interp_shared.dilatedDepth);
        dilated_motion.Create(allocator, interp_shared.dilatedMotionVectors);
        previous_depth.Create(allocator, interp_shared.reconstructedPrevNearestDepth);

        FfxOpticalflowSharedResourceDescriptions flow_shared{};
        Check(ffxOpticalflowGetSharedResourceDescriptions(optical_flow.get(), &flow_shared),
              "Optical Flow shared resources");
        flow_vectors.Create(allocator, flow_shared.opticalFlowVector);
        flow_scd.Create(allocator, flow_shared.opticalFlowSCD);
        const std::array images{*dilated_depth.image, *dilated_motion.image,
                                *previous_depth.image, *flow_vectors.image, *flow_scd.image};
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([images](vk::CommandBuffer cmdbuf) {
            std::array<VkImageMemoryBarrier, images.size()> barriers{};
            for (size_t i = 0; i < images.size(); ++i) {
                barriers[i] = VkImageMemoryBarrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = 0,
                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = images[i],
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                };
            }
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, barriers);
        });
        } catch (...) {
            if (optical_flow_live) {
                ffxOpticalflowContextDestroy(optical_flow.get());
                optical_flow_live = false;
            }
            if (interpolation_live) {
                ffxFrameInterpolationContextDestroy(interpolation.get());
                interpolation_live = false;
            }
            throw;
        }
    }

    ~NativeFrameGeneration() {
        if (optical_flow_live) ffxOpticalflowContextDestroy(optical_flow.get());
        if (interpolation_live) ffxFrameInterpolationContextDestroy(interpolation.get());
    }

    void Prepare(vk::CommandBuffer cmdbuf, const Input& in, uint64_t id) {
        FfxFrameInterpolationPrepareDescription prep{};
        prep.commandList = ffxGetCommandListVK(*cmdbuf);
        prep.renderSize = {in.render.width, in.render.height};
        prep.jitterOffset = {in.jitter[0], in.jitter[1]};
        prep.motionVectorScale = {in.motion_scale[0], in.motion_scale[1]};
        prep.frameTimeDelta = in.frame_ms;
        prep.cameraNear = 0.1f;
        prep.cameraFar = 1000.0f;
        prep.cameraFovAngleVertical = 1.04719755f;
        prep.viewSpaceToMetersFactor = 1.0f;
        prep.depth = MakeFfxResource(in.depth, VK_FORMAT_D32_SFLOAT, in.render,
                                     FFX_RESOURCE_STATE_COMPUTE_READ, false);
        prep.motionVectors = MakeFfxResource(in.motion, in.motion_format, in.render,
                                             FFX_RESOURCE_STATE_COMPUTE_READ, false);
        prep.frameID = id;
        prep.dilatedDepth = dilated_depth.Resource(FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        prep.dilatedMotionVectors = dilated_motion.Resource(FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        prep.reconstructedPrevDepth = previous_depth.Resource(FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        // The DLSS contract has no camera transform. The identity basis is provisional;
        // the real motion vectors and depth are supplied above (never synthetic).
        prep.cameraUp[1] = 1.0f;
        prep.cameraRight[0] = 1.0f;
        prep.cameraForward[2] = 1.0f;
        const FfxErrorCode code = ffxFrameInterpolationPrepare(interpolation.get(), &prep);
        if (code != FFX_OK) {
            failed.store(true);
            LOG_ERROR(Render_Vulkan, "AMD native FG prepare failed: {}", static_cast<int>(code));
        }
    }

    void Process(vk::CommandBuffer cmdbuf, VkImage current, VkFormat format,
                 VkExtent2D extent, bool reset) {
        const VkImageMemoryBarrier color_to_compute{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = current,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, color_to_compute);
        FfxOpticalflowDispatchDescription dispatch{};
        dispatch.commandList = ffxGetCommandListVK(*cmdbuf);
        dispatch.color = MakeFfxResource(current, format, extent,
                                          FFX_RESOURCE_STATE_COMPUTE_READ, false);
        dispatch.opticalFlowVector = flow_vectors.Resource(FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch.opticalFlowSCD = flow_scd.Resource(FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch.reset = reset;
        dispatch.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        dispatch.minMaxLuminance = {0.0f, 1.0f};
        const FfxErrorCode code = ffxOpticalflowContextDispatch(optical_flow.get(), &dispatch);
        if (code != FFX_OK) {
            failed.store(true);
            LOG_ERROR(Render_Vulkan, "AMD native FG optical flow failed: {}", static_cast<int>(code));
        }
    }

    void Generate(vk::CommandBuffer cmdbuf, VkImage presented, VkImage destination,
                  VkFormat format, uint64_t serial, bool reset, float frame_ms) {
        FfxFrameInterpolationDispatchDescription dispatch{};
        dispatch.commandList = ffxGetCommandListVK(*cmdbuf);
        dispatch.displaySize = {display.width, display.height};
        dispatch.renderSize = {render.width, render.height};
        dispatch.currentBackBuffer = MakeFfxResource(presented, format, display,
                                                    FFX_RESOURCE_STATE_COMPUTE_READ, false);
        dispatch.output = MakeFfxResource(destination, format, display,
                                           FFX_RESOURCE_STATE_UNORDERED_ACCESS, true);
        dispatch.opticalFlowVector = flow_vectors.Resource(FFX_RESOURCE_STATE_COMPUTE_READ);
        dispatch.opticalFlowSceneChangeDetection = flow_scd.Resource(FFX_RESOURCE_STATE_COMPUTE_READ);
        dispatch.opticalFlowBufferSize = {flow_vectors.extent.width, flow_vectors.extent.height};
        dispatch.opticalFlowBlockSize = 8;
        dispatch.opticalFlowScale = {1.0f / display.width, 1.0f / display.height};
        dispatch.cameraNear = 0.1f;
        dispatch.cameraFar = 1000.0f;
        dispatch.cameraFovAngleVertical = 1.04719755f;
        dispatch.viewSpaceToMetersFactor = 1.0f;
        dispatch.frameTimeDelta = frame_ms;
        dispatch.reset = reset;
        dispatch.frameID = serial;
        dispatch.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        dispatch.minMaxLuminance[0] = 0.0f;
        dispatch.minMaxLuminance[1] = 1.0f;
        dispatch.interpolationRect.width = display.width;
        dispatch.interpolationRect.height = display.height;
        dispatch.dilatedDepth = dilated_depth.Resource(FFX_RESOURCE_STATE_COMPUTE_READ);
        dispatch.dilatedMotionVectors = dilated_motion.Resource(FFX_RESOURCE_STATE_COMPUTE_READ);
        dispatch.reconstructedPrevDepth = previous_depth.Resource(FFX_RESOURCE_STATE_COMPUTE_READ);
        const auto code = ffxFrameInterpolationDispatch(interpolation.get(), &dispatch);
        if (code != FFX_OK) {
            failed.store(true);
            LOG_ERROR(Render_Vulkan, "AMD native FG dispatch failed: {}", static_cast<int>(code));
        } else if (!first_dispatch_logged.exchange(true)) {
            LOG_INFO(Render_Vulkan,
                     "AMD FidelityFX SDK 1.1.4 native Frame Interpolation dispatch executed (frame {})",
                     serial);
        }
    }

    Scheduler& scheduler;
    VkExtent2D render{};
    VkExtent2D display{};
    std::unique_ptr<FfxFrameInterpolationContext> interpolation;
    std::unique_ptr<FfxOpticalflowContext> optical_flow;
    bool interpolation_live{};
    bool optical_flow_live{};
    OwnedImage dilated_depth, dilated_motion, previous_depth, flow_vectors, flow_scd;
    std::atomic<bool> failed{false};
    std::atomic<bool> first_dispatch_logged{false};
};

bool AmdFsrSdkUpscaler::NativeFgRequested() {
    return NativeFgOptIn();
}

void AmdFsrSdkUpscaler::PublishNative(const std::shared_ptr<AmdFsrSdkUpscaler>& instance) {
    if (!instance || !instance->native_fg) return;
    std::scoped_lock lock(g_native_fg_registry_mutex);
    g_native_fg_registry = instance;
}

std::shared_ptr<AmdFsrSdkUpscaler> AmdFsrSdkUpscaler::LatestNative() {
    std::scoped_lock lock(g_native_fg_registry_mutex);
    return g_native_fg_registry.lock();
}

uint64_t AmdFsrSdkUpscaler::NativeFrameSerial() const {
    return native_serial.load(std::memory_order_relaxed);
}

bool AmdFsrSdkUpscaler::NativeReady(VkExtent2D extent, VkFormat format) const {
    return NativeFgOptIn() && native_fg && !native_fg->failed.load(std::memory_order_relaxed) &&
           native_serial.load(std::memory_order_relaxed) > 0 &&
           native_output_format == format && upscale_extent.width == extent.width &&
           upscale_extent.height == extent.height;
}

bool AmdFsrSdkUpscaler::QueueNativeOpticalFlow(VkImage presented, VkFormat format,
                                               VkExtent2D extent, uint64_t serial) {
    if (!NativeReady(extent, format) || NativeFrameSerial() != serial ||
        serial == native_present_serial) return false;
    native_present_serial = serial;
    auto* const fg = native_fg.get();
    const bool reset = native_reset || serial != native_last_present_serial + 1;
    native_last_present_serial = serial;
    const float frame_ms = native_frame_ms;
    native_presented_image = presented;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([fg, presented, format, extent, serial, reset](vk::CommandBuffer cmdbuf) {
        if (!fg->failed.load(std::memory_order_relaxed)) {
            fg->Process(cmdbuf, presented, format, extent, reset);
        }
    });
    native_present_reset = reset;
    native_present_ms = frame_ms;
    return true;
}

bool AmdFsrSdkUpscaler::QueueNativeGeneration(VkImage dest, VkFormat format,
                                               VkExtent2D extent, uint64_t serial) {
    if (!NativeReady(extent, format) || native_present_serial != serial ||
        !native_presented_image) return false;
    auto* const fg = native_fg.get();
    const VkImage presented = native_presented_image;
    const bool reset = native_present_reset;
    const float frame_ms = native_present_ms;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([fg, presented, dest, format, serial, reset, frame_ms](vk::CommandBuffer cmdbuf) {
        if (!fg->failed.load(std::memory_order_relaxed)) {
            fg->Generate(cmdbuf, presented, dest, format, serial, reset, frame_ms);
        }
    });
    return true;
}

AmdFsrSdkUpscaler::AmdFsrSdkUpscaler(const Device& device_, Scheduler& scheduler_,
                                     MemoryAllocator& allocator_)
    : device{device_}, scheduler{scheduler_}, allocator{allocator_} {
    point_sampler = device.GetLogical().CreateSampler(VkSamplerCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 1.0f,
    });
}

AmdFsrSdkUpscaler::~AmdFsrSdkUpscaler() {
    TearDown();
}

void AmdFsrSdkUpscaler::TearDown() {
    if (context_live || native_fg) scheduler.Finish();
    native_fg.reset();
    native_serial.store(0, std::memory_order_relaxed);
    native_present_serial = 0;
    native_last_present_serial = 0;
    native_presented_image = VK_NULL_HANDLE;
    if (context_live) {
        ffxFsr3UpscalerContextDestroy(&context);
        context_live = false;
    }
    output_view.reset();
    output_image.reset();
    scratch.clear();
}

void AmdFsrSdkUpscaler::Recreate(VkExtent2D render, VkExtent2D upscale) {
    TearDown();
    render_extent = render;
    upscale_extent = upscale;
    vk_device_context.vkDevice = *device.GetLogical();
    vk_device_context.vkPhysicalDevice = device.GetPhysical();
    vk_device_context.vkDeviceProcAddr = device.GetDispatchLoader().vkGetDeviceProcAddr;
    const FfxDevice sdk_device = ffxGetDeviceVK(&vk_device_context);
    const size_t context_count = NativeFgRequested()
        ? FFX_FSR3UPSCALER_CONTEXT_COUNT + FFX_FRAMEINTERPOLATION_CONTEXT_COUNT +
              FFX_OPTICALFLOW_CONTEXT_COUNT
        : FFX_FSR3UPSCALER_CONTEXT_COUNT;
    scratch.resize(ffxGetScratchMemorySizeVK(device.GetPhysical(), context_count));
    Check(ffxGetInterfaceVK(&interface, sdk_device, scratch.data(), scratch.size(), context_count),
          "Vulkan backend init");
    FfxFsr3UpscalerContextDescription context_desc{};
    context_desc.maxRenderSize = {render.width, render.height};
    context_desc.maxUpscaleSize = {upscale.width, upscale.height};
    context_desc.flags = FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED |
                         FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    context_desc.backendInterface = interface;
    Check(ffxFsr3UpscalerContextCreate(&context, &context_desc), "upscaler context creation");
    context_live = true;

    // Native FG requires a full game-temporal input and an explicitly enabled
    // presentation generator. Keep the original FSR 3.1 path working if this fails.
    native_output_format = VK_FORMAT_B8G8R8A8_UNORM;
    if (NativeFgRequested()) {
        try {
            native_fg = std::make_unique<NativeFrameGeneration>(
                scheduler, allocator, interface, render, upscale, native_output_format);
            LOG_INFO(Render_Vulkan, "AMD native Vulkan Frame Interpolation prepared (experimental)");
        } catch (const std::exception& e) {
            native_fg.reset();
            LOG_WARNING(Render_Vulkan, "AMD native Frame Interpolation unavailable: {}", e.what());
        }
    }

    const VkImageCreateInfo image_ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .extent = {upscale.width, upscale.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    output_image = allocator.CreateImage(image_ci);
    output_view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *output_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    });
    const VkImage image = *output_image;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image](vk::CommandBuffer cmd) {
        const VkImageMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, barrier);
    });
}

void AmdFsrSdkUpscaler::Upscale(const Input& in) {
    if (!context_live || in.render.width != render_extent.width ||
        in.render.height != render_extent.height || in.upscale.width != upscale_extent.width ||
        in.upscale.height != upscale_extent.height) {
        Recreate(in.render, in.upscale);
    }
    const VkImage result_image = *output_image;
    // The actual SDK call is recorded in the renderer's Vulkan command-buffer sequence.
    // Previous Lichii FSR shader reimplementation is NOT invoked in this path.
    scheduler.RequestOutsideRenderPassOperationContext();
    const uint64_t frame_serial = native_fg
        ? native_serial.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    native_reset = in.reset;
    native_frame_ms = in.frame_ms;
    scheduler.Record([this, in, result_image, frame_serial](vk::CommandBuffer cmd) {
        FfxFsr3UpscalerDispatchDescription dispatch{};
        dispatch.commandList = ffxGetCommandListVK(*cmd);
        dispatch.color = Resource(in.color, in.color_format, in.render,
                                  FFX_RESOURCE_STATE_COMPUTE_READ, false);
        dispatch.depth = Resource(in.depth, VK_FORMAT_D32_SFLOAT, in.render,
                                  FFX_RESOURCE_STATE_COMPUTE_READ, false);
        dispatch.motionVectors = Resource(in.motion, in.motion_format, in.render,
                                          FFX_RESOURCE_STATE_COMPUTE_READ, false);
        dispatch.output = Resource(result_image, VK_FORMAT_R16G16B16A16_SFLOAT, in.upscale,
                                   FFX_RESOURCE_STATE_UNORDERED_ACCESS, true);
        dispatch.jitterOffset = {in.jitter[0], in.jitter[1]};
        dispatch.motionVectorScale = {in.motion_scale[0], in.motion_scale[1]};
        dispatch.renderSize = {in.render.width, in.render.height};
        dispatch.upscaleSize = {in.upscale.width, in.upscale.height};
        dispatch.enableSharpening = in.sharpen;
        dispatch.sharpness = in.sharpness;
        dispatch.frameTimeDelta = in.frame_ms;
        dispatch.preExposure = 1.0f;
        dispatch.reset = in.reset;
        dispatch.cameraNear = 0.1f;
        dispatch.cameraFar = 1000.0f;
        dispatch.cameraFovAngleVertical = 1.04719755f;
        dispatch.viewSpaceToMetersFactor = 1.0f;
        const auto code = ffxFsr3UpscalerContextDispatch(&context, &dispatch);
        if (code != FFX_OK) {
            LOG_ERROR(Render_Vulkan, "Official AMD FSR 3.1 SDK dispatch failed: {}",
                      static_cast<int>(code));
            if (native_fg) native_fg->failed.store(true, std::memory_order_relaxed);
        } else if (frame_serial != 0 && native_fg &&
                   !native_fg->failed.load(std::memory_order_relaxed)) {
            native_fg->Prepare(cmd, in, frame_serial);
        }
    });
}
}  // namespace Vulkan
