// SPDX-FileCopyrightText: Copyright 2026 Lichii experimental integration
// SPDX-License-Identifier: GPL-3.0-or-later
// Native FidelityFX Optical Flow probe for Lichii's *entire presented image*.
// This first stage records the real AMD optical-flow pass, but deliberately keeps
// the original interpolator as the visible output until vector format/pacing is validated.

#include "video_core/renderer_vulkan/present/amd_universal_optical_flow.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>

#include "common/logging.h"
#include "video_core/host_shaders/amd_universal_flow_convert_comp_spv.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {
void EnsureFfx(FfxErrorCode code, const char* operation) {
    if (code != FFX_OK) {
        throw std::runtime_error(std::string("AMD universal Optical Flow: ") + operation +
                                 " error=" + std::to_string(static_cast<int>(code)));
    }
}

FfxResource ToFfxResource(VkImage image, VkFormat format, VkExtent2D extent,
                           FfxResourceStates state, bool writable) {
    FfxResourceDescription description{};
    description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    description.format = ffxGetSurfaceFormatVK(format);
    description.width = extent.width;
    description.height = extent.height;
    description.depth = 1;
    description.mipCount = 1;
    description.usage = writable ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY;
    return ffxGetResourceVK(reinterpret_cast<void*>(image), description, nullptr, state);
}

VkFormat ToVkFormat(FfxSurfaceFormat format) {
    switch (format) {
    case FFX_SURFACE_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
    case FFX_SURFACE_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

VkImageMemoryBarrier ImageBarrier(VkImage image, VkAccessFlags before,
                                  VkAccessFlags after, VkImageLayout old_layout) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = before;
    barrier.dstAccessMask = after;
    barrier.oldLayout = old_layout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return barrier;
}
} // namespace

struct UniversalAmdOpticalFlow::Impl {
    struct Output {
        vk::Image image;
        VkExtent2D extent{};
        VkFormat format{VK_FORMAT_UNDEFINED};
        vk::ImageView view; // LICHII_AMD_UNIVERSAL_FG_V2
        Output(const Device& device, MemoryAllocator& allocator,
               const FfxCreateResourceDescription& description, bool need_view) {
            const auto& resource = description.resourceDescription;
            format = ToVkFormat(resource.format);
            extent = {resource.width, resource.height};
            if (resource.type != FFX_RESOURCE_TYPE_TEXTURE2D || format == VK_FORMAT_UNDEFINED ||
                !extent.width || !extent.height) {
                throw std::runtime_error("unsupported AMD Optical Flow output format or extent");
            }
            VkImageCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ci.imageType = VK_IMAGE_TYPE_2D;
            ci.format = format;
            ci.extent = {extent.width, extent.height, 1};
            ci.mipLevels = 1;
            ci.arrayLayers = 1;
            ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            image = allocator.CreateImage(ci);
            if (need_view) view = CreateWrappedImageView(device, image, format);
        }
        [[nodiscard]] FfxResource Resource() const {
            return ToFfxResource(*image, format, extent, FFX_RESOURCE_STATE_UNORDERED_ACCESS, true);
        }
    };

    Scheduler& scheduler;
    VkExtent2D extent;
    VkFormat format;
    VkDeviceContext vk_context{};
    FfxInterface backend{};
    std::vector<uint8_t> scratch;
    std::unique_ptr<FfxOpticalflowContext> effect;
    std::unique_ptr<Output> vectors;
    std::unique_ptr<Output> scene_change;
    // LICHII_AMD_UNIVERSAL_FG_V2: shader consumes AMD's signed integer 8x8 flow,
    // and writes only to the existing half-resolution RG16F motion buffer.
    VkImage motion_target{VK_NULL_HANDLE};
    VkExtent2D motion_extent{};
    VkImageView motion_view{VK_NULL_HANDLE};
    bool convert_enabled{};
    vk::ShaderModule convert_shader;
    vk::Sampler convert_sampler;
    vk::DescriptorSetLayout convert_layout;
    vk::PipelineLayout convert_pipeline_layout;
    vk::Pipeline convert_pipeline;
    vk::DescriptorPool convert_descriptor_pool;
    vk::DescriptorSets convert_sets;
    std::atomic<bool> failed{false};
    bool effect_live{};
    uint64_t frames{};

