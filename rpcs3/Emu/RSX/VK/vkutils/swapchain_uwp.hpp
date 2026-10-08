#pragma once
#include "swapchain_core.h"
#include "Embedded/vulkan_frame.h"
namespace vk
{
class swapchain_UWP final : public native_swapchain_base
{
public:
    using native_swapchain_base::native_swapchain_base;
    bool init() override
    {
        if (!m_width || !m_height) return false;
        swapchain_images.clear();
        init_swapchain_images(dev, 3);
        return true;
    }
    void create(display_handle_t&) override {}
    void destroy(bool full = true) override
    {
        swapchain_images.clear();
        if (full) dev.destroy();
    }
    void end_frame(command_buffer& cmd, u32 index) override
    {
        native_swapchain_base::end_frame(cmd, index);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    VkResult present(VkSemaphore, u32 index) override
    {
        // VKGSRender::present flushes submission, not GPU completion. Mapping
        // before the copy finishes produces stale/alternating frames.
        const auto status = vkQueueWaitIdle(dev.get_graphics_queue());
        if (status != VK_SUCCESS) return status;
        auto& image = swapchain_images.at(index);
        auto* pixels = image.second->get_pixels();
        try { embedded_publish_bgra(pixels, m_width, m_height); }
        catch (...) { image.second->free_pixels(); image.first = false; throw; }
        image.second->free_pixels();
        image.first = false;
        return VK_SUCCESS;
    }
};
using swapchain_NATIVE = swapchain_UWP;
}
