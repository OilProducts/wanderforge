#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

struct GLFWwindow;

namespace wf {

class Renderer {
public:
    struct CreateInfo {
        GLFWwindow* window = nullptr;
        bool enable_validation = false;
    };

    struct FrameContext {
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        uint32_t image_index = 0;
        size_t frame_index = 0;
        bool acquired = false;
    };

    Renderer() = default;
    ~Renderer();

    void initialize(const CreateInfo& info);
    void shutdown();

    FrameContext begin_frame();
    void submit_frame(const FrameContext& ctx);
    void present_frame(const FrameContext& ctx);

    void recreate_swapchain();
    void wait_idle() const;

    VkDevice device() const { return device_; }
    VkPhysicalDevice physical_device() const { return physical_device_; }
    VkInstance instance() const { return instance_; }
    VkQueue graphics_queue() const { return queue_graphics_; }
    VkQueue present_queue() const { return queue_present_; }
    uint32_t graphics_queue_family() const { return queue_family_graphics_; }
    uint32_t present_queue_family() const { return queue_family_present_; }

    VkRenderPass render_pass() const { return render_pass_; }
    VkExtent2D swapchain_extent() const { return swapchain_extent_; }
    VkFormat swapchain_format() const { return swapchain_format_; }
    VkSwapchainKHR swapchain() const { return swapchain_; }

    VkCommandPool command_pool() const { return command_pool_; }
    VkCommandBuffer command_buffer(uint32_t image_index) const { return command_buffers_.at(image_index); }
    const std::vector<VkCommandBuffer>& command_buffers() const { return command_buffers_; }
    VkFramebuffer framebuffer(uint32_t image_index) const { return framebuffers_.at(image_index); }

    VkSemaphore image_available(size_t frame_index) const { return sem_image_available_.at(frame_index); }
    VkSemaphore render_finished(size_t frame_index) const { return sem_render_finished_.at(frame_index); }
    VkFence in_flight_fence(size_t frame_index) const { return fences_in_flight_.at(frame_index); }

    size_t frame_count() const { return kFramesInFlight; }
    size_t current_frame() const { return current_frame_; }
    void advance_frame();

    bool validation_enabled() const { return enable_validation_; }
    bool swapchain_needs_recreate() const { return swapchain_needs_recreate_; }
    void clear_swapchain_flag() { swapchain_needs_recreate_ = false; }

private:
    void create_instance();
    void setup_debug_messenger();
    void create_surface();
    void pick_physical_device();
    void create_logical_device();
    void create_swapchain_internal();
    void create_image_views();
    void create_render_pass();
    void create_depth_resources();
    void create_framebuffers();
    void create_command_pool_and_buffers();
    void create_sync_objects();

    void cleanup_swapchain();
    void destroy_sync_objects();

    VkFormat find_depth_format() const;

    static bool has_layer(const char* name);
    static bool has_instance_extension(const char* name);
    static bool has_device_extension(VkPhysicalDevice dev, const char* name);

private:
    static constexpr size_t kFramesInFlight = 2;

    GLFWwindow* window_ = nullptr;
    bool enable_validation_ = false;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queue_family_graphics_ = 0;
    uint32_t queue_family_present_ = 0;
    VkQueue queue_graphics_ = VK_NULL_HANDLE;
    VkQueue queue_present_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchain_extent_{};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;

    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkImage depth_image_ = VK_NULL_HANDLE;
    VkDeviceMemory depth_mem_ = VK_NULL_HANDLE;
    VkImageView depth_view_ = VK_NULL_HANDLE;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;

    std::vector<VkFramebuffer> framebuffers_;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers_;

    std::vector<VkSemaphore> sem_image_available_;
    std::vector<VkSemaphore> sem_render_finished_;
    std::vector<VkFence> fences_in_flight_;

    size_t current_frame_ = 0;
    bool swapchain_needs_recreate_ = false;

    std::vector<const char*> enabled_layers_;
    std::vector<const char*> enabled_instance_exts_;
    std::vector<const char*> enabled_device_exts_;
};

} // namespace wf
