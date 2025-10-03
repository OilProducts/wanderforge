#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <optional>
#include "overlay.h"
#include "chunk_renderer.h"
#include "mesh.h"
#include "chunk_delta.h"
#include "chunk.h"
#include "planet.h"
#include "config_loader.h"
#include "ui/ui_context.h"
#include "ui/ui_backend.h"
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
    void create_instance();
    void setup_debug_messenger();
    void create_surface();
    void pick_physical_device();
    void create_logical_device();
    void create_swapchain();
    void create_image_views();
    void create_render_pass();
    void create_graphics_pipeline();
    // Legacy chunk pipeline removed; ChunkRenderer is authoritative
    void create_compute_pipeline();
    void create_framebuffers();
    void create_command_pool_and_buffers();
    void create_sync_objects();

    void record_command_buffer(VkCommandBuffer cmd, uint32_t imageIndex);
    void draw_frame();
    void update_input(float dt);
    void update_hud(float dt);
    void load_config();
    AppConfig snapshot_config() const;
    void apply_config(const AppConfig& cfg);
    void apply_resolution_option(std::size_t index);
    std::size_t find_resolution_index(int width, int height) const;
    int current_brush_dim() const;
    void overlay_chunk_delta(const FaceChunkKey& key, Chunk64& chunk);
    void generate_base_chunk(const FaceChunkKey& key, const Float3& right, const Float3& up, const Float3& forward, Chunk64& chunk);
    void normalize_chunk_delta_representation(ChunkDelta& delta);
    void flush_dirty_chunk_deltas();
    struct VoxelHit {
        FaceChunkKey key{};
        int x = 0;
        int y = 0;
        int z = 0;
        Int3 voxel{0,0,0};
        double world_pos[3] = {0.0, 0.0, 0.0};
        uint16_t material = MAT_AIR;
    };
    bool world_to_chunk_coords(const double pos[3], FaceChunkKey& key, int& lx, int& ly, int& lz, Int3& voxel_out) const;
    bool pick_voxel(VoxelHit& solid_hit, VoxelHit& empty_before);
    bool apply_voxel_edit(const VoxelHit& target, uint16_t new_material, int brush_dim = 1);
    void queue_chunk_remesh(const FaceChunkKey& key);
    void process_pending_remeshes();

    void cleanup_swapchain();
    void recreate_swapchain();
    VkShaderModule load_shader_module(const std::string& path);
    void create_debug_axes_buffer();
    void destroy_debug_axes_buffer();
    void create_debug_axes_pipeline();
    void destroy_debug_axes_pipeline();

