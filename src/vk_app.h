#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <array>
#include "overlay.h"
#include "chunk_renderer.h"

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
        VkBuffer vbuf = VK_NULL_HANDLE;
        VkDeviceMemory vmem = VK_NULL_HANDLE;
        VkBuffer ibuf = VK_NULL_HANDLE;
        VkDeviceMemory imem = VK_NULL_HANDLE;
        uint32_t index_count = 0;
        float center[3] = {0,0,0};
        float radius = 0.0f;
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

    // HUD / stats
    double hud_accum_ = 0.0;
    float fps_smooth_ = 0.0f;

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
};

} // namespace wf
