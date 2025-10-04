#include "renderer.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <optional>
#include <set>
#include <vector>

#include "vk_utils.h"

namespace wf {
namespace {

void throw_if_failed(VkResult r, const char* msg) {
    if (r != VK_SUCCESS) {
        std::cerr << msg << " (VkResult=" << r << ")\n";
        std::abort();
    }
}

} // namespace

Renderer::~Renderer() {
    shutdown();
}

void Renderer::initialize(const CreateInfo& info) {
    if (device_) {
        shutdown();
    }

    window_ = info.window;
    enable_validation_ = info.enable_validation;
    swapchain_needs_recreate_ = false;
    current_frame_ = 0;

    create_instance();
    setup_debug_messenger();
    create_surface();
    pick_physical_device();
    create_logical_device();
    create_swapchain_internal();
    create_image_views();
    create_render_pass();
    create_depth_resources();
    create_framebuffers();
    create_command_pool_and_buffers();
    create_sync_objects();
}

void Renderer::shutdown() {
    if (!device_) {
        if (instance_ && debug_messenger_) {
            auto pfn_destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
            if (pfn_destroy) {
                pfn_destroy(instance_, debug_messenger_, nullptr);
            }
            debug_messenger_ = VK_NULL_HANDLE;
        }
        if (surface_) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (instance_) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
        return;
    }

    vkDeviceWaitIdle(device_);

    destroy_sync_objects();

    if (command_pool_) {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
        command_buffers_.clear();
    }

    cleanup_swapchain();

    if (device_) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

    if (debug_messenger_) {
        auto pfn_destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (pfn_destroy) {
            pfn_destroy(instance_, debug_messenger_, nullptr);
        }
        debug_messenger_ = VK_NULL_HANDLE;
    }

    if (surface_) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }

    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    physical_device_ = VK_NULL_HANDLE;
    queue_graphics_ = VK_NULL_HANDLE;
    queue_present_ = VK_NULL_HANDLE;
    enabled_layers_.clear();
    enabled_instance_exts_.clear();
    enabled_device_exts_.clear();
}

Renderer::FrameContext Renderer::begin_frame() {
    FrameContext ctx{};
    if (!device_) return ctx;

    const size_t frame_index = current_frame_;
    VkFence fence = in_flight_fence(frame_index);
    vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &fence);

    ctx.frame_index = frame_index;

    VkSemaphore image_available_sem = image_available(frame_index);
    uint32_t image_index = 0;
    VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_available_sem, VK_NULL_HANDLE, &image_index);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_needs_recreate_ = true;
        return ctx;
    }
    if (acq == VK_SUBOPTIMAL_KHR) {
        swapchain_needs_recreate_ = true;
    } else if (acq != VK_SUCCESS) {
        throw_if_failed(acq, "vkAcquireNextImageKHR failed");
    }

    ctx.image_index = image_index;
    ctx.command_buffer = command_buffers_.at(image_index);
    ctx.acquired = true;
    return ctx;
}

void Renderer::submit_frame(const FrameContext& ctx) {
    if (!ctx.acquired) return;
    VkSemaphore wait_semaphores[] = { image_available(ctx.frame_index) };
    VkPipelineStageFlags wait_stages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSemaphore signal_semaphores[] = { render_finished(ctx.frame_index) };

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = wait_semaphores;
    submit.pWaitDstStageMask = wait_stages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &ctx.command_buffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = signal_semaphores;

    throw_if_failed(vkQueueSubmit(queue_graphics_, 1, &submit, in_flight_fence(ctx.frame_index)), "vkQueueSubmit failed");
}

void Renderer::present_frame(const FrameContext& ctx) {
    if (!ctx.acquired) return;
    VkSemaphore signal_semaphores[] = { render_finished(ctx.frame_index) };

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = signal_semaphores;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &ctx.image_index;
    VkResult pres = vkQueuePresentKHR(queue_present_, &present);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
        swapchain_needs_recreate_ = true;
    } else if (pres != VK_SUCCESS) {
        throw_if_failed(pres, "vkQueuePresentKHR failed");
    }

    advance_frame();
}

void Renderer::advance_frame() {
    current_frame_ = (current_frame_ + 1) % kFramesInFlight;
}

void Renderer::wait_idle() const {
    if (device_) vkDeviceWaitIdle(device_);
}

void Renderer::recreate_swapchain() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);

    cleanup_swapchain();

    if (command_pool_) {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
        command_buffers_.clear();
    }

    create_swapchain_internal();
    create_image_views();
    create_render_pass();
    create_depth_resources();
    create_framebuffers();
    create_command_pool_and_buffers();
    swapchain_needs_recreate_ = false;
}