private:
    // Window
    GLFWwindow* window_ = nullptr;
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

    // Vulkan core
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queue_family_graphics_ = 0;
    uint32_t queue_family_present_ = 0;
    VkQueue queue_graphics_ = VK_NULL_HANDLE;
    VkQueue queue_present_ = VK_NULL_HANDLE;

    // Swapchain
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchain_extent_{};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;

    // Render pass & framebuffers
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkImage depth_image_ = VK_NULL_HANDLE;
    VkDeviceMemory depth_mem_ = VK_NULL_HANDLE;
    VkImageView depth_view_ = VK_NULL_HANDLE;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_triangle_ = VK_NULL_HANDLE;
    // Legacy chunk pipeline removed

    std::vector<VkFramebuffer> framebuffers_;

    // Compute pipeline (no-op dispatch)
    VkPipelineLayout pipeline_layout_compute_ = VK_NULL_HANDLE;
    VkPipeline pipeline_compute_ = VK_NULL_HANDLE;
    bool compute_enabled_ = false; // gate no-op compute dispatch

    struct RenderChunk {
        // Legacy per-chunk buffers (unused in indirect path)
        VkBuffer vbuf = VK_NULL_HANDLE;
        VkDeviceMemory vmem = VK_NULL_HANDLE;
        VkBuffer ibuf = VK_NULL_HANDLE;
        VkDeviceMemory imem = VK_NULL_HANDLE;
        // Draw info
        uint32_t index_count = 0;
        uint32_t first_index = 0; // indirect path
        int32_t  base_vertex = 0; // indirect path
        uint32_t vertex_count = 0; // for pooled free-list reclamation
        float center[3] = {0,0,0};
        float radius = 0.0f;
        FaceChunkKey key{0,0,0,0};
    };
    std::vector<RenderChunk> render_chunks_;

    // Helpers for buffers
    uint32_t find_memory_type(uint32_t typeBits, VkMemoryPropertyFlags properties);
    void create_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf, VkDeviceMemory& mem, const void* data);
    VkFormat find_depth_format();
    void create_depth_resources();

    // Commands & sync
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers_;
    static constexpr int kFramesInFlight = 2;
    std::vector<VkSemaphore> sem_image_available_;
    std::vector<VkSemaphore> sem_render_finished_;
    std::vector<VkFence> fences_in_flight_;
    size_t current_frame_ = 0;

    // Settings
    bool enable_validation_ = false;
    std::vector<const char*> enabled_layers_;
    std::vector<const char*> enabled_instance_exts_;
    std::vector<const char*> enabled_device_exts_;

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
    ui::UIContext hud_ui_context_;
    ui::UIBackend hud_ui_backend_;
    std::uint64_t hud_ui_frame_index_ = 0;
    ChunkRenderer chunk_renderer_;
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
    VkBuffer debug_axes_vbo_ = VK_NULL_HANDLE;
    VkDeviceMemory debug_axes_vbo_mem_ = VK_NULL_HANDLE;
    uint32_t debug_axes_vertex_count_ = 0;
    VkPipelineLayout debug_axes_layout_ = VK_NULL_HANDLE;
    VkPipeline debug_axes_pipeline_ = VK_NULL_HANDLE;

    std::unordered_map<FaceChunkKey, ChunkDelta, FaceChunkKeyHash> chunk_deltas_;
    std::mutex chunk_delta_mutex_;
    std::unordered_map<FaceChunkKey, Chunk64, FaceChunkKeyHash> chunk_cache_;
    mutable std::mutex chunk_cache_mutex_;
    std::deque<FaceChunkKey> remesh_queue_;
    std::mutex remesh_mutex_;
    const size_t remesh_per_frame_cap_ = 4;
    bool edit_lmb_prev_down_ = false;
    bool edit_place_prev_down_ = false;
    uint16_t edit_place_material_ = MAT_DIRT;
    double edit_max_distance_m_ = 40.0;
    std::optional<VoxelHit> edit_last_empty_;
    std::optional<VoxelHit> edit_last_solid_;

    // Deferred GPU resource destruction to avoid device-lost
    std::array<std::vector<RenderChunk>, kFramesInFlight> trash_;

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
    std::atomic<double> loader_last_mesh_ms_{0.0};
    std::atomic<double> loader_last_total_ms_{0.0};
    std::atomic<int>    loader_last_meshed_{0};
    int                 last_upload_count_ = 0;
    double              last_upload_ms_ = 0.0;
    double              upload_ms_avg_ = 0.0;
    bool                profile_csv_enabled_ = true;
    std::string         profile_csv_path_ = "profile.csv";
    bool                profile_header_written_ = false;
    std::chrono::steady_clock::time_point app_start_tp_{};
    std::mutex          profile_mutex_;
    void profile_append_csv(const std::string& line);
    void schedule_delete_chunk(const RenderChunk& rc);

    // Async loading/meshing
    struct MeshResult {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        float center[3] = {0,0,0};
        float radius = 0.0f;
        FaceChunkKey key{0,0,0,0};
        uint64_t job_gen = 0;
        uint32_t first_index = 0;
        int32_t base_vertex = 0;
    };
    struct CachedNeighborChunks {
        std::optional<Chunk64> neg_x;
        std::optional<Chunk64> pos_x;
        std::optional<Chunk64> neg_y;
        std::optional<Chunk64> pos_y;
        std::optional<Chunk64> neg_z;
        std::optional<Chunk64> pos_z;
        const Chunk64* nx_ptr() const { return neg_x ? &*neg_x : nullptr; }
        const Chunk64* px_ptr() const { return pos_x ? &*pos_x : nullptr; }
        const Chunk64* ny_ptr() const { return neg_y ? &*neg_y : nullptr; }
        const Chunk64* py_ptr() const { return pos_y ? &*pos_y : nullptr; }
        const Chunk64* nz_ptr() const { return neg_z ? &*neg_z : nullptr; }
        const Chunk64* pz_ptr() const { return pos_z ? &*pos_z : nullptr; }
    };
    CachedNeighborChunks gather_cached_neighbors(const FaceChunkKey& key) const;
    bool build_chunk_mesh_result(const FaceChunkKey& key,
                                 const Chunk64& chunk,
                                 MeshResult& out) const;
    bool build_chunk_mesh_result(const FaceChunkKey& key,
                                         const Chunk64& chunk,
                                         const Chunk64* nx, const Chunk64* px,
                                         const Chunk64* ny, const Chunk64* py,
                                         const Chunk64* nz, const Chunk64* pz,
                                         MeshResult& out) const;
    std::thread loader_thread_;
    std::mutex loader_mutex_;
    std::condition_variable loader_cv_;
    bool loader_quit_ = false;
    bool loader_busy_ = false;
    std::deque<MeshResult> results_queue_;
    int uploads_per_frame_limit_ = 16;
    int loader_threads_ = 0; // 0 = auto
    std::atomic<double> loader_last_gen_ms_{0.0};
    std::atomic<int> loader_last_chunks_{0};

    // Persistent loader: request queue and helpers
    struct LoadRequest { int face; int ring_radius; std::int64_t ci; std::int64_t cj; std::int64_t ck; int k_down; int k_up; float fwd_s; float fwd_t; uint64_t gen; };
    std::deque<LoadRequest> request_queue_;
    std::atomic<uint64_t> request_gen_{0};

    void start_initial_ring_async();
    void start_loader_thread();
    void enqueue_ring_request(int face, int ring_radius, std::int64_t center_i, std::int64_t center_j, std::int64_t center_k, int k_down, int k_up, float fwd_s, float fwd_t);
    void loader_thread_main();
    void build_ring_job(int face, int ring_radius, std::int64_t center_i, std::int64_t center_j, std::int64_t center_k, int k_down, int k_up, float fwd_s, float fwd_t, uint64_t job_gen);
    void drain_mesh_results();
    void update_streaming();
    void prune_chunks_outside(int face, std::int64_t ci, std::int64_t cj, int span);
    // Multi-face/k pruning: keep any chunk that falls within any of the allowed rings and k-range
    struct AllowRegion { int face; std::int64_t ci; std::int64_t cj; std::int64_t ck; int span; int k_down; int k_up; };
    void prune_chunks_multi(const std::vector<AllowRegion>& allows);

    // Streaming state: current face and ring center
    int stream_face_ = 0;
    bool stream_face_ready_ = false;
    uint64_t pending_request_gen_ = 0;
    float face_switch_hysteresis_ = 0.05f;
    std::int64_t ring_center_i_ = 0;
    std::int64_t ring_center_j_ = 0;
    std::int64_t ring_center_k_ = 0;

    // Multi-face transition support: keep previous face briefly while new face loads
    int prev_face_ = -1;
    std::int64_t prev_center_i_ = 0;
    std::int64_t prev_center_j_ = 0;
    std::int64_t prev_center_k_ = 0;
    float face_keep_timer_s_ = 0.0f;    // countdown while preserving previous face
    float face_keep_time_cfg_s_ = 0.75f; // configurable hold time

    // Radial depth control (number of shells).
    int k_down_ = 3; // shells below center (toward planet center)
    int k_up_ = 3;   // shells above center (toward space)
    int k_prune_margin_ = 1; // hysteresis for k pruning

    // Config state
    std::string config_path_override_;
    std::string config_path_used_ = "wanderforge.cfg";
};

} // namespace wf
