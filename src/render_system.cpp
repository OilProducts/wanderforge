#include "render_system.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>

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
    Renderer::FrameContext ctx = renderer_->begin_frame();
    if (ctx.acquired) {
        std::size_t fc = renderer_->frame_count();
        ensure_trash_capacity(fc);
        if (fc > 0) {
            std::size_t slot = ctx.frame_index % fc;
            trash_[slot].clear();
        }
    }
    return ctx;
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

void RenderSystem::rebuild_swapchain_dependents(const char* shader_dir,
                                                bool device_local_enabled,
                                                VkDeviceSize pool_vtx_bytes,
                                                VkDeviceSize pool_idx_bytes,
                                                bool log_pool) {
    if (!renderer_) {
        return;
    }

    ensure_trash_capacity(renderer_->frame_count());

    VkPhysicalDevice physical = renderer_->physical_device();
    VkDevice device = renderer_->device();
    VkRenderPass render_pass = renderer_->render_pass();
    VkExtent2D extent = renderer_->swapchain_extent();

    if (!overlay_) {
        overlay_initialized_ = false;
    } else if (!overlay_initialized_) {
        overlay_->init(physical, device, render_pass, extent, shader_dir);
        overlay_initialized_ = true;
    } else {
        overlay_->recreate_swapchain(render_pass, extent, shader_dir);
    }

    if (!chunk_renderer_) {
        chunk_renderer_initialized_ = false;
    } else if (!chunk_renderer_initialized_) {
        chunk_renderer_->init(physical, device, render_pass, extent, shader_dir);
        chunk_renderer_initialized_ = true;
    } else {
        chunk_renderer_->recreate(render_pass, extent, shader_dir);
    }

    if (chunk_renderer_) {
        chunk_renderer_->set_device_local(device_local_enabled);
        chunk_renderer_->set_transfer_context(renderer_->graphics_queue_family(), renderer_->graphics_queue());
        chunk_renderer_->set_pool_caps_bytes(pool_vtx_bytes, pool_idx_bytes);
        chunk_renderer_->set_logging(log_pool);
    }
}

void RenderSystem::cleanup_swapchain_dependents() {
    for (auto& bucket : trash_) {
        bucket.clear();
    }
    trash_.clear();

    chunks_.clear();

    if (!renderer_) {
        overlay_initialized_ = false;
        chunk_renderer_initialized_ = false;
        return;
    }

    VkDevice device = renderer_->device();
    if (!device) {
        overlay_initialized_ = false;
        chunk_renderer_initialized_ = false;
        return;
    }

    if (overlay_initialized_ && overlay_) {
        overlay_->cleanup(device);
        overlay_initialized_ = false;
    }
    if (chunk_renderer_initialized_ && chunk_renderer_) {
        chunk_renderer_->cleanup(device);
        chunk_renderer_initialized_ = false;
    }
}

bool RenderSystem::upload_chunk_mesh(const ChunkMeshData& data, bool log_stream) {
    if (!chunk_renderer_ || data.vertex_count == 0 || data.index_count == 0) {
        return false;
    }

    ChunkInstance chunk;
    chunk.chunk_renderer = chunk_renderer_;
    chunk.index_count = static_cast<uint32_t>(data.index_count);
    chunk.vertex_count = static_cast<uint32_t>(data.vertex_count);

    if (!chunk_renderer_->upload_mesh(data.vertices,
                                      data.vertex_count,
                                      data.indices,
                                      data.index_count,
                                      chunk.first_index,
                                      chunk.base_vertex)) {
        if (log_stream) {
            std::cerr << "[stream] skip upload (pool full): face=" << data.key.face
                      << " i=" << data.key.i << " j=" << data.key.j << " k=" << data.key.k
                      << " vtx=" << data.vertex_count << " idx=" << data.index_count << '\n';
        }
        return false;
    }

    chunk.center[0] = data.center[0];
    chunk.center[1] = data.center[1];
    chunk.center[2] = data.center[2];
    chunk.radius = data.radius;
    chunk.key = data.key;

    bool replaced = false;
    for (std::size_t i = 0; i < chunks_.size(); ++i) {
        const FaceChunkKey& existing = chunks_[i].key;
        if (existing.face == chunk.key.face &&
            existing.i == chunk.key.i &&
            existing.j == chunk.key.j &&
            existing.k == chunk.key.k) {
            schedule_delete_chunk(std::move(chunks_[i]));
            chunks_[i] = std::move(chunk);
            replaced = true;
            if (log_stream) {
                std::cout << "[stream] replace: face=" << data.key.face
                          << " i=" << data.key.i << " j=" << data.key.j << " k=" << data.key.k
                          << " idx_count=" << data.index_count
                          << " vtx_count=" << data.vertex_count
                          << " first_index=" << chunks_[i].first_index
                          << " base_vertex=" << chunks_[i].base_vertex << '\n';
            }
            break;
        }
    }

    if (!replaced) {
        if (log_stream) {
            std::cout << "[stream] add: face=" << data.key.face
                      << " i=" << data.key.i << " j=" << data.key.j << " k=" << data.key.k
                      << " idx_count=" << data.index_count
                      << " vtx_count=" << data.vertex_count
                      << " first_index=" << chunk.first_index
                      << " base_vertex=" << chunk.base_vertex << '\n';
        }
        chunks_.push_back(std::move(chunk));
    }

    return true;
}

