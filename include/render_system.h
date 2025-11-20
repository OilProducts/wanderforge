#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan.h>

#include "chunk.h"
#include "chunk_renderer.h"
#include "mesh.h"
#include "overlay.h"
#include "renderer.h"
#include "vk_handle.h"

namespace wf {

class RenderSystem {
public:
    struct Bindings {
        Renderer* renderer = nullptr;
        OverlayRenderer* overlay = nullptr;
        ChunkRenderer* chunk_renderer = nullptr;
    };

    struct ChunkMeshData {
        FaceChunkKey key{};
        const Vertex* vertices = nullptr;
        std::size_t vertex_count = 0;
        const uint32_t* indices = nullptr;
        std::size_t index_count = 0;
        float center[3] = {0.0f, 0.0f, 0.0f};
        float radius = 0.0f;
    };

    struct AllowRegion {
        int face = -1;
        std::int64_t ci = 0;
        std::int64_t cj = 0;
        std::int64_t ck = 0;
        int span = 0;
        int k_down = 0;
        int k_up = 0;
    };

    struct ChunkInstance {
        wf::vk::UniqueBuffer vbuf;
        wf::vk::UniqueDeviceMemory vmem;
        wf::vk::UniqueBuffer ibuf;
        wf::vk::UniqueDeviceMemory imem;

        uint32_t index_count = 0;
        uint32_t first_index = 0;
        int32_t base_vertex = 0;
        uint32_t vertex_count = 0;
        float center[3] = {0.0f, 0.0f, 0.0f};
        float radius = 0.0f;
        FaceChunkKey key{0, 0, 0, 0};
        ChunkRenderer* chunk_renderer = nullptr;

        ChunkInstance() = default;
        ~ChunkInstance() { release(); }
        ChunkInstance(const ChunkInstance&) = delete;
        ChunkInstance& operator=(const ChunkInstance&) = delete;

        ChunkInstance(ChunkInstance&& other) noexcept { move_from(std::move(other)); }
        ChunkInstance& operator=(ChunkInstance&& other) noexcept {
            if (this != &other) {
                release();
                move_from(std::move(other));
            }
            return *this;
        }

        void release() {
            if (chunk_renderer && index_count > 0 && vertex_count > 0) {
                chunk_renderer->free_mesh(first_index, index_count, base_vertex, vertex_count);
            }
            chunk_renderer = nullptr;
            index_count = 0;
            first_index = 0;
            base_vertex = 0;
            vertex_count = 0;
            radius = 0.0f;
            center[0] = center[1] = center[2] = 0.0f;
            key = FaceChunkKey{0, 0, 0, 0};
            vbuf.reset();
            vmem.reset();
            ibuf.reset();
            imem.reset();
        }

    private:
        void move_from(ChunkInstance&& other) noexcept {
            vbuf = std::move(other.vbuf);
            vmem = std::move(other.vmem);
            ibuf = std::move(other.ibuf);
            imem = std::move(other.imem);
            index_count = other.index_count;
            first_index = other.first_index;
            base_vertex = other.base_vertex;
            vertex_count = other.vertex_count;
            center[0] = other.center[0];
            center[1] = other.center[1];
            center[2] = other.center[2];
            radius = other.radius;
            key = other.key;
            chunk_renderer = other.chunk_renderer;

            other.index_count = 0;
            other.first_index = 0;
            other.base_vertex = 0;
            other.vertex_count = 0;
            other.radius = 0.0f;
            other.center[0] = other.center[1] = other.center[2] = 0.0f;
            other.key = FaceChunkKey{0, 0, 0, 0};
            other.chunk_renderer = nullptr;
        }
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

    void rebuild_swapchain_dependents(const char* shader_dir,
                                      bool device_local_enabled,
                                      VkDeviceSize pool_vtx_bytes,
                                      VkDeviceSize pool_idx_bytes,
                                      bool log_pool);
    void cleanup_swapchain_dependents();

    bool upload_chunk_mesh(const ChunkMeshData& data, bool log_stream);
    bool release_chunk(const FaceChunkKey& key, bool log_stream);
    void prune_chunks_outside(int face,
                              std::int64_t ci,
                              std::int64_t cj,
                              int span,
                              bool log_stream,
                              std::vector<FaceChunkKey>& out_removed);
    void prune_chunks_multi(std::span<const AllowRegion> allows,
                            bool log_stream,
                            std::vector<FaceChunkKey>& out_removed);

    std::span<const ChunkInstance> chunk_instances() const;

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
    void ensure_trash_capacity(std::size_t frame_count);
    void schedule_delete_chunk(ChunkInstance&& chunk);

    Renderer* renderer_ = nullptr;
    OverlayRenderer* overlay_ = nullptr;
    ChunkRenderer* chunk_renderer_ = nullptr;

    std::vector<ChunkInstance> chunks_;
    std::vector<std::vector<ChunkInstance>> trash_;
    bool overlay_initialized_ = false;
    bool chunk_renderer_initialized_ = false;
};

} // namespace wf
