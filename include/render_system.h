#pragma once

#include <cstddef>
#include <functional>
#include <vulkan/vulkan.h>

#include "renderer.h"
#include "overlay.h"
#include "chunk_renderer.h"

namespace wf {

class RenderSystem {
public:
    RenderSystem() = default;

    void initialize_renderer(const Renderer::CreateInfo& info);
    void shutdown_renderer();
    void wait_idle() const;

    Renderer& renderer();
    const Renderer& renderer() const;

    OverlayRenderer& overlay();
    const OverlayRenderer& overlay() const;

    ChunkRenderer& chunk_renderer();
    const ChunkRenderer& chunk_renderer() const;

    Renderer::FrameContext begin_frame();
    void submit_frame(const Renderer::FrameContext& ctx);
    void present_frame(const Renderer::FrameContext& ctx);

    struct FrameCallbacks {
        std::function<void(Renderer::FrameContext&)> record;
        std::function<void()> on_not_acquired;
        std::function<void()> on_swapchain_recreated;
    };

    bool render_frame(const FrameCallbacks& callbacks);

    bool swapchain_needs_recreate() const;
    void recreate_swapchain();

    VkExtent2D swapchain_extent() const;
    std::size_t frame_count() const;
    std::size_t current_frame() const;
    VkFramebuffer framebuffer(uint32_t image_index) const;

    VkInstance instance() const;
    VkPhysicalDevice physical_device() const;
    VkDevice device() const;
    VkRenderPass render_pass() const;
    uint32_t graphics_queue_family() const;
    VkQueue graphics_queue() const;

private:
    Renderer renderer_{};
    OverlayRenderer overlay_{};
    ChunkRenderer chunk_renderer_{};
};

} // namespace wf
