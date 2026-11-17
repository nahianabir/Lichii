// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class MemoryAllocator;
class Scheduler;

class XeFGVKCapture {
public:
    XeFGVKCapture(MemoryAllocator& allocator, Scheduler& scheduler);

    void Capture(VkImage color, VkExtent2D extent, VkFormat format);

private:
    struct Slot {
        vk::Image image;
        bool initialized{};
    };

    MemoryAllocator& allocator;
    Scheduler& scheduler;
    std::array<Slot, 2> history;
    VkExtent2D extent{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    u64 frame_index{};
    u32 captured_frames{};
    size_t next_slot{};
    bool enabled{};
};

} // namespace Vulkan