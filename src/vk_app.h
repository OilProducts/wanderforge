#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <array>
#include "overlay.h"
#include "chunk_renderer.h"
#include "mesh.h"
#include "planet.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <tuple>

struct GLFWwindow;

namespace wf {

class VulkanApp {
public:
    VulkanApp();
    ~VulkanApp();

    void run();

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

    void cleanup_swapchain();
    void recreate_swapchain();
    VkShaderModule load_shader_module(const std::string& path);

private:
    // Window
    GLFWwindow* window_ = nullptr;
    int width_ = 1280;
    int height_ = 720;

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
    float cam_pos_[3] = { 1165.0f, 12.0f, 0.0f }; // near face0 surface by default
    double last_time_ = 0.0;
    bool rmb_down_ = false;
    double last_cursor_x_ = 0.0, last_cursor_y_ = 0.0;

    bool invert_mouse_x_ = false;
    bool invert_mouse_y_ = false;
    bool key_prev_toggle_x_ = false;
    bool key_prev_toggle_y_ = false;
    bool walk_mode_ = false;
    bool key_prev_toggle_walk_ = false;
    float eye_height_m_ = 1.7f; // camera height above terrain when walking
    float walk_speed_ = 6.0f;   // m/s on ground
    float walk_heading_ = 0.0f; // radians, rotation around local up when in walk mode
    float walk_pitch_ = 0.0f;   // radians, local pitch relative to horizon (positive up)
    float walk_pitch_max_deg_ = 60.0f; // clamp for walk pitch

    // HUD / stats
    double hud_accum_ = 0.0;
    float fps_smooth_ = 0.0f;
    bool log_stream_ = false;
    bool log_pool_ = false;
    int pool_vtx_mb_ = 64;
    int pool_idx_mb_ = 64;

    size_t overlay_draw_slot_ = 0;
    OverlayRenderer overlay_;
    ChunkRenderer chunk_renderer_;
    // Reused per-frame container to avoid allocations when building draw items
    std::vector<ChunkDrawItem> chunk_items_tmp_;

    // Parity toggle: use new ChunkRenderer path vs legacy pipeline
    bool use_chunk_renderer_ = true;

    // HUD text management (update at 0.25s cadence, rebuild per-slot on demand)
    std::string hud_text_;
    std::string overlay_last_text_;
    bool hud_force_refresh_ = true;
    std::array<bool, kFramesInFlight> overlay_text_valid_{{false, false}};

    // Rendering controls
    int ring_radius_ = 4;         // loads (2*ring_radius_+1)^2 chunks
    int prune_margin_ = 2;        // hysteresis: keep extra radius around load ring
    bool cull_enabled_ = true;    // CPU frustum culling toggle
    bool draw_stats_enabled_ = true; // show draw stats in HUD
    // Per-frame draw stats captured last frame
    int last_draw_total_ = 0;
    int last_draw_visible_ = 0;
    uint64_t last_draw_indices_ = 0;

    bool device_local_enabled_ = true; // default to device-local pools with staging

    // Deferred GPU resource destruction to avoid device-lost
    std::array<std::vector<RenderChunk>, kFramesInFlight> trash_;
    void schedule_delete_chunk(const RenderChunk& rc);

    // Async loading/meshing
    struct MeshResult {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        float center[3] = {0,0,0};
        float radius = 0.0f;
        FaceChunkKey key{0,0,0,0};
    };
    std::thread loader_thread_;
    std::mutex loader_mutex_;
    std::condition_variable loader_cv_;
    bool loader_quit_ = false;
    bool loader_busy_ = false;
    std::deque<MeshResult> results_queue_;
    int uploads_per_frame_limit_ = 1024;
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
    int k_up_ = 1;   // shells above center (toward space)
    int k_prune_margin_ = 1; // hysteresis for k pruning
};

} // namespace wf