void Renderer::create_instance() {
    uint32_t glfw_count = 0;
    const char** glfw_ext = glfwGetRequiredInstanceExtensions(&glfw_count);
    enabled_instance_exts_.assign(glfw_ext, glfw_ext + glfw_count);

    if (enable_validation_) {
        enabled_layers_.push_back("VK_LAYER_KHRONOS_validation");
        if (!has_layer(enabled_layers_.back())) enabled_layers_.clear();
        if (has_instance_extension("VK_EXT_debug_utils")) {
            enabled_instance_exts_.push_back("VK_EXT_debug_utils");
        }
    }

#ifdef __APPLE__
    if (has_instance_extension("VK_KHR_portability_enumeration")) {
        enabled_instance_exts_.push_back("VK_KHR_portability_enumeration");
    }
#endif

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "wanderforge";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "wf";
    app.engineVersion = VK_MAKE_VERSION(0, 0, 1);
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(enabled_instance_exts_.size());
    ci.ppEnabledExtensionNames = enabled_instance_exts_.data();
    ci.enabledLayerCount = static_cast<uint32_t>(enabled_layers_.size());
    ci.ppEnabledLayerNames = enabled_layers_.data();
#ifdef __APPLE__
    if (std::find(enabled_instance_exts_.begin(), enabled_instance_exts_.end(), "VK_KHR_portability_enumeration") != enabled_instance_exts_.end()) {
        ci.flags |= static_cast<VkInstanceCreateFlags>(0x00000001); // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
    }
#endif
    throw_if_failed(vkCreateInstance(&ci, nullptr, &instance_), "vkCreateInstance failed");
}

void Renderer::setup_debug_messenger() {
    if (!enable_validation_) return;
    if (!has_instance_extension("VK_EXT_debug_utils")) return;

    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                              VkDebugUtilsMessageTypeFlagsEXT type,
                              const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
                              void*) -> VkBool32 {
        (void)severity;
        (void)type;
        std::cerr << "[VK] " << callback_data->pMessage << "\n";
        return VK_FALSE;
    };
    auto pfn_create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    if (pfn_create) {
        throw_if_failed(pfn_create(instance_, &info, nullptr, &debug_messenger_), "CreateDebugUtilsMessenger failed");
    }
}

void Renderer::create_surface() {
    throw_if_failed(glfwCreateWindowSurface(instance_, window_, nullptr, &surface_), "glfwCreateWindowSurface failed");
}

void Renderer::pick_physical_device() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    auto score = [](VkPhysicalDevice device) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device, &props);
        int s = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) s += 1000;
        return s;
    };

    std::sort(devices.begin(), devices.end(), [&](VkPhysicalDevice a, VkPhysicalDevice b) {
        return score(a) > score(b);
    });

    for (auto device : devices) {
        bool ok = has_device_extension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
#ifdef __APPLE__
        ok = ok && has_device_extension(device, "VK_KHR_portability_subset");
#endif
        if (!ok) continue;

        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &qcount, qprops.data());

        std::optional<uint32_t> gfx;
        std::optional<uint32_t> present;
        for (uint32_t i = 0; i < qcount; ++i) {
            if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) gfx = i;
            VkBool32 sup = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &sup);
            if (sup) present = i;
        }

        if (gfx && present) {
            physical_device_ = device;
            queue_family_graphics_ = *gfx;
            queue_family_present_ = *present;
            break;
        }
    }

    if (physical_device_ == VK_NULL_HANDLE) {
        std::cerr << "No suitable GPU found\n";
        std::abort();
    }
}

void Renderer::create_logical_device() {
    std::set<uint32_t> unique_queues = { queue_family_graphics_, queue_family_present_ };
    float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    queue_infos.reserve(unique_queues.size());
    for (uint32_t idx : unique_queues) {
        VkDeviceQueueCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = idx;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        queue_infos.push_back(qi);
    }

    enabled_device_exts_.clear();
    enabled_device_exts_.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
#ifdef __APPLE__
    if (has_device_extension(physical_device_, "VK_KHR_portability_subset")) {
        enabled_device_exts_.push_back("VK_KHR_portability_subset");
    }
#endif

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
    dci.pQueueCreateInfos = queue_infos.data();
    dci.enabledExtensionCount = static_cast<uint32_t>(enabled_device_exts_.size());
    dci.ppEnabledExtensionNames = enabled_device_exts_.data();

    throw_if_failed(vkCreateDevice(physical_device_, &dci, nullptr, &device_), "vkCreateDevice failed");
    vkGetDeviceQueue(device_, queue_family_graphics_, 0, &queue_graphics_);
    vkGetDeviceQueue(device_, queue_family_present_, 0, &queue_present_);
}

