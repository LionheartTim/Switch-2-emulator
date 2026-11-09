// SPDX-FileCopyrightText: Copyright 2026 Lichii experimental integration
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <vulkan/vulkan.h>

namespace Vulkan {
class Device;
class Scheduler;
class MemoryAllocator;
class UniversalAmdOpticalFlow final {
public:
    // Independent of the game's DLSS/temporal-upscaling path.
    // Explicit opt-in so the established built-in interpolator stays untouched.
    static bool IsRequested();
    UniversalAmdOpticalFlow(const Device& device, Scheduler& scheduler,
                            MemoryAllocator& allocator, VkExtent2D extent, VkFormat format,
                            VkImage motion_target, VkImageView motion_view,
                            VkExtent2D motion_extent);
    ~UniversalAmdOpticalFlow();
    UniversalAmdOpticalFlow(const UniversalAmdOpticalFlow&) = delete;
    UniversalAmdOpticalFlow& operator=(const UniversalAmdOpticalFlow&) = delete;
    // LICHII_AMD_UNIVERSAL_FG_V2: output affects images only with explicit second opt-in.
    static bool UseForInterpolation();
    bool Failed() const;
    void Dispatch(VkImage current, uint64_t frame_index);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Vulkan
