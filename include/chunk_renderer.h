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
    void set_logging(bool enabled) { log_ = enabled; }
    void get_pool_usage(VkDeviceSize& v_used, VkDeviceSize& v_cap, VkDeviceSize& i_used, VkDeviceSize& i_cap) const {
        v_used = vtx_used_; v_cap = vtx_capacity_; i_used = idx_used_; i_cap = idx_capacity_;
    }
    void set_pool_log_every(int n_frames) { log_every_n_ = n_frames > 0 ? n_frames : 120; }
    void set_pool_caps_bytes(VkDeviceSize vtx_bytes, VkDeviceSize idx_bytes) { vtx_initial_cap_ = vtx_bytes; idx_initial_cap_ = idx_bytes; }
    void set_device_local(bool enable) { use_device_local_ = enable; }
    void set_transfer_context(uint32_t queue_family, VkQueue queue) { transfer_queue_family_ = queue_family; transfer_queue_ = queue; }

    // Upload a mesh into the shared pools; returns offsets for indirect drawing
    // Returns false if the pool has no space (mesh not uploaded)
    bool upload_mesh(const struct Vertex* vertices, size_t vcount,
                     const uint32_t* indices, size_t icount,
                     uint32_t& out_first_index,
                     int32_t& out_base_vertex);
    void free_mesh(uint32_t first_index, uint32_t index_count,
                   int32_t base_vertex, uint32_t vertex_count);

private:
    // Device
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::string shader_dir_;
    bool log_ = false;
    int log_every_n_ = 120;
    int log_frame_cnt_ = 0;

    // Device-local toggle
    bool use_device_local_ = false;

    // Shared mesh pools (either host-visible or device-local)
    VkBuffer vtx_pool_ = VK_NULL_HANDLE;
    VkDeviceMemory vtx_mem_ = VK_NULL_HANDLE;
    VkDeviceSize vtx_capacity_ = 0; // bytes
    VkDeviceSize vtx_used_ = 0;     // bytes
    VkDeviceSize vtx_initial_cap_ = 64ull * 1024ull * 1024ull; // default 64MB

    VkBuffer idx_pool_ = VK_NULL_HANDLE;
    VkDeviceMemory idx_mem_ = VK_NULL_HANDLE;
    VkDeviceSize idx_capacity_ = 0; // bytes
    VkDeviceSize idx_used_ = 0;     // bytes
    VkDeviceSize idx_initial_cap_ = 64ull * 1024ull * 1024ull; // default 64MB
    // Free-list state for reuse
    VkDeviceSize vtx_tail_ = 0;
    VkDeviceSize idx_tail_ = 0;
    struct FreeBlock { VkDeviceSize off; VkDeviceSize size; };
    std::vector<FreeBlock> vtx_free_;
    std::vector<FreeBlock> idx_free_;

    // Indirect command buffer (rebuilt per record)
    VkBuffer indirect_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory indirect_mem_ = VK_NULL_HANDLE;
    VkDeviceSize indirect_capacity_cmds_ = 0; // number of commands capacity

    // Transfer context (for staging copies when using device-local pools)
    uint32_t transfer_queue_family_ = 0;
    VkQueue transfer_queue_ = VK_NULL_HANDLE;
    VkCommandPool transfer_pool_ = VK_NULL_HANDLE;
    VkFence transfer_fence_ = VK_NULL_HANDLE;

    bool ensure_pool_capacity(VkDeviceSize add_vtx_bytes, VkDeviceSize add_idx_bytes);
    static inline VkDeviceSize align_up(VkDeviceSize x, VkDeviceSize a) {
        if (a == 0) return x;
        return ((x + a - 1) / a) * a;
    }
    bool alloc_from_pool(VkDeviceSize bytes, VkDeviceSize alignment, bool isVertex, VkDeviceSize& out_offset);
    void free_to_pool(VkDeviceSize offset, VkDeviceSize bytes, bool isVertex);
    void ensure_indirect_capacity(size_t drawCount);
    void ensure_transfer_objects();
};

} // namespace wf