void Renderer::create_swapchain_internal() {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);

    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count, formats.data());

    uint32_t pm_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &pm_count, nullptr);
    std::vector<VkPresentModeKHR> present_modes(pm_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &pm_count, present_modes.data());

    VkSurfaceFormatKHR chosen_fmt = formats[0];
    for (auto f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen_fmt = f;
            break;
        }
    }

    VkPresentModeKHR chosen_pm = VK_PRESENT_MODE_FIFO_KHR;
    for (auto m : present_modes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
            chosen_pm = m;
            break;
        }
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFF) {
        int width = 0;
        int height = 0;
        glfwGetWindowSize(window_, &width, &height);
        extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
    }

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface_;
    sci.minImageCount = image_count;
    sci.imageFormat = chosen_fmt.format;
    sci.imageColorSpace = chosen_fmt.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t queue_indices[] = { queue_family_graphics_, queue_family_present_ };
    if (queue_family_graphics_ != queue_family_present_) {
        sci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        sci.queueFamilyIndexCount = 2;
        sci.pQueueFamilyIndices = queue_indices;
    } else {
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = chosen_pm;
    sci.clipped = VK_TRUE;

    throw_if_failed(vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_), "vkCreateSwapchainKHR failed");

    uint32_t retrieved = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &retrieved, nullptr);
    swapchain_images_.resize(retrieved);
    vkGetSwapchainImagesKHR(device_, swapchain_, &retrieved, swapchain_images_.data());

    swapchain_format_ = chosen_fmt.format;
    swapchain_extent_ = extent;

    const char* pm_name = (chosen_pm == VK_PRESENT_MODE_MAILBOX_KHR) ? "MAILBOX" :
                          (chosen_pm == VK_PRESENT_MODE_IMMEDIATE_KHR) ? "IMMEDIATE" : "FIFO";
    std::cout << "Swapchain present mode: " << pm_name << "\n";
}

void Renderer::create_image_views() {
    swapchain_image_views_.resize(swapchain_images_.size());
    for (size_t i = 0; i < swapchain_images_.size(); ++i) {
        VkImageViewCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image = swapchain_images_[i];
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = swapchain_format_;
        ci.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
        ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ci.subresourceRange.baseMipLevel = 0;
        ci.subresourceRange.levelCount = 1;
        ci.subresourceRange.baseArrayLayer = 0;
        ci.subresourceRange.layerCount = 1;
        throw_if_failed(vkCreateImageView(device_, &ci, nullptr, &swapchain_image_views_[i]), "vkCreateImageView failed");
    }
}

void Renderer::create_render_pass() {
    VkAttachmentDescription color{};
    color.format = swapchain_format_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depth{};
    depth.format = find_depth_format();
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_format_ = depth.format;

    VkAttachmentReference color_ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth_ref{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color_ref;
    sub.pDepthStencilAttachment = &depth_ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> atts{ color, depth };
    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = static_cast<uint32_t>(atts.size());
    rpci.pAttachments = atts.data();
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;

    throw_if_failed(vkCreateRenderPass(device_, &rpci, nullptr, &render_pass_), "vkCreateRenderPass failed");
}

VkFormat Renderer::find_depth_format() const {
    VkFormat candidates[] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM };
    for (VkFormat f : candidates) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(physical_device_, f, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            return f;
        }
    }
    return VK_FORMAT_D32_SFLOAT;
}

