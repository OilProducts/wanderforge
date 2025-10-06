#include "render_system.h"

#include <cassert>

namespace wf {

void RenderSystem::bind(const Bindings& bindings) {
    renderer_ = bindings.renderer;
    overlay_ = bindings.overlay;
    chunk_renderer_ = bindings.chunk_renderer;
}

void RenderSystem::initialize_renderer(const Renderer::CreateInfo& info) {
    assert(renderer_);
    renderer_->initialize(info);
}

void RenderSystem::shutdown_renderer() {
    if (renderer_) {
        renderer_->shutdown();
    }
}

void RenderSystem::wait_idle() const {
    if (renderer_) {
        renderer_->wait_idle();
    }
}

Renderer& RenderSystem::renderer() {
    assert(renderer_);
    return *renderer_;
}

const Renderer& RenderSystem::renderer() const {
    assert(renderer_);
    return *renderer_;
}

OverlayRenderer& RenderSystem::overlay() {
    assert(overlay_);
    return *overlay_;
}

const OverlayRenderer& RenderSystem::overlay() const {
    assert(overlay_);
    return *overlay_;
}

ChunkRenderer& RenderSystem::chunk_renderer() {
    assert(chunk_renderer_);
    return *chunk_renderer_;
}

const ChunkRenderer& RenderSystem::chunk_renderer() const {
    assert(chunk_renderer_);
    return *chunk_renderer_;
}

Renderer::FrameContext RenderSystem::begin_frame() {
    assert(renderer_);
    return renderer_->begin_frame();
}

void RenderSystem::submit_frame(const Renderer::FrameContext& ctx) {
    assert(renderer_);
    renderer_->submit_frame(ctx);
}

void RenderSystem::present_frame(const Renderer::FrameContext& ctx) {
    assert(renderer_);
    renderer_->present_frame(ctx);
}

bool RenderSystem::swapchain_needs_recreate() const {
    return renderer_ ? renderer_->swapchain_needs_recreate() : false;
}

void RenderSystem::recreate_swapchain() {
    if (renderer_) {
        renderer_->recreate_swapchain();
    }
}

VkExtent2D RenderSystem::swapchain_extent() const {
    if (!renderer_) {
        return VkExtent2D{0, 0};
    }
    return renderer_->swapchain_extent();
}

std::size_t RenderSystem::frame_count() const {
    return renderer_ ? renderer_->frame_count() : 0;
}

std::size_t RenderSystem::current_frame() const {
    return renderer_ ? renderer_->current_frame() : 0;
}

VkFramebuffer RenderSystem::framebuffer(uint32_t image_index) const {
    if (!renderer_) {
        return VK_NULL_HANDLE;
    }
    return renderer_->framebuffer(image_index);
}

VkInstance RenderSystem::instance() const {
    return renderer_ ? renderer_->instance() : VK_NULL_HANDLE;
}

VkPhysicalDevice RenderSystem::physical_device() const {
    return renderer_ ? renderer_->physical_device() : VK_NULL_HANDLE;
}

VkDevice RenderSystem::device() const {
    return renderer_ ? renderer_->device() : VK_NULL_HANDLE;
}

VkRenderPass RenderSystem::render_pass() const {
    return renderer_ ? renderer_->render_pass() : VK_NULL_HANDLE;
}

uint32_t RenderSystem::graphics_queue_family() const {
    return renderer_ ? renderer_->graphics_queue_family() : 0;
}

VkQueue RenderSystem::graphics_queue() const {
    return renderer_ ? renderer_->graphics_queue() : VK_NULL_HANDLE;
}

} // namespace wf

