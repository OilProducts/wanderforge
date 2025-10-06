#pragma once

#include <cstddef>
#include <vulkan/vulkan.h>

#include "renderer.h"
#include "overlay.h"
#include "chunk_renderer.h"

namespace wf {

class RenderSystem {
public:
    struct Bindings {
        Renderer* renderer = nullptr;
        OverlayRenderer* overlay = nullptr;
        ChunkRenderer* chunk_renderer = nullptr;
    };

    RenderSystem() = default;

    void bind(const Bindings& bindings);

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
    Renderer* renderer_ = nullptr;
    OverlayRenderer* overlay_ = nullptr;
    ChunkRenderer* chunk_renderer_ = nullptr;
};

} // namespace wf

