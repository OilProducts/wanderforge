#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <cstdint>
#include <unordered_map>
#include <optional>
#include <memory>
#include "overlay.h"
#include "chunk_renderer.h"
#include "mesh.h"
#include "chunk_delta.h"
#include "chunk.h"
#include "chunk_streaming_manager.h"
#include "world_streaming_subsystem.h"
#include "world_runtime.h"
#include "planet.h"
#include "config_loader.h"
#include "ui/ui_context.h"
#include "ui/ui_backend.h"
#include "window_input.h"
#include "renderer.h"
#include "vk_handle.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <tuple>
#include <chrono>

struct GLFWwindow;

namespace wf {

class VulkanApp {
public:
    VulkanApp();
    ~VulkanApp();

    void run();
    void set_config_path(std::string path);

private:
    void init_window();
    void init_vulkan();
    void create_graphics_pipeline();
    // Legacy chunk pipeline removed; ChunkRenderer is authoritative
    void create_compute_pipeline();
    void record_command_buffer(const Renderer::FrameContext& ctx);
    void draw_frame();
    void update_input(float dt);
    void update_hud(float dt);
    void load_config();
    AppConfig snapshot_config() const;
    void apply_config(const AppConfig& cfg);
    void apply_resolution_option(std::size_t index);
    std::size_t find_resolution_index(int width, int height) const;
    int current_brush_dim() const;
    void update_streaming_runtime_settings();
    void reload_config_from_disk();
    void save_active_config();
    void flush_dirty_chunk_deltas();
    using VoxelHit = wf::VoxelHit;
    using MeshResult = ChunkStreamingManager::MeshResult;
    using LoadRequest = ChunkStreamingManager::LoadRequest;
    bool world_to_chunk_coords(const double pos[3], FaceChunkKey& key, int& lx, int& ly, int& lz, Int3& voxel_out) const;
    bool pick_voxel(VoxelHit& solid_hit, VoxelHit& empty_before);
    bool apply_voxel_edit(const VoxelHit& target, uint16_t new_material, int brush_dim = 1);
    void queue_chunk_remesh(const FaceChunkKey& key);
    void process_pending_remeshes();
    bool process_runtime_mesh_upload(const wf::MeshUpload& upload);
    void process_runtime_mesh_release(const FaceChunkKey& key);
    void apply_runtime_result(const WorldUpdateResult& result);

    void recreate_swapchain();
    void rebuild_swapchain_dependents();
    VkShaderModule load_shader_module(const std::string& path);
    void create_debug_axes_buffer();
    void destroy_debug_axes_buffer();
    void create_debug_axes_pipeline();
    void destroy_debug_axes_pipeline();

private:
    // Window
    WindowInput window_system_;
    int window_width_ = 1280;
    int window_height_ = 720;
    int framebuffer_width_ = 1280;
    int framebuffer_height_ = 720;
    std::size_t hud_resolution_index_ = 0;

    enum class ToolSelection {
        None,
        SmallShovel,
        LargeShovel
    };
    ToolSelection selected_tool_ = ToolSelection::None;

    // Renderer core
    Renderer renderer_;

    wf::vk::UniquePipelineLayout pipeline_layout_;
    wf::vk::UniquePipeline pipeline_triangle_;
    // Legacy chunk pipeline removed

    // Compute pipeline (no-op dispatch)
    wf::vk::UniquePipelineLayout pipeline_layout_compute_;
    wf::vk::UniquePipeline pipeline_compute_;
    bool compute_enabled_ = false; // gate no-op compute dispatch

    struct RenderChunk {
        wf::vk::UniqueBuffer vbuf;
        wf::vk::UniqueDeviceMemory vmem;
        wf::vk::UniqueBuffer ibuf;
        wf::vk::UniqueDeviceMemory imem;

        uint32_t index_count = 0;
        uint32_t first_index = 0;
        int32_t  base_vertex = 0;
        uint32_t vertex_count = 0;
        float center[3] = {0.0f, 0.0f, 0.0f};
        float radius = 0.0f;
        FaceChunkKey key{0,0,0,0};
        ChunkRenderer* chunk_renderer = nullptr;

        RenderChunk() = default;
        ~RenderChunk() { release(); }