bool RenderSystem::release_chunk(const FaceChunkKey& key, bool log_stream) {
    for (std::size_t i = 0; i < chunks_.size(); ++i) {
        const FaceChunkKey& existing = chunks_[i].key;
        if (existing.face == key.face && existing.i == key.i &&
            existing.j == key.j && existing.k == key.k) {
            if (log_stream) {
                std::cout << "[stream] release: face=" << key.face
                          << " i=" << key.i << " j=" << key.j << " k=" << key.k
                          << " idx_count=" << chunks_[i].index_count
                          << " vtx_count=" << chunks_[i].vertex_count << '\n';
            }
            schedule_delete_chunk(std::move(chunks_[i]));
            chunks_.erase(chunks_.begin() + i);
            return true;
        }
    }
    return false;
}

void RenderSystem::prune_chunks_outside(int face,
                                         std::int64_t ci,
                                         std::int64_t cj,
                                         int span,
                                         bool log_stream,
                                         std::vector<FaceChunkKey>& out_removed) {
    for (std::size_t i = 0; i < chunks_.size();) {
        const FaceChunkKey& key = chunks_[i].key;
        if (key.face != face || std::llabs(key.i - ci) > span || std::llabs(key.j - cj) > span) {
            if (log_stream) {
                std::cout << "[stream] prune: face=" << key.face << " i=" << key.i
                          << " j=" << key.j << " k=" << key.k
                          << " first_index=" << chunks_[i].first_index
                          << " base_vertex=" << chunks_[i].base_vertex
                          << " idx_count=" << chunks_[i].index_count
                          << " vtx_count=" << chunks_[i].vertex_count << '\n';
            }
            out_removed.push_back(key);
            schedule_delete_chunk(std::move(chunks_[i]));
            chunks_.erase(chunks_.begin() + i);
        } else {
            ++i;
        }
    }
}

void RenderSystem::prune_chunks_multi(std::span<const AllowRegion> allows,
                                      bool log_stream,
                                      std::vector<FaceChunkKey>& out_removed) {
    auto inside_any = [&](const FaceChunkKey& key) {
        for (const auto& allow : allows) {
            if (key.face != allow.face) {
                continue;
            }
            if (std::llabs(key.i - allow.ci) > allow.span) {
                continue;
            }
            if (std::llabs(key.j - allow.cj) > allow.span) {
                continue;
            }
            std::int64_t kmin = allow.ck - allow.k_down;
            std::int64_t kmax = allow.ck + allow.k_up;
            if (key.k < kmin || key.k > kmax) {
                continue;
            }
            return true;
        }
        return allows.empty();
    };

    for (std::size_t i = 0; i < chunks_.size();) {
        const FaceChunkKey& key = chunks_[i].key;
        if (!inside_any(key)) {
            if (log_stream) {
                std::cout << "[stream] prune: face=" << key.face << " i=" << key.i
                          << " j=" << key.j << " k=" << key.k
                          << " first_index=" << chunks_[i].first_index
                          << " base_vertex=" << chunks_[i].base_vertex
                          << " idx_count=" << chunks_[i].index_count
                          << " vtx_count=" << chunks_[i].vertex_count << '\n';
            }
            out_removed.push_back(key);
            schedule_delete_chunk(std::move(chunks_[i]));
            chunks_.erase(chunks_.begin() + i);
        } else {
            ++i;
        }
    }
}

std::span<const RenderSystem::ChunkInstance> RenderSystem::chunk_instances() const {
    return std::span<const ChunkInstance>(chunks_.data(), chunks_.size());
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

void RenderSystem::ensure_trash_capacity(std::size_t frame_count) {
    if (frame_count == 0) {
        trash_.clear();
        return;
    }
    if (trash_.size() != frame_count) {
        trash_.resize(frame_count);
    }
}

void RenderSystem::schedule_delete_chunk(ChunkInstance&& chunk) {
    if (!renderer_) {
        return;
    }

    std::size_t frame_count = renderer_->frame_count();
    if (frame_count == 0) {
        return;
    }

    ensure_trash_capacity(frame_count);
    std::size_t slot = (renderer_->current_frame() + 1) % frame_count;
    trash_[slot].push_back(std::move(chunk));
}

} // namespace wf

