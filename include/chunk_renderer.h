// ChunkRenderer: encapsulates chunk graphics pipeline and drawing
#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <string>

namespace wf {

struct ChunkDrawItem {
    // Direct draw path (legacy): bind per-chunk buffers
    VkBuffer vbuf = VK_NULL_HANDLE;
    VkBuffer ibuf = VK_NULL_HANDLE;
    uint32_t index_count = 0;
    // Indirect path (batched pool): use offsets into shared buffers
    uint32_t first_index = 0;
    int32_t  base_vertex = 0;
};

class ChunkRenderer {
public:
    void init(VkPhysicalDevice phys, VkDevice device, VkRenderPass renderPass, VkExtent2D extent, const char* shaderDir);
    void recreate(VkRenderPass renderPass, VkExtent2D extent, const char* shaderDir);
    void cleanup(VkDevice device);

    // Record draw commands for provided chunks; expects MVP as 16 floats (row-major, multiplied in shader as provided)
    void record(VkCommandBuffer cmd, const float mvp[16], const std::vector<ChunkDrawItem>& items);

    bool is_ready() const { return pipeline_ != VK_NULL_HANDLE; }

    // Upload a mesh into the shared pools; returns offsets for indirect drawing
    void upload_mesh(const struct Vertex* vertices, size_t vcount,
                     const uint32_t* indices, size_t icount,
                     uint32_t& out_first_index,
                     int32_t& out_base_vertex);

private:
    // Device
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::string shader_dir_;

    // Shared mesh pools (host-visible for now)
    VkBuffer vtx_pool_ = VK_NULL_HANDLE;
    VkDeviceMemory vtx_mem_ = VK_NULL_HANDLE;
    VkDeviceSize vtx_capacity_ = 0; // bytes
    VkDeviceSize vtx_used_ = 0;     // bytes

    VkBuffer idx_pool_ = VK_NULL_HANDLE;
    VkDeviceMemory idx_mem_ = VK_NULL_HANDLE;
    VkDeviceSize idx_capacity_ = 0; // bytes
    VkDeviceSize idx_used_ = 0;     // bytes

    // Indirect command buffer (rebuilt per record)
    VkBuffer indirect_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory indirect_mem_ = VK_NULL_HANDLE;
    VkDeviceSize indirect_capacity_cmds_ = 0; // number of commands capacity

    bool ensure_pool_capacity(VkDeviceSize add_vtx_bytes, VkDeviceSize add_idx_bytes);
    void ensure_indirect_capacity(size_t drawCount);
};

} // namespace wf
