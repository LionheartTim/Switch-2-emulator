// Native Vulkan bridge to the official AMD FidelityFX SDK 1.1.4; GPL-3.0-or-later.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
class Device;
class Scheduler;
class MemoryAllocator;

/// Calls AMD's actual ffxFsr3UpscalerContextDispatch rather than Lichii's shader reimplementation.
/// This only supports the game-temporal path, which provides true depth and motion vectors.
class AmdFsrSdkUpscaler final {
public:
    struct Input {
        VkImage color{};
        VkImage depth{};
        VkImage motion{};
        VkFormat color_format{};
        VkFormat motion_format{};
        VkExtent2D render{};
        VkExtent2D upscale{};
        std::array<float, 2> jitter{};
        std::array<float, 2> motion_scale{};
        float frame_ms{};
        bool reset{};
        bool sharpen{};
        float sharpness{};
    };

    AmdFsrSdkUpscaler(const Device& device, Scheduler& scheduler,
                       MemoryAllocator& allocator);
    ~AmdFsrSdkUpscaler();
    AmdFsrSdkUpscaler(const AmdFsrSdkUpscaler&) = delete;
    AmdFsrSdkUpscaler& operator=(const AmdFsrSdkUpscaler&) = delete;

    void Upscale(const Input& in);

    // Opt-in official AMD Frame Interpolation. Always disabled without the
    // EDEN_FFX3_NATIVE_FG=1 environment variable and matching Graphics settings.
    [[nodiscard]] static bool NativeFgRequested();
    static void PublishNative(const std::shared_ptr<AmdFsrSdkUpscaler>& instance);
    [[nodiscard]] static std::shared_ptr<AmdFsrSdkUpscaler> LatestNative();
    [[nodiscard]] uint64_t NativeFrameSerial() const;
    [[nodiscard]] bool NativeReady(VkExtent2D extent, VkFormat format) const;
    bool QueueNativeOpticalFlow(VkImage presented, VkFormat format,
                                VkExtent2D extent, uint64_t serial);
    bool QueueNativeGeneration(VkImage destination, VkFormat format,
                               VkExtent2D extent, uint64_t serial);
    VkImage OutputImage() const { return *output_image; }
    VkImageView OutputView() const { return *output_view; }
    VkSampler PointSampler() const { return *point_sampler; }

private:
    void Recreate(VkExtent2D render, VkExtent2D upscale);
    void TearDown();
    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& allocator;
    struct NativeFrameGeneration;
    std::unique_ptr<NativeFrameGeneration> native_fg;
    std::atomic<uint64_t> native_serial{0};
    uint64_t native_present_serial{};
    uint64_t native_last_present_serial{};
    VkFormat native_output_format{VK_FORMAT_UNDEFINED};
    VkImage native_presented_image{VK_NULL_HANDLE};
    bool native_present_reset{};
    bool native_reset{};
    float native_frame_ms{1000.0f / 30.0f};
    float native_present_ms{1000.0f / 30.0f};
    FfxFsr3UpscalerContext context{};
    bool context_live{};
    VkDeviceContext vk_device_context{};
    FfxInterface interface{};
    std::vector<uint8_t> scratch;
    vk::Image output_image;
    vk::ImageView output_view;
    vk::Sampler point_sampler;
    VkExtent2D render_extent{};
    VkExtent2D upscale_extent{};
};
}  // namespace Vulkan
