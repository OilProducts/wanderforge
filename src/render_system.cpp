#include "render_system.h"

namespace wf {

void RenderSystem::initialize_renderer(const Renderer::CreateInfo& info) {
    renderer_.initialize(info);
}

void RenderSystem::shutdown_renderer() {
    renderer_.shutdown();
}

void RenderSystem::wait_idle() const {
    renderer_.wait_idle();
}

Renderer& RenderSystem::renderer() {
    return renderer_;
}

const Renderer& RenderSystem::renderer() const {
    return renderer_;
}

OverlayRenderer& RenderSystem::overlay() {
    return overlay_;
}

const OverlayRenderer& RenderSystem::overlay() const {
    return overlay_;
}

ChunkRenderer& RenderSystem::chunk_renderer() {
    return chunk_renderer_;
}

const ChunkRenderer& RenderSystem::chunk_renderer() const {
    return chunk_renderer_;
}

Renderer::FrameContext RenderSystem::begin_frame() {
    return renderer_.begin_frame();
}

void RenderSystem::submit_frame(const Renderer::FrameContext& ctx) {
    renderer_.submit_frame(ctx);
}

void RenderSystem::present_frame(const Renderer::FrameContext& ctx) {
    renderer_.present_frame(ctx);
}

bool RenderSystem::swapchain_needs_recreate() const {
    return renderer_.swapchain_needs_recreate();
}

void RenderSystem::recreate_swapchain() {
    renderer_.recreate_swapchain();
}

VkExtent2D RenderSystem::swapchain_extent() const {
    return renderer_.swapchain_extent();
}

std::size_t RenderSystem::frame_count() const {
    return renderer_.frame_count();
}

std::size_t RenderSystem::current_frame() const {
    return renderer_.current_frame();
}

VkFramebuffer RenderSystem::framebuffer(uint32_t image_index) const {
    return renderer_.framebuffer(image_index);
}

VkInstance RenderSystem::instance() const {
    return renderer_.instance();
}

VkPhysicalDevice RenderSystem::physical_device() const {
    return renderer_.physical_device();
}

VkDevice RenderSystem::device() const {
    return renderer_.device();
}

VkRenderPass RenderSystem::render_pass() const {
    return renderer_.render_pass();
}

uint32_t RenderSystem::graphics_queue_family() const {
    return renderer_.graphics_queue_family();
}

VkQueue RenderSystem::graphics_queue() const {
    return renderer_.graphics_queue();
}

bool RenderSystem::render_frame(const FrameCallbacks& callbacks) {
    Renderer::FrameContext ctx = renderer_.begin_frame();
    if (!ctx.acquired) {
        if (callbacks.on_not_acquired) {
            callbacks.on_not_acquired();
        }
        if (renderer_.swapchain_needs_recreate()) {
            renderer_.recreate_swapchain();
            if (callbacks.on_swapchain_recreated) {
                callbacks.on_swapchain_recreated();
            }
        }
        return false;
    }

    if (callbacks.record) {
        callbacks.record(ctx);
    }

    renderer_.submit_frame(ctx);
    renderer_.present_frame(ctx);

    if (renderer_.swapchain_needs_recreate()) {
        renderer_.recreate_swapchain();
        if (callbacks.on_swapchain_recreated) {
            callbacks.on_swapchain_recreated();
        }
    }

    return true;
}

} // namespace wf
