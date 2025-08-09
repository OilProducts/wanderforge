#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <string>

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
    void create_framebuffers();
    void create_command_pool_and_buffers();
    void create_sync_objects();

    void record_command_buffer(VkCommandBuffer cmd, uint32_t imageIndex);
    void draw_frame();

    void cleanup_swapchain();
    void recreate_swapchain();

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
    std::vector<VkFramebuffer> framebuffers_;

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
};

} // namespace wf