        RenderChunk(const RenderChunk&) = delete;
        RenderChunk& operator=(const RenderChunk&) = delete;

        RenderChunk(RenderChunk&& other) noexcept { move_from(std::move(other)); }
        RenderChunk& operator=(RenderChunk&& other) noexcept {
            if (this != &other) {
                release();
                move_from(std::move(other));
            }
            return *this;
        }

        void release() {
            if (chunk_renderer && index_count > 0 && vertex_count > 0 && !vbuf && !ibuf) {
                chunk_renderer->free_mesh(first_index, index_count, base_vertex, vertex_count);
            }
            chunk_renderer = nullptr;
            index_count = 0;
            first_index = 0;
            base_vertex = 0;
            vertex_count = 0;
            radius = 0.0f;
            center[0] = center[1] = center[2] = 0.0f;
            key = FaceChunkKey{0,0,0,0};
            vbuf.reset();
            vmem.reset();
            ibuf.reset();
            imem.reset();
        }

    private:
        void move_from(RenderChunk&& other) noexcept {
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
            other.key = FaceChunkKey{0,0,0,0};
            other.chunk_renderer = nullptr;
        }
    };
    std::vector<RenderChunk> render_chunks_;

    // Settings
    bool enable_validation_ = false;

#ifdef WF_HAVE_VMA
    // Optional: Vulkan Memory Allocator
    struct VmaAllocator_T* vma_allocator_ = nullptr;
#endif

    // Camera
    float cam_yaw_ = 0.0f;   // radians
    float cam_pitch_ = 0.0f; // radians
    float cam_speed_ = 12.0f; // m/s
    float cam_sensitivity_ = 0.0025f; // radians per pixel
    double cam_pos_[3] = { 1165.0, 12.0, 0.0 }; // store camera position at double precision
    double last_time_ = 0.0;
    bool rmb_down_ = false;
    double last_cursor_x_ = 0.0, last_cursor_y_ = 0.0;
    bool mouse_captured_ = false; // true when cursor is disabled for mouse-look

    bool invert_mouse_x_ = true; // default: inverted horizontal look (swap left/right)
    bool invert_mouse_y_ = true;
    bool key_prev_toggle_x_ = false;
    bool key_prev_toggle_y_ = false;
    bool walk_mode_ = false;
    bool key_prev_toggle_walk_ = false;
    float eye_height_m_ = 1.7f; // camera height above terrain when walking
    float walk_speed_ = 6.0f;   // m/s on ground
    float walk_pitch_max_deg_ = 60.0f; // clamp for walk pitch
    float walk_surface_bias_m_ = 1.0f; // extra offset above computed surface to avoid clipping
    float surface_push_m_ = 0.0f;      // optional outward push for near-horizontal faces (mesh visual alignment)

    // Planet configuration (defaults can be overridden via config file/env)
    PlanetConfig planet_cfg_{};

    // HUD / stats
    double hud_accum_ = 0.0;
    float fps_smooth_ = 0.0f;
    float hud_scale_ = 2.0f;
    bool hud_shadow_enabled_ = false;
    float hud_shadow_offset_px_ = 1.5f;
    bool log_stream_ = false;
    bool log_pool_ = false;
    int pool_vtx_mb_ = 256;
    int pool_idx_mb_ = 128;
    bool save_chunks_enabled_ = false; // skip disk saves by default for faster streaming
    std::string region_root_ = "regions";

    size_t overlay_draw_slot_ = 0;
    OverlayRenderer overlay_;
    bool overlay_initialized_ = false;
    ui::UIContext hud_ui_context_;
    ui::UIBackend hud_ui_backend_;
    std::uint64_t hud_ui_frame_index_ = 0;
    ChunkRenderer chunk_renderer_;
    bool chunk_renderer_initialized_ = false;
    // Reused per-frame container to avoid allocations when building draw items
    std::vector<ChunkDrawItem> chunk_items_tmp_;

    // Parity toggle: use new ChunkRenderer path vs legacy pipeline
    bool use_chunk_renderer_ = true;

    // HUD text management (update at 0.25s cadence, rebuild per-slot on demand)
    std::string hud_text_;
    bool hud_force_refresh_ = true;

