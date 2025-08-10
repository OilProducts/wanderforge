#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace wf {

// Minimal in-window text overlay renderer using a tiny CPU 6x8 bitmap font.
class OverlayRenderer {
public:
    void init(VkPhysicalDevice phys, VkDevice dev, VkRenderPass renderPass, VkExtent2D extent, const char* shaderDir);
    void recreate_swapchain(VkRenderPass renderPass, VkExtent2D extent, const char* shaderDir);
    void cleanup(VkDevice dev);

    void build_text(size_t frameSlot, const char* text, int screenW, int screenH);
    void record_draw(VkCommandBuffer cmd, size_t frameSlot);

private:
    // Pipeline
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};

    // Per-frame overlay vertex buffers (NDC position + RGBA)
    static constexpr size_t kFrames = 2;
    VkBuffer vbuf_[kFrames] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory vmem_[kFrames] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    uint32_t vertex_count_[kFrames] = {0, 0};
    VkDeviceSize capacity_bytes_[kFrames] = {0, 0};

    // Helpers
    uint32_t find_memory_type(uint32_t typeBits, VkMemoryPropertyFlags properties);
    void create_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf, VkDeviceMemory& mem, const void* data);
};

} // namespace wf

