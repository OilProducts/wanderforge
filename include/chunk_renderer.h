// ChunkRenderer: encapsulates chunk graphics pipeline and drawing
#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <string>

namespace wf {

struct ChunkDrawItem {
    VkBuffer vbuf = VK_NULL_HANDLE;
    VkBuffer ibuf = VK_NULL_HANDLE;
    uint32_t index_count = 0;
};

class ChunkRenderer {
public:
    void init(VkDevice device, VkRenderPass renderPass, VkExtent2D extent, const char* shaderDir);
    void recreate(VkRenderPass renderPass, VkExtent2D extent, const char* shaderDir);
    void cleanup(VkDevice device);
    bool is_ready() const { return pipeline_ != VK_NULL_HANDLE && layout_ != VK_NULL_HANDLE; }

    // Record draw commands for provided chunks; expects MVP as 16 floats (row-major, multiplied in shader as provided)
    void record(VkCommandBuffer cmd, const float mvp[16], const std::vector<ChunkDrawItem>& items);

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::string shader_dir_;
};

} // namespace wf