void Renderer::create_depth_resources() {
    if (depth_image_) {
        vkDestroyImageView(device_, depth_view_, nullptr);
        vkDestroyImage(device_, depth_image_, nullptr);
        vkFreeMemory(device_, depth_mem_, nullptr);
        depth_image_ = VK_NULL_HANDLE;
        depth_view_ = VK_NULL_HANDLE;
        depth_mem_ = VK_NULL_HANDLE;
    }

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.extent = { swapchain_extent_.width, swapchain_extent_.height, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.format = depth_format_ ? depth_format_ : find_depth_format();
    depth_format_ = ici.format;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    throw_if_failed(vkCreateImage(device_, &ici, nullptr, &depth_image_), "vkCreateImage(depth) failed");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, depth_image_, &req);
    uint32_t mem_type = wf::vk::find_memory_type(physical_device_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mem_type;
    throw_if_failed(vkAllocateMemory(device_, &mai, nullptr, &depth_mem_), "vkAllocateMemory(depth) failed");
    throw_if_failed(vkBindImageMemory(device_, depth_image_, depth_mem_, 0), "vkBindImageMemory(depth) failed");

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = depth_image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = depth_format_;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vci.subresourceRange.baseMipLevel = 0;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.baseArrayLayer = 0;
    vci.subresourceRange.layerCount = 1;
    throw_if_failed(vkCreateImageView(device_, &vci, nullptr, &depth_view_), "vkCreateImageView(depth) failed");
}

void Renderer::create_framebuffers() {
    framebuffers_.resize(swapchain_image_views_.size());
    for (size_t i = 0; i < framebuffers_.size(); ++i) {
        VkImageView attachments[] = { swapchain_image_views_[i], depth_view_ };
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = render_pass_;
        fci.attachmentCount = 2;
        fci.pAttachments = attachments;
        fci.width = swapchain_extent_.width;
        fci.height = swapchain_extent_.height;
        fci.layers = 1;
        throw_if_failed(vkCreateFramebuffer(device_, &fci, nullptr, &framebuffers_[i]), "vkCreateFramebuffer failed");
    }
}

void Renderer::create_command_pool_and_buffers() {
    if (command_pool_) {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
        command_buffers_.clear();
    }

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family_graphics_;
    throw_if_failed(vkCreateCommandPool(device_, &pci, nullptr, &command_pool_), "vkCreateCommandPool failed");

    command_buffers_.resize(framebuffers_.size());
    if (!command_buffers_.empty()) {
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = command_pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = static_cast<uint32_t>(command_buffers_.size());
        throw_if_failed(vkAllocateCommandBuffers(device_, &ai, command_buffers_.data()), "vkAllocateCommandBuffers failed");
    }
}

void Renderer::create_sync_objects() {
    sem_image_available_.resize(kFramesInFlight);
    sem_render_finished_.resize(kFramesInFlight);
    fences_in_flight_.resize(kFramesInFlight);

    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < kFramesInFlight; ++i) {
        throw_if_failed(vkCreateSemaphore(device_, &sci, nullptr, &sem_image_available_[i]), "vkCreateSemaphore failed");
        throw_if_failed(vkCreateSemaphore(device_, &sci, nullptr, &sem_render_finished_[i]), "vkCreateSemaphore failed");
        throw_if_failed(vkCreateFence(device_, &fci, nullptr, &fences_in_flight_[i]), "vkCreateFence failed");
    }
}

void Renderer::destroy_sync_objects() {
    for (size_t i = 0; i < sem_image_available_.size(); ++i) {
        if (sem_image_available_[i]) {
            vkDestroySemaphore(device_, sem_image_available_[i], nullptr);
            sem_image_available_[i] = VK_NULL_HANDLE;
        }
        if (sem_render_finished_[i]) {
            vkDestroySemaphore(device_, sem_render_finished_[i], nullptr);
            sem_render_finished_[i] = VK_NULL_HANDLE;
        }
        if (fences_in_flight_[i]) {
            vkDestroyFence(device_, fences_in_flight_[i], nullptr);
            fences_in_flight_[i] = VK_NULL_HANDLE;
        }
    }
    sem_image_available_.clear();
    sem_render_finished_.clear();
    fences_in_flight_.clear();
}

void Renderer::cleanup_swapchain() {
    for (auto fb : framebuffers_) {
        vkDestroyFramebuffer(device_, fb, nullptr);
    }
    framebuffers_.clear();

    if (depth_view_) {
        vkDestroyImageView(device_, depth_view_, nullptr);
        depth_view_ = VK_NULL_HANDLE;
    }
    if (depth_image_) {
        vkDestroyImage(device_, depth_image_, nullptr);
        depth_image_ = VK_NULL_HANDLE;
    }
    if (depth_mem_) {
        vkFreeMemory(device_, depth_mem_, nullptr);
        depth_mem_ = VK_NULL_HANDLE;
    }

    if (render_pass_) {
        vkDestroyRenderPass(device_, render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
    }

    for (auto iv : swapchain_image_views_) {
        vkDestroyImageView(device_, iv, nullptr);
    }
    swapchain_image_views_.clear();

    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool Renderer::has_layer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> props(count);
    vkEnumerateInstanceLayerProperties(&count, props.data());
    for (const auto& p : props) {
        if (std::strcmp(p.layerName, name) == 0) return true;
    }
    return false;
}

bool Renderer::has_instance_extension(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());
    for (const auto& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

bool Renderer::has_device_extension(VkPhysicalDevice dev, const char* name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, props.data());
    for (const auto& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

} // namespace wf