    // Rendering controls
    int ring_radius_ = 14;        // loads (2*ring_radius_+1)^2 chunks
    int prune_margin_ = 3;        // hysteresis: keep extra radius around load ring
    bool cull_enabled_ = true;    // CPU frustum culling toggle
    bool draw_stats_enabled_ = true; // show draw stats in HUD
    // Per-frame draw stats captured last frame
    int last_draw_total_ = 0;
    int last_draw_visible_ = 0;
    uint64_t last_draw_indices_ = 0;

    bool device_local_enabled_ = true; // default to device-local pools with staging
    bool debug_chunk_keys_ = false;
    bool debug_auto_aim_done_ = false;

    bool debug_show_axes_ = false;
    bool debug_show_test_triangle_ = false;

    struct DebugAxisVertex {
        float pos[3];
        float color[3];
    };
    wf::vk::UniqueBuffer debug_axes_vbo_;
    wf::vk::UniqueDeviceMemory debug_axes_vbo_mem_;
    uint32_t debug_axes_vertex_count_ = 0;
    wf::vk::UniquePipelineLayout debug_axes_layout_;
    wf::vk::UniquePipeline debug_axes_pipeline_;

    WorldStreamingSubsystem streaming_;
    bool edit_lmb_prev_down_ = false;
    bool edit_place_prev_down_ = false;
    uint16_t edit_place_material_ = MAT_DIRT;
    double edit_max_distance_m_ = 40.0;
    std::optional<VoxelHit> edit_last_empty_;
    std::optional<VoxelHit> edit_last_solid_;

    // Deferred GPU resource destruction to avoid device-lost
    std::vector<std::vector<RenderChunk>> trash_;

    // Input helpers
    void set_mouse_capture(bool capture);

    // Camera projection params (configurable)
    float fov_deg_ = 60.0f;
    float near_m_ = 0.1f;
    float far_m_  = 300.0f;
    // Streaming prioritization cone (degrees) around camera forward for meshing
    float stream_cone_deg_ = 75.0f;
    // Surface-band prefilter: pad in whole chunk shells to avoid misses
    int surface_band_pad_shells_ = 2; // intersects [minR-pad*chunk_thickness, maxR+pad*chunk_thickness]

    // Profiling/metrics
    int                 last_upload_count_ = 0;
    double              last_upload_ms_ = 0.0;
    double              upload_ms_avg_ = 0.0;
    bool                profile_csv_enabled_ = true;
    std::string         profile_csv_path_ = "profile.csv";
    bool                profile_header_written_ = false;
    std::chrono::steady_clock::time_point app_start_tp_{};
    std::mutex          profile_mutex_;
    void profile_append_csv(const std::string& line);
    void schedule_delete_chunk(RenderChunk&& rc);

    // Async loading/meshing
    int uploads_per_frame_limit_ = 16;
    int loader_threads_ = 0; // 0 = auto
    void drain_mesh_results();
    void prune_chunks_outside(int face, std::int64_t ci, std::int64_t cj, int span);
    // Multi-face/k pruning: keep any chunk that falls within any of the allowed rings and k-range
    struct AllowRegion { int face; std::int64_t ci; std::int64_t cj; std::int64_t ck; int span; int k_down; int k_up; };
    void prune_chunks_multi(const std::vector<AllowRegion>& allows);

    // Streaming state: current face and ring center
    float face_switch_hysteresis_ = 0.05f;
    float face_keep_time_cfg_s_ = 0.75f; // configurable hold time

    // Radial depth control (number of shells).
    int k_down_ = 3; // shells below center (toward planet center)
    int k_up_ = 3;   // shells above center (toward space)
    int k_prune_margin_ = 1; // hysteresis for k pruning

    // Config state
    std::string config_path_override_;
    std::string config_path_used_ = "wanderforge.cfg";
    std::unique_ptr<AppConfigManager> config_manager_;
    std::unique_ptr<WorldRuntime> world_runtime_;
    bool world_runtime_initialized_ = false;
    bool config_auto_reload_enabled_ = true;
    double config_watch_accum_ = 0.0;
    bool key_prev_reload_config_ = false;
    bool key_prev_save_config_ = false;
};

} // namespace wf