    Impl(const Device& device, Scheduler& scheduler_, MemoryAllocator& allocator,
         VkExtent2D extent_, VkFormat format_, VkImage motion_target_,
         VkImageView motion_view_, VkExtent2D motion_extent_)
        : scheduler(scheduler_), extent(extent_), format(format_),
          motion_target(motion_target_), motion_extent(motion_extent_),
          motion_view(motion_view_) {
        if (!extent.width || !extent.height ||
            ffxGetSurfaceFormatVK(format) == FFX_SURFACE_FORMAT_UNKNOWN) {
            throw std::runtime_error("unsupported presentation format or zero-sized frame");
        }
        vk_context.vkDevice = *device.GetLogical();
        vk_context.vkPhysicalDevice = device.GetPhysical();
        vk_context.vkDeviceProcAddr = device.GetDispatchLoader().vkGetDeviceProcAddr;
        // LICHII_AMD_UNIVERSAL_OF_NULLPTR_FIX_V1: verify Vulkan callbacks
        // BEFORE the SDK tries to invoke any of them.
        if (!vk_context.vkDeviceProcAddr) {
            vk_context.vkDeviceProcAddr = vkGetDeviceProcAddr;
        }
        if (!vk_context.vkDevice || !vk_context.vkPhysicalDevice ||
            !vk_context.vkDeviceProcAddr) {
            throw std::runtime_error("AMD Optical Flow: missing Vulkan device or function loader");
        }
        const auto has_proc = [&](const char* name) {
            return vk_context.vkDeviceProcAddr(vk_context.vkDevice, name) != nullptr;
        };
        for (const char* name : {
                 "vkCreateDescriptorPool", "vkCreateBuffer", "vkGetBufferMemoryRequirements",
                 "vkAllocateMemory", "vkMapMemory", "vkBindBufferMemory", "vkDestroyBuffer"}) {
            if (!has_proc(name)) {
                throw std::runtime_error(std::string("AMD Optical Flow: missing Vulkan callback ") + name);
            }
        }
        LOG_INFO(Render_Vulkan,
                 "AMD-UNIVERSAL-OF Vulkan preflight: KHR memory2={} core memory2={}",
                 has_proc("vkGetBufferMemoryRequirements2KHR"),
                 has_proc("vkGetBufferMemoryRequirements2"));
        const FfxDevice sdk_device = ffxGetDeviceVK(&vk_context);
        constexpr size_t kContextCount = FFX_OPTICALFLOW_CONTEXT_COUNT;
        scratch.resize(ffxGetScratchMemorySizeVK(device.GetPhysical(), kContextCount));
        EnsureFfx(ffxGetInterfaceVK(&backend, sdk_device, scratch.data(), scratch.size(),
                                     kContextCount), "backend initialization");
        effect = std::make_unique<FfxOpticalflowContext>();
        FfxOpticalflowContextDescription description{};
        description.backendInterface = backend;
        description.resolution = {extent.width, extent.height};
        EnsureFfx(ffxOpticalflowContextCreate(effect.get(), &description), "context creation");
        effect_live = true;
        try {
            FfxOpticalflowSharedResourceDescriptions shared{};
            EnsureFfx(ffxOpticalflowGetSharedResourceDescriptions(effect.get(), &shared),
                      "shared resource descriptions");
            const bool want_converter = UniversalAmdOpticalFlow::UseForInterpolation();
            vectors = std::make_unique<Output>(device, allocator, shared.opticalFlowVector, want_converter);
            scene_change = std::make_unique<Output>(device, allocator, shared.opticalFlowSCD, want_converter);
            if (want_converter) {
                CreateConverter(device);
            }
            LOG_INFO(Render_Vulkan,
                     "AMD-UNIVERSAL-OF initialized; game-independent SDK 1.1.4 Optical Flow at {}x{}; vectors={}x{} R16G16_SINT",
                     extent.width, extent.height, vectors->extent.width, vectors->extent.height);
        } catch (...) {
            ffxOpticalflowContextDestroy(effect.get());
            effect_live = false;
            throw;
        }
    }
    // LICHII_AMD_UNIVERSAL_FG_V2: the conversion is opt-in and uses a separate
    // compute pipeline, leaving Lichii's original interpolation pipeline intact.
    void CreateConverter(const Device& device) {
        if (motion_target == VK_NULL_HANDLE || motion_view == VK_NULL_HANDLE ||
            motion_extent.width == 0 || motion_extent.height == 0 ||
            vectors->format != VK_FORMAT_R16G16_SINT ||
            scene_change->format != VK_FORMAT_R32_UINT) {
            throw std::runtime_error("universal FG converter requires signed flow, SCD and RG16F output");
        }
        convert_shader = BuildShader(device, AMD_UNIVERSAL_FLOW_CONVERT_COMP_SPV);
        // Signed-integer R16G16_SINT must use nearest sampling. Use an INTEGER
        // border colour (the existing generic helper uses a float border).
        convert_sampler = device.GetLogical().CreateSampler(VkSamplerCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .maxLod = 0.0f,
            .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        });
        const std::array bindings{
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        convert_layout = device.GetLogical().CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = static_cast<uint32_t>(bindings.size()),
            .pBindings = bindings.data(),
        });
        struct ConvertConstants { int32_t motion_w, motion_h, display_w, display_h; };
        const VkPushConstantRange range{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(ConvertConstants),
        };
        convert_pipeline_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = convert_layout.address(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        });
        convert_pipeline = device.GetLogical().CreateComputePipeline(VkComputePipelineCreateInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = *convert_shader,
                .pName = "main",
            },
            .layout = *convert_pipeline_layout,
        });
        const std::array sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
        };
        convert_descriptor_pool = device.GetLogical().CreateDescriptorPool(VkDescriptorPoolCreateInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1,
            .poolSizeCount = static_cast<uint32_t>(sizes.size()),
            .pPoolSizes = sizes.data(),
        });
        convert_sets = CreateWrappedDescriptorSets(convert_descriptor_pool, {*convert_layout});
        const VkDescriptorImageInfo flow_info{
            .sampler = *convert_sampler,
            .imageView = *vectors->view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo destination_info{
            .imageView = motion_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkDescriptorImageInfo scene_info{
            .imageView = *scene_change->view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const std::array writes{
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = convert_sets[0], .dstBinding = 0, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &flow_info,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = convert_sets[0], .dstBinding = 1, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &destination_info,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = convert_sets[0], .dstBinding = 2, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &scene_info,
            },
        };
        device.GetLogical().UpdateDescriptorSets(writes, {});
        convert_enabled = true;
        LOG_INFO(Render_Vulkan,
                 "AMD-UNIVERSAL-FG converter prepared: AMD vectors={}x{}, Lichii motion={}x{} (experimental)",
                 vectors->extent.width, vectors->extent.height, motion_extent.width, motion_extent.height);
    }

    ~Impl() {
        if (effect_live) {
            scheduler.Finish();
            ffxOpticalflowContextDestroy(effect.get());
        }
    }

    void Dispatch(VkImage input, uint64_t frame_index) {
        if (failed.load(std::memory_order_relaxed)) return;
        const VkImage optical_vectors = *vectors->image;
        const VkImage scd = *scene_change->image;
        // The source is Lichii's presentation-sized frame copy in GENERAL layout.
        // Its previous copy and built-in interpolation are recorded earlier on this queue.
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([this, input, optical_vectors, scd, frame_index](vk::CommandBuffer cmd) {
            if (failed.load(std::memory_order_relaxed)) return;
            const auto source_barrier = ImageBarrier(input,
                VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, source_barrier);
            if (frames == 0) {
                const std::array initial{
                    ImageBarrier(optical_vectors, 0, VK_ACCESS_SHADER_WRITE_BIT,
                                 VK_IMAGE_LAYOUT_UNDEFINED),
                    ImageBarrier(scd, 0, VK_ACCESS_SHADER_WRITE_BIT,
                                 VK_IMAGE_LAYOUT_UNDEFINED),
                };
                cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, initial);
            }
            FfxOpticalflowDispatchDescription description{};
            description.commandList = ffxGetCommandListVK(*cmd);
            description.color = ToFfxResource(input, format, extent, FFX_RESOURCE_STATE_COMPUTE_READ, false);
            description.opticalFlowVector = vectors->Resource();
            description.opticalFlowSCD = scene_change->Resource();
            description.reset = frames == 0;
            description.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
            description.minMaxLuminance = {0.0f, 1.0f};
            const FfxErrorCode status = ffxOpticalflowContextDispatch(effect.get(), &description);
            if (status != FFX_OK) {
                failed.store(true, std::memory_order_relaxed);
                LOG_WARNING(Render_Vulkan,
                            "AMD-UNIVERSAL-OF dispatch failed: code={}; built-in generator kept",
                            static_cast<int>(status));
                return;
            }
            ++frames;
            // LICHII_AMD_UNIVERSAL_FG_V2: same scheduler command buffer, after
            // AMD dispatch. On an SDK error above this block is skipped and the
            // built-in RG16F motion field remains intact for the next Generate.
            if (convert_enabled && frames >= 2) {
                const std::array read_barriers{
                    ImageBarrier(optical_vectors, VK_ACCESS_SHADER_WRITE_BIT,
                                 VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL),
                    ImageBarrier(scd, VK_ACCESS_SHADER_WRITE_BIT,
                                 VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL),
                };
                cmd.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    0, {}, {}, read_barriers);
                const auto motion_before = ImageBarrier(motion_target,
                    VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
                cmd.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    0, motion_before);
                struct ConvertConstants { int32_t motion_w, motion_h, display_w, display_h; };
                const ConvertConstants pc{
                    static_cast<int32_t>(motion_extent.width),
                    static_cast<int32_t>(motion_extent.height),
                    static_cast<int32_t>(extent.width),
                    static_cast<int32_t>(extent.height),
                };
                cmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *convert_pipeline);
                cmd.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                       *convert_pipeline_layout, 0, convert_sets[0], {});
                cmd.PushConstants(*convert_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, pc);
                cmd.Dispatch((motion_extent.width + 15) / 16,
                             (motion_extent.height + 15) / 16, 1);
                const auto motion_after = ImageBarrier(motion_target,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL);
                cmd.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    0, motion_after);
                if (frames == 1 || frames % 300 == 0) {
                    LOG_INFO(Render_Vulkan,
                             "AMD-UNIVERSAL-FG vector conversion recorded: count={} frame_index={} (visible generator uses AMD flow)",
                             frames, frame_index);
                }
            }
            if (frames == 1 || frames % 300 == 0) {
                LOG_INFO(Render_Vulkan,
                         "AMD-UNIVERSAL-OF SDK dispatch success: count={} frame_index={} vectors={}x{} mode={}",
                         frames, frame_index, vectors->extent.width, vectors->extent.height,
                         convert_enabled && frames >= 2 ? "VISIBLE-AMD-FLOW" : "preview/built-in");
            }
        });
    }
};

