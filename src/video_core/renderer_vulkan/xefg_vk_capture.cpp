// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/xefg_vk_capture.h"

#include <cstdlib>
#include <cstring>

#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {

bool CaptureEnabled() {
    const char* value = std::getenv("EDEN_XEFG_CAPTURE");
    return value && std::strcmp(value, "1") == 0;
}

} // namespace

XeFGVKCapture::XeFGVKCapture(MemoryAllocator& allocator_, Scheduler& scheduler_)
    : allocator{allocator_}, scheduler{scheduler_}, enabled{CaptureEnabled()} {
    if (enabled) {
        LOG_INFO(Render_Vulkan, "[XeFG] Color frame capture enabled (MVP 1)");
    }
}

void XeFGVKCapture::Capture(VkImage color, VkExtent2D extent_, VkFormat format_) {
    if (!enabled || color == VK_NULL_HANDLE || extent_.width == 0 || extent_.height == 0 ||
        format_ == VK_FORMAT_UNDEFINED) {
        return;
    }

    if (extent.width != extent_.width || extent.height != extent_.height || format != format_) {
        if (extent.width != 0 && extent.height != 0) {
            scheduler.Finish();
        }
        for (auto& slot : history) {
            slot.image = {};
            slot.initialized = false;
        }
        extent = extent_;
        format = format_;
        next_slot = 0;
        captured_frames = 0;
        const VkImageCreateInfo image_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {extent.width, extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        try {
            for (auto& slot : history) {
                slot.image = allocator.TryCreateImage(image_info);
                if (!slot.image) {
                    enabled = false;
                    LOG_WARNING(Render_Vulkan, "[XeFG] Capture disabled: history image allocation failed");
                    return;
                }
            }
        } catch (const vk::Exception& error) {
            enabled = false;
            LOG_WARNING(Render_Vulkan, "[XeFG] Capture disabled: could not create history images ({})",
                        error.what());
            return;
        }
        LOG_INFO(Render_Vulkan, "[XeFG] Capture history allocated: {}x{}, format {:#x}",
                 extent.width, extent.height, static_cast<u32>(format));
    }

    Slot& slot = history[next_slot];
    const VkImage destination = *slot.image;
    const VkImageLayout destination_layout =
        slot.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    const VkAccessFlags destination_access = slot.initialized ? VK_ACCESS_MEMORY_READ_BIT : 0;
    const VkImage source = color;
    const VkExtent2D copy_extent = extent;
    scheduler.Record([source, destination, destination_layout, destination_access,
                      copy_extent](vk::CommandBuffer command_buffer) {
        const std::array pre_barriers{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source,
                .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            },
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = destination_access,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = destination_layout,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = destination,
                .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            },
        };
        command_buffer.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT, {}, {}, {}, pre_barriers);
        const VkImageCopy copy{
            .srcSubresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstSubresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .extent{copy_extent.width, copy_extent.height, 1},
        };
        const std::array copies{copy};
        command_buffer.CopyImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copies);
        const std::array post_barriers{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source,
                .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            },
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = destination,
                .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            },
        };
        command_buffer.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, {}, {}, {}, post_barriers);
    });

    slot.initialized = true;
    const u64 captured_index = frame_index++;
    if (captured_frames < 2 || captured_index % 120 == 0) {
        LOG_INFO(Render_Vulkan, "[XeFG] Frame {} color capture queued ({}x{}, format {:#x})",
                 captured_index, extent.width, extent.height, static_cast<u32>(format));
    }
    if (captured_frames == 1) {
        LOG_INFO(Render_Vulkan, "[XeFG] Two-frame color history queued; generic compositor has no depth or motion input");
    }
    ++captured_frames;
    next_slot = (next_slot + 1) % history.size();
}

} // namespace Vulkan