bool UniversalAmdOpticalFlow::UseForInterpolation() {
    // LICHII_AMD_UNIVERSAL_FG_V2: second explicit opt-in; phase 1 remains unchanged.
    const char* enabled = std::getenv("LICHII_AMD_UNIVERSAL_FG");
    return enabled && std::string_view(enabled) == "1";
}

bool UniversalAmdOpticalFlow::IsRequested() {
    const char* const enabled = std::getenv("LICHII_AMD_UNIVERSAL_OF");
    return enabled && std::string_view(enabled) == "1";
}

UniversalAmdOpticalFlow::UniversalAmdOpticalFlow(const Device& device, Scheduler& scheduler,
    MemoryAllocator& allocator, VkExtent2D extent, VkFormat format,
    VkImage motion_target, VkImageView motion_view, VkExtent2D motion_extent)
    : impl(std::make_unique<Impl>(device, scheduler, allocator, extent, format,
                                  motion_target, motion_view, motion_extent)) {}

UniversalAmdOpticalFlow::~UniversalAmdOpticalFlow() = default;

bool UniversalAmdOpticalFlow::Failed() const {
    return impl->failed.load(std::memory_order_relaxed);
}

void UniversalAmdOpticalFlow::Dispatch(VkImage current, uint64_t frame_index) {
    impl->Dispatch(current, frame_index);
}
} // namespace Vulkan
