#include "vk_app.h"

#include <GLFW/glfw3.h>
#ifdef WF_HAVE_VMA
#include <vk_mem_alloc.h>
#endif
#include <cassert>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <cstdio>
#include <optional>
#include <set>
#include <vector>
#include <fstream>
#include <sstream>
#include <cmath>
#include <chrono>

#include "chunk.h"
#include "mesh.h"
#include "planet.h"
#include "region_io.h"
#include "vk_utils.h"
#include "camera.h"
#include "planet.h"

namespace wf {

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* /*pUserData*/) {
    std::cerr << "[VK] " << pCallbackData->pMessage << "\n";
    return VK_FALSE;
}


static void throw_if_failed(VkResult r, const char* msg) {
    if (r != VK_SUCCESS) {
        std::cerr << msg << " (VkResult=" << r << ")\n";
        std::abort();
    }
}

static bool has_layer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> props(count);
    vkEnumerateInstanceLayerProperties(&count, props.data());
    for (auto& p : props) if (std::strcmp(p.layerName, name) == 0) return true;
    return false;
}

static bool has_instance_extension(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());
    for (auto& p : props) if (std::strcmp(p.extensionName, name) == 0) return true;
    return false;
}

static bool has_device_extension(VkPhysicalDevice dev, const char* name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, props.data());
    for (auto& p : props) if (std::strcmp(p.extensionName, name) == 0) return true;
    return false;
}

VulkanApp::VulkanApp() {
    enable_validation_ = true; // toggled by build type in future
}

VulkanApp::~VulkanApp() {
    // Stop loader thread if running
    {
        std::lock_guard<std::mutex> lk(loader_mutex_);
        loader_quit_ = true;
    }
    loader_cv_.notify_all();
    if (loader_thread_.joinable()) loader_thread_.join();
    vkDeviceWaitIdle(device_);

    cleanup_swapchain();

    // Flush any deferred deletions
    for (auto& bin : trash_) {
        for (auto& rc : bin) {
            if (rc.vbuf) vkDestroyBuffer(device_, rc.vbuf, nullptr);
            if (rc.vmem) vkFreeMemory(device_, rc.vmem, nullptr);
            if (rc.ibuf) vkDestroyBuffer(device_, rc.ibuf, nullptr);
            if (rc.imem) vkFreeMemory(device_, rc.imem, nullptr);
        }
        bin.clear();
    }

    for (auto& rc : render_chunks_) {
        if (rc.vbuf) vkDestroyBuffer(device_, rc.vbuf, nullptr);
        if (rc.vmem) vkFreeMemory(device_, rc.vmem, nullptr);
        if (rc.ibuf) vkDestroyBuffer(device_, rc.ibuf, nullptr);
        if (rc.imem) vkFreeMemory(device_, rc.imem, nullptr);
    }

    // Overlay resources
    overlay_.cleanup(device_);
    // Chunk renderer
    chunk_renderer_.cleanup(device_);

    if (pipeline_compute_) { vkDestroyPipeline(device_, pipeline_compute_, nullptr); pipeline_compute_ = VK_NULL_HANDLE; }
    if (pipeline_layout_compute_) { vkDestroyPipelineLayout(device_, pipeline_layout_compute_, nullptr); pipeline_layout_compute_ = VK_NULL_HANDLE; }

#ifdef WF_HAVE_VMA
    if (vma_allocator_) {
        vmaDestroyAllocator(vma_allocator_);
        vma_allocator_ = nullptr;
    }
#endif

    for (auto f : fences_in_flight_) vkDestroyFence(device_, f, nullptr);
    for (auto s : sem_render_finished_) vkDestroySemaphore(device_, s, nullptr);
    for (auto s : sem_image_available_) vkDestroySemaphore(device_, s, nullptr);

    if (command_pool_) vkDestroyCommandPool(device_, command_pool_, nullptr);
    if (device_) vkDestroyDevice(device_, nullptr);

    if (debug_messenger_) {
        auto pfn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance_, "vkDestroyDebugUtilsMessengerEXT");
        if (pfn) pfn(instance_, debug_messenger_, nullptr);
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);

    if (window_) {
        glfwDestroyWindow(window_);
        glfwTerminate();
    }
}

void VulkanApp::run() {
    init_window();
    load_config();
    init_vulkan();
    app_start_tp_ = std::chrono::steady_clock::now();
    last_time_ = glfwGetTime();
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        double now = glfwGetTime();
        float dt = (float)std::max(0.0, now - last_time_);
        last_time_ = now;
        update_input(dt);
        update_hud(dt);
        draw_frame();
    }
}

void VulkanApp::init_window() {
    if (!glfwInit()) { std::abort(); }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window_ = glfwCreateWindow(width_, height_, "Wanderforge", nullptr, nullptr);
}

void VulkanApp::set_mouse_capture(bool capture) {
    if (!window_) return;
    if (mouse_captured_ == capture) return;
    if (capture) {
        glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
#if GLFW_VERSION_MAJOR >= 3
        if (glfwRawMouseMotionSupported()) {
            glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        }
#endif
    } else {
#if GLFW_VERSION_MAJOR >= 3
        if (glfwRawMouseMotionSupported()) {
            glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
        }
#endif
        glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
    mouse_captured_ = capture;
}

void VulkanApp::init_vulkan() {
    create_instance();
    setup_debug_messenger();
    create_surface();
    pick_physical_device();
    create_logical_device();
    create_compute_pipeline();
#ifdef WF_HAVE_VMA
    // Create optional VMA allocator
    {
        VmaAllocatorCreateInfo aci{};
        aci.instance = instance_;
        aci.physicalDevice = physical_device_;
        aci.device = device_;
        aci.vulkanApiVersion = VK_API_VERSION_1_0;
        if (vmaCreateAllocator(&aci, &vma_allocator_) != VK_SUCCESS) {
            std::cerr << "Warning: VMA allocator creation failed; continuing without VMA.\n";
            vma_allocator_ = nullptr;
        }
    }
#endif
    create_swapchain();
    create_image_views();
    create_render_pass();
    create_depth_resources();
    create_graphics_pipeline();
#include "wf_config.h"
    overlay_.init(physical_device_, device_, render_pass_, swapchain_extent_, WF_SHADER_DIR);
    chunk_renderer_.init(physical_device_, device_, render_pass_, swapchain_extent_, WF_SHADER_DIR);
    // Device-local pools + staging: configurable; default enabled
    chunk_renderer_.set_device_local(device_local_enabled_);
    chunk_renderer_.set_transfer_context(queue_family_graphics_, queue_graphics_);
    // Apply pool caps and logging preferences
    chunk_renderer_.set_pool_caps_bytes((VkDeviceSize)pool_vtx_mb_ * 1024ull * 1024ull,
                                        (VkDeviceSize)pool_idx_mb_ * 1024ull * 1024ull);
    chunk_renderer_.set_logging(log_pool_);
    std::cout << "ChunkRenderer ready: " << (chunk_renderer_.is_ready() ? "yes" : "no")
              << ", use_chunk_renderer=" << (use_chunk_renderer_ ? 1 : 0) << "\n";
    overlay_text_valid_.fill(false);
    hud_force_refresh_ = true;
    create_framebuffers();
    create_command_pool_and_buffers();
    create_sync_objects();

    // Print basic GPU and queue info once
    VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(physical_device_, &props);
    std::cout << "GPU: " << props.deviceName << " API "
              << VK_API_VERSION_MAJOR(props.apiVersion) << '.'
              << VK_API_VERSION_MINOR(props.apiVersion) << '.'
              << VK_API_VERSION_PATCH(props.apiVersion) << "\n";
    std::cout << "Queues: graphics=" << queue_family_graphics_ << ", present=" << queue_family_present_ << "\n";

    // Place camera slightly outside the loaded shell, looking inward (matches previous behavior)
    {
        const PlanetConfig& cfg = planet_cfg_;
        const int N = Chunk64::N;
        const double chunk_m = (double)N * cfg.voxel_size_m;
        const std::int64_t k0 = (std::int64_t)std::floor(cfg.radius_m / chunk_m);
        Float3 right, up, forward; face_basis(0, right, up, forward);
        float R0f = (float)(k0 * chunk_m);
        Float3 pos = forward * (R0f + 10.0f) + up * 4.0f; // 10m radially outward, 4m up
        cam_pos_[0] = pos.x; cam_pos_[1] = pos.y; cam_pos_[2] = pos.z;
        cam_yaw_ = 3.14159265f;   // look toward -forward (inward)
        cam_pitch_ = -0.05f;
    }
    // Start persistent loader and enqueue initial ring based on current camera
    start_initial_ring_async();
}
void VulkanApp::create_instance() {
    // Query GLFW extensions
    uint32_t glfwCount = 0; const char** glfwExt = glfwGetRequiredInstanceExtensions(&glfwCount);
    enabled_instance_exts_.assign(glfwExt, glfwExt + glfwCount);

    if (enable_validation_) {
        enabled_layers_.push_back("VK_LAYER_KHRONOS_validation");
        if (!has_layer(enabled_layers_.back())) enabled_layers_.clear();
        if (has_instance_extension("VK_EXT_debug_utils"))
            enabled_instance_exts_.push_back("VK_EXT_debug_utils");
    }

#ifdef __APPLE__
    if (has_instance_extension("VK_KHR_portability_enumeration")) {
        enabled_instance_exts_.push_back("VK_KHR_portability_enumeration");
    }
#endif

    VkApplicationInfo app{}; app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "wanderforge"; app.applicationVersion = VK_MAKE_VERSION(0,1,0);
    app.pEngineName = "wf"; app.engineVersion = VK_MAKE_VERSION(0,0,1);
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (uint32_t)enabled_instance_exts_.size();
    ci.ppEnabledExtensionNames = enabled_instance_exts_.data();
    ci.enabledLayerCount = (uint32_t)enabled_layers_.size();
    ci.ppEnabledLayerNames = enabled_layers_.data();
#ifdef __APPLE__
    ci.flags |= (VkInstanceCreateFlags)0x00000001; // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
#endif
    throw_if_failed(vkCreateInstance(&ci, nullptr, &instance_), "vkCreateInstance failed");
}

void VulkanApp::setup_debug_messenger() {
    if (!enable_validation_) return;
    if (!has_instance_extension("VK_EXT_debug_utils")) return;
    VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugCallback;
    auto pfn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
    if (pfn) throw_if_failed(pfn(instance_, &info, nullptr, &debug_messenger_), "CreateDebugUtilsMessenger failed");
}

void VulkanApp::create_surface() {
    throw_if_failed(glfwCreateWindowSurface(instance_, window_, nullptr, &surface_), "glfwCreateWindowSurface failed");
}

void VulkanApp::pick_physical_device() {
    uint32_t count = 0; vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devs(count); vkEnumeratePhysicalDevices(instance_, &count, devs.data());
    auto score = [&](VkPhysicalDevice d)->int{
        VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(d, &p);
        int s = 0; if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) s += 1000;
        return s;
    };
    std::sort(devs.begin(), devs.end(), [&](auto a, auto b){ return score(a) > score(b); });
    for (auto d : devs) {
        // Ensure swapchain and (on macOS) portability subset available
        bool ok = has_device_extension(d, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
#ifdef __APPLE__
        ok = ok && has_device_extension(d, "VK_KHR_portability_subset");
#endif
        if (!ok) continue;
        // Ensure surface support
        uint32_t qcount = 0; vkGetPhysicalDeviceQueueFamilyProperties(d, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qfp(qcount); vkGetPhysicalDeviceQueueFamilyProperties(d, &qcount, qfp.data());
        std::optional<uint32_t> gfx, present;
        for (uint32_t i=0;i<qcount;i++) {
            if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) gfx = i;
            VkBool32 sup = VK_FALSE; vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &sup);
            if (sup) present = i;
        }
        if (gfx && present) { physical_device_ = d; queue_family_graphics_ = *gfx; queue_family_present_ = *present; break; }
    }
    if (physical_device_ == VK_NULL_HANDLE) { std::cerr << "No suitable GPU found\n"; std::abort(); }
}

void VulkanApp::create_logical_device() {
    std::set<uint32_t> uniqueQ = {queue_family_graphics_, queue_family_present_};
    float prio = 1.0f; std::vector<VkDeviceQueueCreateInfo> qcis; qcis.reserve(uniqueQ.size());
    for (uint32_t qf : uniqueQ) {
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = qf; qci.queueCount = 1; qci.pQueuePriorities = &prio; qcis.push_back(qci);
    }
    enabled_device_exts_.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
#ifdef __APPLE__
    if (has_device_extension(physical_device_, "VK_KHR_portability_subset"))
        enabled_device_exts_.push_back("VK_KHR_portability_subset");
#endif

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = (uint32_t)qcis.size(); dci.pQueueCreateInfos = qcis.data();
    dci.enabledExtensionCount = (uint32_t)enabled_device_exts_.size();
    dci.ppEnabledExtensionNames = enabled_device_exts_.data();
    throw_if_failed(vkCreateDevice(physical_device_, &dci, nullptr, &device_), "vkCreateDevice failed");
    vkGetDeviceQueue(device_, queue_family_graphics_, 0, &queue_graphics_);
    vkGetDeviceQueue(device_, queue_family_present_, 0, &queue_present_);
}

void VulkanApp::create_swapchain() {
    // Capabilities
    VkSurfaceCapabilitiesKHR caps{}; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);
    uint32_t fmtCount=0; vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount); vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmtCount, fmts.data());
    uint32_t pmCount=0; vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &pmCount, nullptr);
    std::vector<VkPresentModeKHR> pms(pmCount); vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &pmCount, pms.data());

    VkSurfaceFormatKHR chosenFmt = fmts[0];
    for (auto f : fmts) if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosenFmt = f; break; }
    VkPresentModeKHR chosenPm = VK_PRESENT_MODE_FIFO_KHR; // guaranteed
    for (auto m : pms) if (m == VK_PRESENT_MODE_MAILBOX_KHR) { chosenPm = m; break; }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFF) { extent = { (uint32_t)width_, (uint32_t)height_ }; }
    uint32_t imageCount = caps.minImageCount + 1; if (caps.maxImageCount && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = imageCount;
    sci.imageFormat = chosenFmt.format; sci.imageColorSpace = chosenFmt.colorSpace;
    sci.imageExtent = extent; sci.imageArrayLayers = 1; sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    uint32_t qidx[2] = {queue_family_graphics_, queue_family_present_};
    if (queue_family_graphics_ != queue_family_present_) {
        sci.imageSharingMode = VK_SHARING_MODE_CONCURRENT; sci.queueFamilyIndexCount = 2; sci.pQueueFamilyIndices = qidx;
    } else { sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; }
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = chosenPm;
    sci.clipped = VK_TRUE;
    throw_if_failed(vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_), "vkCreateSwapchainKHR failed");
    const char* pmName = (chosenPm==VK_PRESENT_MODE_IMMEDIATE_KHR)?"IMMEDIATE":(chosenPm==VK_PRESENT_MODE_MAILBOX_KHR)?"MAILBOX":"FIFO";
    std::cout << "Swapchain present mode: " << pmName << "\n";

    uint32_t count=0; vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    swapchain_images_.resize(count); vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapchain_images_.data());
    swapchain_format_ = chosenFmt.format; swapchain_extent_ = extent;
}

void VulkanApp::create_image_views() {
    swapchain_image_views_.resize(swapchain_images_.size());
    for (size_t i=0;i<swapchain_images_.size();++i) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = swapchain_images_[i];
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D; ci.format = swapchain_format_;
        ci.components = {VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY};
        ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ci.subresourceRange.baseMipLevel=0; ci.subresourceRange.levelCount=1;
        ci.subresourceRange.baseArrayLayer=0; ci.subresourceRange.layerCount=1;
        throw_if_failed(vkCreateImageView(device_, &ci, nullptr, &swapchain_image_views_[i]), "vkCreateImageView failed");
    }
}

void VulkanApp::create_render_pass() {
    VkAttachmentDescription color{};
    color.format = swapchain_format_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    // Depth attachment
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
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &colorRef;
    sub.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkAttachmentDescription atts[2] = { color, depth };
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 2; rpci.pAttachments = atts;
    rpci.subpassCount = 1; rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1; rpci.pDependencies = &dep;
    throw_if_failed(vkCreateRenderPass(device_, &rpci, nullptr, &render_pass_), "vkCreateRenderPass failed");
}

void VulkanApp::create_framebuffers() {
    framebuffers_.resize(swapchain_image_views_.size());
    for (size_t i=0;i<swapchain_image_views_.size();++i) {
        VkImageView attachments[] = { swapchain_image_views_[i], depth_view_ };
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = render_pass_;
        fci.attachmentCount = 2; fci.pAttachments = attachments;
        fci.width = swapchain_extent_.width; fci.height = swapchain_extent_.height; fci.layers = 1;
        throw_if_failed(vkCreateFramebuffer(device_, &fci, nullptr, &framebuffers_[i]), "vkCreateFramebuffer failed");
    }
}

VkFormat VulkanApp::find_depth_format() {
    VkFormat candidates[] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM };
    for (VkFormat f : candidates) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(physical_device_, f, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) return f;
    }
    return VK_FORMAT_D32_SFLOAT;
}

void VulkanApp::create_depth_resources() {
    if (depth_image_) {
        vkDestroyImageView(device_, depth_view_, nullptr); depth_view_ = VK_NULL_HANDLE;
        vkDestroyImage(device_, depth_image_, nullptr); depth_image_ = VK_NULL_HANDLE;
        vkFreeMemory(device_, depth_mem_, nullptr); depth_mem_ = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.extent = { swapchain_extent_.width, swapchain_extent_.height, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.format = depth_format_ ? depth_format_ : find_depth_format();
    depth_format_ = ici.format;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    throw_if_failed(vkCreateImage(device_, &ici, nullptr, &depth_image_), "vkCreateImage(depth) failed");
    VkMemoryRequirements req{}; vkGetImageMemoryRequirements(device_, depth_image_, &req);
    uint32_t mt = wf::vk::find_memory_type(physical_device_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
    throw_if_failed(vkAllocateMemory(device_, &mai, nullptr, &depth_mem_), "vkAllocateMemory(depth) failed");
    throw_if_failed(vkBindImageMemory(device_, depth_image_, depth_mem_, 0), "vkBindImageMemory(depth) failed");
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = depth_image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = depth_format_;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vci.subresourceRange.baseMipLevel = 0; vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.baseArrayLayer = 0; vci.subresourceRange.layerCount = 1;
    throw_if_failed(vkCreateImageView(device_, &vci, nullptr, &depth_view_), "vkCreateImageView(depth) failed");
}

void VulkanApp::create_command_pool_and_buffers() {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family_graphics_;
    throw_if_failed(vkCreateCommandPool(device_, &pci, nullptr, &command_pool_), "vkCreateCommandPool failed");

    command_buffers_.resize(framebuffers_.size());
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = command_pool_; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = (uint32_t)command_buffers_.size();
    throw_if_failed(vkAllocateCommandBuffers(device_, &ai, command_buffers_.data()), "vkAllocateCommandBuffers failed");
}

void VulkanApp::create_sync_objects() {
    sem_image_available_.resize(kFramesInFlight);
    sem_render_finished_.resize(kFramesInFlight);
    fences_in_flight_.resize(kFramesInFlight);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i=0;i<kFramesInFlight;i++) {
        throw_if_failed(vkCreateSemaphore(device_, &sci, nullptr, &sem_image_available_[i]), "vkCreateSemaphore failed");
        throw_if_failed(vkCreateSemaphore(device_, &sci, nullptr, &sem_render_finished_[i]), "vkCreateSemaphore failed");
        throw_if_failed(vkCreateFence(device_, &fci, nullptr, &fences_in_flight_[i]), "vkCreateFence failed");
    }
}

void VulkanApp::record_command_buffer(VkCommandBuffer cmd, uint32_t imageIndex) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    throw_if_failed(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer failed");

    // No-op compute dispatch before rendering
    if (pipeline_compute_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_compute_);
        vkCmdDispatch(cmd, 1, 1, 1);
    }

    VkClearValue clear{ { {0.02f, 0.02f, 0.06f, 1.0f} } };
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = render_pass_;
    rbi.framebuffer = framebuffers_[imageIndex];
    rbi.renderArea.offset = {0,0}; rbi.renderArea.extent = swapchain_extent_;
    VkClearValue clears[2];
    clears[0] = clear;
    clears[1].depthStencil = {1.0f, 0};
    rbi.clearValueCount = 2; rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    if ((!render_chunks_.empty()) && chunk_renderer_.is_ready()) {
        // Row-major projection and view via camera helpers; multiply as V * P and pass directly
        float aspect = (float)swapchain_extent_.width / (float)swapchain_extent_.height;
        auto P = wf::perspective_row_major(fov_deg_, aspect, near_m_, far_m_);
        float eye[3] = { (float)cam_pos_[0], (float)cam_pos_[1], (float)cam_pos_[2] };
        wf::Mat4 V;
        if (!walk_mode_) {
            V = wf::view_row_major(cam_yaw_, cam_pitch_, eye);
        } else {
            // Build view from local basis (updir, heading)
            Float3 updir = wf::normalize(Float3{eye[0], eye[1], eye[2]});
            Float3 world_up{0,1,0};
            Float3 r0 = wf::normalize(Float3{ updir.y*world_up.z - updir.z*world_up.y,
                                              updir.z*world_up.x - updir.x*world_up.z,
                                              updir.x*world_up.y - updir.y*world_up.x });
            if (wf::length(r0) < 1e-5f) r0 = Float3{1,0,0};
            Float3 f0 = wf::normalize(Float3{ r0.y*updir.z - r0.z*updir.y,
                                              r0.z*updir.x - r0.x*updir.z,
                                              r0.x*updir.y - r0.y*updir.x });
            float ch = std::cos(walk_heading_), sh = std::sin(walk_heading_);
            Float3 fwd_h{ f0.x*ch + r0.x*sh, f0.y*ch + r0.y*sh, f0.z*ch + r0.z*sh };
            float cp = std::cos(walk_pitch_), sp = std::sin(walk_pitch_);
            Float3 fwd{ fwd_h.x*cp + updir.x*sp, fwd_h.y*cp + updir.y*sp, fwd_h.z*cp + updir.z*sp };
            Float3 up_rot{ updir.x*cp - fwd_h.x*sp, updir.y*cp - fwd_h.y*sp, updir.z*cp - fwd_h.z*sp };
            // Build right and recompute up via cross to ensure right-handed triad
            Float3 rightv{ fwd.y*up_rot.z - fwd.z*up_rot.y, fwd.z*up_rot.x - fwd.x*up_rot.z, fwd.x*up_rot.y - fwd.y*up_rot.x };
            // Recompute up_final = right x forward
            Float3 up_final{ rightv.y*fwd.z - rightv.z*fwd.y, rightv.z*fwd.x - rightv.x*fwd.z, rightv.x*fwd.y - rightv.y*fwd.x };
            // If the frame is rolled (terrain appears on top), flip right and up simultaneously
            if (up_final.y < 0.0f && std::fabs(updir.y) > 0.2f) {
                rightv = Float3{ -rightv.x, -rightv.y, -rightv.z };
                up_final = Float3{ -up_final.x, -up_final.y, -up_final.z };
            }
            // Row-major view matrix from basis
            V = wf::Mat4{
                rightv.x, up_final.x, -fwd.x, 0.0f,
                rightv.y, up_final.y, -fwd.y, 0.0f,
                rightv.z, up_final.z, -fwd.z, 0.0f,
                -(rightv.x*eye[0] + rightv.y*eye[1] + rightv.z*eye[2]),
                -(up_final.x*eye[0] + up_final.y*eye[1] + up_final.z*eye[2]),
                 ( fwd.x*eye[0] +  fwd.y*eye[1] +  fwd.z*eye[2]),
                 1.0f
            };
        }
        auto MVP = wf::mul_row_major(V, P);

        // Prepare preallocated container and compute draw stats
        chunk_items_tmp_.clear();
        chunk_items_tmp_.reserve(render_chunks_.size());
        last_draw_total_ = (int)render_chunks_.size();
        last_draw_visible_ = 0;
        last_draw_indices_ = 0;

        if (cull_enabled_) {
            // Frustum culling (simple sphere vs frustum)
            float fwd[3]; float upv[3]; float rightv[3];
            if (!walk_mode_) {
                float cyaw = std::cos(cam_yaw_), syaw = std::sin(cam_yaw_);
                float cp = std::cos(cam_pitch_), sp = std::sin(cam_pitch_);
                fwd[0] = cp*cyaw; fwd[1] = sp; fwd[2] = cp*syaw;
                upv[0] = 0.0f; upv[1] = 1.0f; upv[2] = 0.0f;
                rightv[0] = fwd[1]*upv[2]-fwd[2]*upv[1]; rightv[1] = fwd[2]*upv[0]-fwd[0]*upv[2]; rightv[2] = fwd[0]*upv[1]-fwd[1]*upv[0];
            } else {
                Float3 updir = wf::normalize(Float3{eye[0], eye[1], eye[2]});
                Float3 world_up{0,1,0};
                Float3 r0 = wf::normalize(Float3{ updir.y*world_up.z - updir.z*world_up.y,
                                                  updir.z*world_up.x - updir.x*world_up.z,
                                                  updir.x*world_up.y - updir.y*world_up.x });
                if (wf::length(r0) < 1e-5f) r0 = Float3{1,0,0};
                Float3 f0 = wf::normalize(Float3{ r0.y*updir.z - r0.z*updir.y,
                                                  r0.z*updir.x - r0.x*updir.z,
                                                  r0.x*updir.y - r0.y*updir.x });
                float ch = std::cos(walk_heading_), sh = std::sin(walk_heading_);
                Float3 fwh{ f0.x*ch + r0.x*sh, f0.y*ch + r0.y*sh, f0.z*ch + r0.z*sh };
                float cp2 = std::cos(walk_pitch_), sp2 = std::sin(walk_pitch_);
                Float3 fw{ fwh.x*cp2 + updir.x*sp2, fwh.y*cp2 + updir.y*sp2, fwh.z*cp2 + updir.z*sp2 };
                Float3 up2{ updir.x*cp2 - fwh.x*sp2, updir.y*cp2 - fwh.y*sp2, updir.z*cp2 - fwh.z*sp2 };
                // right = forward x up; up = right x forward
                Float3 rv{ fw.y*up2.z - fw.z*up2.y, fw.z*up2.x - fw.x*up2.z, fw.x*up2.y - fw.y*up2.x };
                Float3 upf{ rv.y*fw.z - rv.z*fw.y, rv.z*fw.x - rv.x*fw.z, rv.x*fw.y - rv.y*fw.x };
                // Flip both if inverted relative to world up to avoid upside-down view
                if (upf.y < 0.0f && std::fabs(updir.y) > 0.2f) {
                    rv = Float3{ -rv.x, -rv.y, -rv.z };
                    upf = Float3{ -upf.x, -upf.y, -upf.z };
                }
                fwd[0]=fw.x; fwd[1]=fw.y; fwd[2]=fw.z;
                upv[0]=upf.x; upv[1]=upf.y; upv[2]=upf.z;
                rightv[0]=rv.x; rightv[1]=rv.y; rightv[2]=rv.z;
            }
            float rl = std::sqrt(rightv[0]*rightv[0]+rightv[1]*rightv[1]+rightv[2]*rightv[2]);
            if (rl > 0) { rightv[0]/=rl; rightv[1]/=rl; rightv[2]/=rl; }
            // FOVs
            float fovy = 60.0f * 0.01745329252f;
            float tan_y = std::tan(fovy * 0.5f);
            float tan_x = tan_y * aspect;

            for (const auto& rc : render_chunks_) {
                float dx = rc.center[0] - eye[0];
                float dy = rc.center[1] - eye[1];
                float dz = rc.center[2] - eye[2];
                float dist_f = dx*fwd[0] + dy*fwd[1] + dz*fwd[2];
                float dist_r = dx*rightv[0] + dy*rightv[1] + dz*rightv[2];
                float dist_u = dx*upv[0] + dy*upv[1] + dz*upv[2];
                // near/far
                if (dist_f + rc.radius < near_m_) continue;
                if (dist_f - rc.radius > far_m_) continue;
                // side planes
                if (std::fabs(dist_r) > dist_f * tan_x + rc.radius) continue;
                if (std::fabs(dist_u) > dist_f * tan_y + rc.radius) continue;
                ChunkDrawItem item{};
                item.vbuf = rc.vbuf; item.ibuf = rc.ibuf; item.index_count = rc.index_count;
                item.first_index = rc.first_index; item.base_vertex = rc.base_vertex;
                chunk_items_tmp_.push_back(item);
                last_draw_visible_++;
                last_draw_indices_ += rc.index_count;
            }
        } else {
            for (const auto& rc : render_chunks_) {
                ChunkDrawItem item{};
                item.vbuf = rc.vbuf; item.ibuf = rc.ibuf; item.index_count = rc.index_count;
                item.first_index = rc.first_index; item.base_vertex = rc.base_vertex;
                chunk_items_tmp_.push_back(item);
                last_draw_visible_++;
                last_draw_indices_ += rc.index_count;
            }
        }
        chunk_renderer_.record(cmd, MVP.data(), chunk_items_tmp_);
    } else if (pipeline_triangle_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_triangle_);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    overlay_.record_draw(cmd, overlay_draw_slot_);
    vkCmdEndRenderPass(cmd);

    throw_if_failed(vkEndCommandBuffer(cmd), "vkEndCommandBuffer failed");
}

void VulkanApp::update_input(float dt) {
    // Close on Escape
    if (glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }

    // Mouse look when RMB is held
    int rmb = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT);
    double cx, cy; glfwGetCursorPos(window_, &cx, &cy);
    if (rmb == GLFW_PRESS) {
        if (!rmb_down_) {
            rmb_down_ = true;
            // Enable cursor-disabled/raw mode for consistent relative deltas
            set_mouse_capture(true);
            // Reset deltas to avoid an initial jump
            last_cursor_x_ = cx; last_cursor_y_ = cy;
        }
        double dx = cx - last_cursor_x_;
        double dy = cy - last_cursor_y_;
        last_cursor_x_ = cx; last_cursor_y_ = cy;
        float sx = invert_mouse_x_ ? -1.0f : 1.0f;
        float sy = invert_mouse_y_ ?  1.0f : -1.0f;
        if (!walk_mode_) {
            cam_yaw_   += sx * (float)(dx * cam_sensitivity_);
            cam_pitch_ += sy * (float)(dy * cam_sensitivity_);
            const float maxp = 1.55334306f; // ~89 deg
            if (cam_pitch_ > maxp) cam_pitch_ = maxp; if (cam_pitch_ < -maxp) cam_pitch_ = -maxp;
        } else {
            // Walk mode: heading around local up, and local pitch around horizon
            walk_heading_ += sx * (float)(dx * cam_sensitivity_);
            if (walk_heading_ > 3.14159265f) walk_heading_ -= 6.28318531f;
            if (walk_heading_ < -3.14159265f) walk_heading_ += 6.28318531f;
            float maxp = walk_pitch_max_deg_ * 0.01745329252f;
            walk_pitch_ += sy * (float)(dy * cam_sensitivity_);
            if (walk_pitch_ > maxp) walk_pitch_ = maxp; if (walk_pitch_ < -maxp) walk_pitch_ = -maxp;
        }
    } else {
        if (rmb_down_) {
            rmb_down_ = false;
            // Restore normal cursor when leaving mouse-look
            set_mouse_capture(false);
        }
    }

    // Compute basis from yaw/pitch
    float cyaw = std::cos(cam_yaw_), syaw = std::sin(cam_yaw_);
    float cp = std::cos(cam_pitch_), sp = std::sin(cam_pitch_);
    float fwd[3] = { cp*cyaw, sp, cp*syaw };
    float up[3]  = { 0.0f, 1.0f, 0.0f };
    // Right-handed basis: right = cross(fwd, up)
    float right[3] = { fwd[1]*up[2]-fwd[2]*up[1], fwd[2]*up[0]-fwd[0]*up[2], fwd[0]*up[1]-fwd[1]*up[0] };
    float rl = std::sqrt(right[0]*right[0]+right[1]*right[1]+right[2]*right[2]);
    if (rl > 0) { right[0]/=rl; right[1]/=rl; right[2]/=rl; }

    // Toggle walk mode with F key
    int kwalk = glfwGetKey(window_, GLFW_KEY_F);
    if (kwalk == GLFW_PRESS && !key_prev_toggle_walk_) {
        walk_mode_ = !walk_mode_;
        hud_force_refresh_ = true;
        if (walk_mode_) {
            // Initialize walk heading from current forward projected into tangent
            Float3 pos{cam_pos_[0], cam_pos_[1], cam_pos_[2]};
            Float3 updir = wf::normalize(pos);
            Float3 world_up{0,1,0};
            Float3 r0 = wf::normalize(Float3{ world_up.y*updir.z - world_up.z*updir.y,
                                              world_up.z*updir.x - world_up.x*updir.z,
                                              world_up.x*updir.y - world_up.y*updir.x });
            if (wf::length(r0) < 1e-5f) r0 = Float3{1,0,0};
            Float3 f0 = wf::normalize(Float3{ r0.y*updir.z - r0.z*updir.y,
                                              r0.z*updir.x - r0.x*updir.z,
                                              r0.x*updir.y - r0.y*updir.x });
            float cyaw = std::cos(cam_yaw_), syaw = std::sin(cam_yaw_);
            float cp = std::cos(cam_pitch_), sp = std::sin(cam_pitch_);
            Float3 fwdv{ cp*cyaw, sp, cp*syaw };
            float dotfu = fwdv.x*updir.x + fwdv.y*updir.y + fwdv.z*updir.z;
            Float3 fwd_t{ fwdv.x - updir.x*dotfu, fwdv.y - updir.y*dotfu, fwdv.z - updir.z*dotfu };
            fwd_t = wf::normalize(fwd_t);
            float x = fwd_t.x*f0.x + fwd_t.y*f0.y + fwd_t.z*f0.z;
            float y = fwd_t.x*r0.x + fwd_t.y*r0.y + fwd_t.z*r0.z;
            walk_heading_ = std::atan2(y, x);
            walk_pitch_ = 0.0f;
            // Snap to surface radius immediately on entering walk mode
            const PlanetConfig& cfg = planet_cfg_;
            double h = terrain_height_m(cfg, updir);
            double surface_r = cfg.radius_m + h;
            if (surface_r < cfg.sea_level_m) surface_r = cfg.sea_level_m;
            // Snap camera to the voxelized mesh surface: floor(surface_r/s)*s + s/2 aligns to top face
            double s_m = cfg.voxel_size_m;
            double mesh_r = std::floor(surface_r / s_m) * s_m + 0.5 * s_m;
            double target_r = mesh_r + (double)eye_height_m_ + (double)walk_surface_bias_m_;
            cam_pos_[0] = (double)updir.x * target_r;
            cam_pos_[1] = (double)updir.y * target_r;
            cam_pos_[2] = (double)updir.z * target_r;
        }
    }
    key_prev_toggle_walk_ = (kwalk == GLFW_PRESS);

    if (!walk_mode_) {
        float speed = cam_speed_ * dt * (glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 3.0f : 1.0f);
        if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS) { cam_pos_[0]+=fwd[0]*speed; cam_pos_[1]+=fwd[1]*speed; cam_pos_[2]+=fwd[2]*speed; }
        if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS) { cam_pos_[0]-=fwd[0]*speed; cam_pos_[1]-=fwd[1]*speed; cam_pos_[2]-=fwd[2]*speed; }
        if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS) { cam_pos_[0]-=right[0]*speed; cam_pos_[1]-=right[1]*speed; cam_pos_[2]-=right[2]*speed; }
        if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS) { cam_pos_[0]+=right[0]*speed; cam_pos_[1]+=right[1]*speed; cam_pos_[2]+=right[2]*speed; }
        if (glfwGetKey(window_, GLFW_KEY_Q) == GLFW_PRESS) { cam_pos_[1]-=speed; }
        if (glfwGetKey(window_, GLFW_KEY_E) == GLFW_PRESS) { cam_pos_[1]+=speed; }
    } else {
        // Walk mode: move along tangent plane using heading; keep camera at surface + eye_height
        const PlanetConfig& cfg = planet_cfg_;
        Float3 pos{(float)cam_pos_[0], (float)cam_pos_[1], (float)cam_pos_[2]};
        Float3 updir = normalize(pos);
        Float3 world_up{0,1,0};
        Float3 r0 = wf::normalize(Float3{ updir.y*world_up.z - updir.z*world_up.y,
                                          updir.z*world_up.x - updir.x*world_up.z,
                                          updir.x*world_up.y - updir.y*world_up.x });
        if (wf::length(r0) < 1e-5f) r0 = Float3{1,0,0};
        Float3 f0 = wf::normalize(Float3{ r0.y*updir.z - r0.z*updir.y,
                                          r0.z*updir.x - r0.x*updir.z,
                                          r0.x*updir.y - r0.y*updir.x });
        float ch = std::cos(walk_heading_), sh = std::sin(walk_heading_);
        Float3 fwd_t{ f0.x*ch + r0.x*sh, f0.y*ch + r0.y*sh, f0.z*ch + r0.z*sh };
        // Use right = forward x updir (RH), so +D moves to the screen-right
        Float3 right_t{ fwd_t.y*updir.z - fwd_t.z*updir.y, fwd_t.z*updir.x - fwd_t.x*updir.z, fwd_t.x*updir.y - fwd_t.y*updir.x };
        right_t = wf::normalize(right_t);
        float speed = walk_speed_ * dt * (glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 2.0f : 1.0f);
        // Build tangent step vector (meters) in the local tangent basis
        Float3 step{0,0,0};
        if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS) { step = step + fwd_t   * speed; }
        if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS) { step = step - fwd_t   * speed; }
        if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS) { step = step - right_t * speed; }
        if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS) { step = step + right_t * speed; }
        // Exact great-circle update: rotate updir toward the step direction by angle phi = |step| / radius
        Float3 ndir = updir;
        float step_len = wf::length(step);
        if (step_len > 0.0f) {
            Float3 tdir = step / step_len; // unit tangent direction
            double cam_rd = std::sqrt(cam_pos_[0]*cam_pos_[0] + cam_pos_[1]*cam_pos_[1] + cam_pos_[2]*cam_pos_[2]);
            float phi = (float)(step_len / std::max(cam_rd, 1e-9));
            float c = std::cos(phi), s = std::sin(phi);
            // Since tdir is tangent to updir, Rodrigues reduces to ndir = updir*c + tdir*s
            ndir = wf::normalize(Float3{ updir.x * c + tdir.x * s,
                                          updir.y * c + tdir.y * s,
                                          updir.z * c + tdir.z * s });
        }
        double h = terrain_height_m(cfg, ndir);
        double surface_r = cfg.radius_m + h;
        if (surface_r < cfg.sea_level_m) surface_r = cfg.sea_level_m; // keep above water surface
        double s_m = cfg.voxel_size_m;
        double mesh_r = std::floor(surface_r / s_m) * s_m + 0.5 * s_m;
        double target_r = mesh_r + (double)eye_height_m_ + (double)walk_surface_bias_m_;
        cam_pos_[0] = (double)ndir.x * target_r;
        cam_pos_[1] = (double)ndir.y * target_r;
        cam_pos_[2] = (double)ndir.z * target_r;
    }

    // Toggle invert via keys: X for invert X, Y for invert Y (edge-triggered)
    int kx = glfwGetKey(window_, GLFW_KEY_X);
    if (kx == GLFW_PRESS && !key_prev_toggle_x_) { invert_mouse_x_ = !invert_mouse_x_; std::cout << "invert_mouse_x=" << invert_mouse_x_ << "\n"; hud_force_refresh_ = true; }
    key_prev_toggle_x_ = (kx == GLFW_PRESS);
    int ky = glfwGetKey(window_, GLFW_KEY_Y);
    if (ky == GLFW_PRESS && !key_prev_toggle_y_) { invert_mouse_y_ = !invert_mouse_y_; std::cout << "invert_mouse_y=" << invert_mouse_y_ << "\n"; hud_force_refresh_ = true; }
    key_prev_toggle_y_ = (ky == GLFW_PRESS);
}

void VulkanApp::update_hud(float dt) {
    // Smooth FPS
    if (dt > 0.0001f && dt < 1.0f) {
        float fps = 1.0f / dt;
        if (fps_smooth_ <= 0.0f) fps_smooth_ = fps; else fps_smooth_ = fps_smooth_ * 0.9f + fps * 0.1f;
    }
    hud_accum_ += dt;

    bool do_refresh = hud_force_refresh_ || (hud_accum_ >= 0.25);
    if (!do_refresh) return;
    hud_force_refresh_ = false;
    hud_accum_ = 0.0;

    // Format both the window title and HUD overlay strings
    char title[256];
    float yaw_deg = cam_yaw_ * 57.2957795f;
    float pitch_deg = cam_pitch_ * 57.2957795f;
    std::snprintf(title, sizeof(title),
                  "Wanderforge | FPS: %.1f | Pos: (%.1f, %.1f, %.1f) | Yaw/Pitch: (%.1f, %.1f) | InvX:%d InvY:%d | Speed: %.1f",
                  fps_smooth_, cam_pos_[0], cam_pos_[1], cam_pos_[2], yaw_deg, pitch_deg,
                  invert_mouse_x_ ? 1 : 0, invert_mouse_y_ ? 1 : 0, cam_speed_);
    glfwSetWindowTitle(window_, title);

    char hud[768];
    if (draw_stats_enabled_) {
        float tris_m = (float)last_draw_indices_ / 3.0f / 1.0e6f;
        VkDeviceSize v_used=0,v_cap=0,i_used=0,i_cap=0;
        if (chunk_renderer_.is_ready()) chunk_renderer_.get_pool_usage(v_used, v_cap, i_used, i_cap);
        float v_used_mb = (float)v_used / (1024.0f*1024.0f);
        float v_cap_mb  = (float)(v_cap ? v_cap : (VkDeviceSize)1) / (1024.0f*1024.0f);
        float i_used_mb = (float)i_used / (1024.0f*1024.0f);
        float i_cap_mb  = (float)(i_cap ? i_cap : (VkDeviceSize)1) / (1024.0f*1024.0f);
        size_t qdepth = 0; {
            std::lock_guard<std::mutex> lk(loader_mutex_);
            qdepth = results_queue_.size();
        }
        double gen_ms = loader_last_gen_ms_.load();
        int gen_chunks = loader_last_chunks_.load();
        double ms_per = (gen_chunks > 0) ? (gen_ms / (double)gen_chunks) : 0.0;
        double mesh_ms = loader_last_mesh_ms_.load();
        int meshed = loader_last_meshed_.load();
        double mesh_ms_per = (meshed > 0) ? (mesh_ms / (double)meshed) : 0.0;
        double up_ms = last_upload_ms_;
        int up_count = last_upload_count_;
        // Ground-follow diagnostics: camera vs. target surface radius
        double cam_rd_hud = std::sqrt(cam_pos_[0]*cam_pos_[0] + cam_pos_[1]*cam_pos_[1] + cam_pos_[2]*cam_pos_[2]);
        const PlanetConfig& pcfg = planet_cfg_;
        Float3 ndir = wf::normalize(Float3{(float)cam_pos_[0], (float)cam_pos_[1], (float)cam_pos_[2]});
        double h_surf = terrain_height_m(pcfg, ndir);
        double ground_r = pcfg.radius_m + h_surf; if (ground_r < pcfg.sea_level_m) ground_r = pcfg.sea_level_m;
        double target_r = ground_r + (double)eye_height_m_ + (double)walk_surface_bias_m_;
        double dr = cam_rd_hud - target_r;
        std::snprintf(hud, sizeof(hud),
                      "FPS: %.1f\nPos:(%.1f,%.1f,%.1f)  Yaw/Pitch:(%.1f,%.1f)  InvX:%d InvY:%d  Speed:%.1f\nDraw:%d/%d  Tris:%.2fM  Cull:%s  Ring:%d  Face:%d ci:%lld cj:%lld ck:%lld  k:%d/%d  Hold:%.2fs\nQueue:%zu  Gen:%.0fms (%d ch, %.2f ms/ch)  Mesh:%.0fms (%d ch, %.2f ms/ch)  Upload:%d in %.1fms (avg %.1fms)\nRad: cam=%.1f  tgt=%.1f  d=%.2f  (eye=%.2f bias=%.2f)\nPoolV: %.1f/%.1f MB  PoolI: %.1f/%.1f MB  Loader:%s",
                       fps_smooth_,
                       cam_pos_[0], cam_pos_[1], cam_pos_[2], yaw_deg, pitch_deg,
                       invert_mouse_x_?1:0, invert_mouse_y_?1:0, cam_speed_,
                      last_draw_visible_, last_draw_total_, tris_m, cull_enabled_?"on":"off", ring_radius_,
                      stream_face_, (long long)ring_center_i_, (long long)ring_center_j_, (long long)ring_center_k_, k_down_, k_up_, (double)face_keep_timer_s_,
                      qdepth, gen_ms, gen_chunks, ms_per,
                      mesh_ms, meshed, mesh_ms_per,
                      up_count, up_ms, upload_ms_avg_,
                      (float)cam_rd_hud, (float)target_r, (float)dr, eye_height_m_, walk_surface_bias_m_,
                      v_used_mb, v_cap_mb, i_used_mb, i_cap_mb, loader_busy_?"busy":"idle");
    } else {
        std::snprintf(hud, sizeof(hud),
                      "FPS: %.1f\nPos:(%.1f,%.1f,%.1f)  Yaw/Pitch:(%.1f,%.1f)  InvX:%d InvY:%d  Speed:%.1f",
                      fps_smooth_, cam_pos_[0], cam_pos_[1], cam_pos_[2], yaw_deg, pitch_deg,
                      invert_mouse_x_?1:0, invert_mouse_y_?1:0, cam_speed_);
    }

    // Only update overlay text if it actually changed
    if (hud_text_ != hud) {
        hud_text_.assign(hud);
        // Mark all per-frame overlays as needing rebuild
        overlay_text_valid_.fill(false);
    }
}

static inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a, b - a);
}

static inline std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static inline bool parse_bool(std::string v, bool defv) {
    v = lower(trim(v));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return defv;
}

void VulkanApp::load_config() {
    // Environment overrides
    if (const char* s = std::getenv("WF_INVERT_MOUSE_X")) invert_mouse_x_ = parse_bool(s, invert_mouse_x_);
    if (const char* s = std::getenv("WF_INVERT_MOUSE_Y")) invert_mouse_y_ = parse_bool(s, invert_mouse_y_);
    if (const char* s = std::getenv("WF_MOUSE_SENSITIVITY")) { try { cam_sensitivity_ = std::stof(s); } catch(...){} }
    if (const char* s = std::getenv("WF_MOVE_SPEED")) { try { cam_speed_ = std::stof(s); } catch(...){} }
    if (const char* s = std::getenv("WF_FOV_DEG")) { try { fov_deg_ = std::stof(s); } catch(...){} }
    if (const char* s = std::getenv("WF_NEAR_M")) { try { near_m_ = std::stof(s); } catch(...){} }
    if (const char* s = std::getenv("WF_FAR_M"))  { try { far_m_  = std::stof(s); } catch(...){} }
    // Planet/terrain
    if (const char* s = std::getenv("WF_TERRAIN_AMP_M")) { try { planet_cfg_.terrain_amp_m = std::stod(s); } catch(...){} }
    if (const char* s = std::getenv("WF_TERRAIN_FREQ")) { try { planet_cfg_.terrain_freq = std::stof(s); } catch(...){} }
    if (const char* s = std::getenv("WF_TERRAIN_OCTAVES")) { try { planet_cfg_.terrain_octaves = std::max(1, std::stoi(s)); } catch(...){} }
    if (const char* s = std::getenv("WF_TERRAIN_LACUNARITY")) { try { planet_cfg_.terrain_lacunarity = std::stof(s); } catch(...){} }
    if (const char* s = std::getenv("WF_TERRAIN_GAIN")) { try { planet_cfg_.terrain_gain = std::stof(s); } catch(...){} }
    if (const char* s = std::getenv("WF_PLANET_SEED")) { try { planet_cfg_.seed = (uint32_t)std::stoul(s); } catch(...){} }
    if (const char* s = std::getenv("WF_RADIUS_M")) { try { planet_cfg_.radius_m = std::stod(s); } catch(...){} }
    if (const char* s = std::getenv("WF_SEA_LEVEL_M")) { try { planet_cfg_.sea_level_m = std::stod(s); } catch(...){} }
    if (const char* s = std::getenv("WF_VOXEL_SIZE_M")) { try { planet_cfg_.voxel_size_m = std::stod(s); } catch(...){} }
    if (const char* s = std::getenv("WF_WALK_MODE")) walk_mode_ = parse_bool(s, walk_mode_);
    if (const char* s = std::getenv("WF_EYE_HEIGHT")) { try { eye_height_m_ = std::stof(s); } catch(...){} }
    if (const char* s = std::getenv("WF_WALK_SPEED")) { try { walk_speed_ = std::stof(s); } catch(...){} }
    if (const char* s = std::getenv("WF_WALK_PITCH_MAX_DEG")) { try { walk_pitch_max_deg_ = std::stof(s); } catch(...){} }
    if (const char* s = std::getenv("WF_USE_CHUNK_RENDERER")) use_chunk_renderer_ = parse_bool(s, use_chunk_renderer_);
    if (const char* s = std::getenv("WF_RING_RADIUS")) { try { ring_radius_ = std::max(0, std::stoi(s)); } catch(...){} }
    if (const char* s = std::getenv("WF_PRUNE_MARGIN")) { try { prune_margin_ = std::max(0, std::stoi(s)); } catch(...){} }
    if (const char* s = std::getenv("WF_CULL")) cull_enabled_ = parse_bool(s, cull_enabled_);
    if (const char* s = std::getenv("WF_DRAW_STATS")) draw_stats_enabled_ = parse_bool(s, draw_stats_enabled_);
    if (const char* s = std::getenv("WF_LOG_STREAM")) log_stream_ = parse_bool(s, log_stream_);
    if (const char* s = std::getenv("WF_LOG_POOL")) log_pool_ = parse_bool(s, log_pool_);
    if (const char* s = std::getenv("WF_SAVE_CHUNKS")) save_chunks_enabled_ = parse_bool(s, save_chunks_enabled_);
    if (const char* s = std::getenv("WF_SURFACE_PUSH_M")) { try { surface_push_m_ = std::stof(s); } catch(...){} }
    if (const char* s = std::getenv("WF_PROFILE_CSV")) profile_csv_enabled_ = parse_bool(s, profile_csv_enabled_);
    if (const char* s = std::getenv("WF_PROFILE_CSV_PATH")) { try { profile_csv_path_ = s; } catch(...){} }
    if (const char* s = std::getenv("WF_DEVICE_LOCAL")) device_local_enabled_ = parse_bool(s, device_local_enabled_);
    if (const char* s = std::getenv("WF_POOL_VTX_MB")) { try { pool_vtx_mb_ = std::max(1, std::stoi(s)); } catch(...){} }
    if (const char* s = std::getenv("WF_POOL_IDX_MB")) { try { pool_idx_mb_ = std::max(1, std::stoi(s)); } catch(...){} }
    if (const char* s = std::getenv("WF_UPLOADS_PER_FRAME")) { try { uploads_per_frame_limit_ = std::max(1, std::stoi(s)); } catch(...){} }
    if (const char* s = std::getenv("WF_LOADER_THREADS")) { try { loader_threads_ = std::max(0, std::stoi(s)); } catch(...){} }
    if (const char* s = std::getenv("WF_K_DOWN")) { try { k_down_ = std::max(0, std::stoi(s)); } catch(...){} }
    if (const char* s = std::getenv("WF_K_UP")) { try { k_up_ = std::max(0, std::stoi(s)); } catch(...){} }
    if (const char* s = std::getenv("WF_K_PRUNE_MARGIN")) { try { k_prune_margin_ = std::max(0, std::stoi(s)); } catch(...){} }
    if (const char* s = std::getenv("WF_FACE_KEEP_SEC")) { try { face_keep_time_cfg_s_ = std::max(0.0f, std::stof(s)); } catch(...){} }

    std::ifstream in("wanderforge.cfg");
    if (!in.good()) return;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower(trim(line.substr(0, eq)));
        std::string val = trim(line.substr(eq + 1));
        if (key == "invert_mouse_x") invert_mouse_x_ = parse_bool(val, invert_mouse_x_);
        else if (key == "invert_mouse_y") invert_mouse_y_ = parse_bool(val, invert_mouse_y_);
        else if (key == "mouse_sensitivity") { try { cam_sensitivity_ = std::stof(val); } catch(...){} }
        else if (key == "move_speed") { try { cam_speed_ = std::stof(val); } catch(...){} }
        else if (key == "fov_deg") { try { fov_deg_ = std::stof(val); } catch(...){} }
        else if (key == "near_m") { try { near_m_ = std::stof(val); } catch(...){} }
        else if (key == "far_m")  { try { far_m_  = std::stof(val); } catch(...){} }
        else if (key == "walk_mode") walk_mode_ = parse_bool(val, walk_mode_);
        else if (key == "eye_height") { try { eye_height_m_ = std::stof(val); } catch(...){} }
        else if (key == "walk_speed") { try { walk_speed_ = std::stof(val); } catch(...){} }
        else if (key == "walk_pitch_max_deg") { try { walk_pitch_max_deg_ = std::stof(val); } catch(...){} }
        else if (key == "walk_surface_bias_m") { try { walk_surface_bias_m_ = std::stof(val); } catch(...){} }
        else if (key == "surface_push_m") { try { surface_push_m_ = std::stof(val); } catch(...){} }
        // Planet / terrain controls
        else if (key == "terrain_amp_m") { try { planet_cfg_.terrain_amp_m = std::stod(val); } catch(...){} }
        else if (key == "terrain_freq") { try { planet_cfg_.terrain_freq = std::stof(val); } catch(...){} }
        else if (key == "terrain_octaves") { try { planet_cfg_.terrain_octaves = std::max(1, std::stoi(val)); } catch(...){} }
        else if (key == "terrain_lacunarity") { try { planet_cfg_.terrain_lacunarity = std::stof(val); } catch(...){} }
        else if (key == "terrain_gain") { try { planet_cfg_.terrain_gain = std::stof(val); } catch(...){} }
        else if (key == "planet_seed") { try { planet_cfg_.seed = (uint32_t)std::stoul(val); } catch(...){} }
        else if (key == "radius_m") { try { planet_cfg_.radius_m = std::stod(val); } catch(...){} }
        else if (key == "sea_level_m") { try { planet_cfg_.sea_level_m = std::stod(val); } catch(...){} }
        else if (key == "voxel_size_m") { try { planet_cfg_.voxel_size_m = std::stod(val); } catch(...){} }
        else if (key == "use_chunk_renderer") use_chunk_renderer_ = parse_bool(val, use_chunk_renderer_);
        else if (key == "ring_radius") { try { ring_radius_ = std::max(0, std::stoi(val)); } catch(...){} }
        else if (key == "prune_margin") { try { prune_margin_ = std::max(0, std::stoi(val)); } catch(...){} }
        else if (key == "cull") cull_enabled_ = parse_bool(val, cull_enabled_);
        else if (key == "draw_stats") draw_stats_enabled_ = parse_bool(val, draw_stats_enabled_);
        else if (key == "log_stream") log_stream_ = parse_bool(val, log_stream_);
        else if (key == "log_pool") log_pool_ = parse_bool(val, log_pool_);
        else if (key == "save_chunks") save_chunks_enabled_ = parse_bool(val, save_chunks_enabled_);
        else if (key == "profile_csv") profile_csv_enabled_ = parse_bool(val, profile_csv_enabled_);
        else if (key == "profile_csv_path") { profile_csv_path_ = val; }
        else if (key == "device_local") device_local_enabled_ = parse_bool(val, device_local_enabled_);
        else if (key == "pool_vtx_mb") { try { pool_vtx_mb_ = std::max(1, std::stoi(val)); } catch(...){} }
        else if (key == "pool_idx_mb") { try { pool_idx_mb_ = std::max(1, std::stoi(val)); } catch(...){} }
        else if (key == "uploads_per_frame") { try { uploads_per_frame_limit_ = std::max(1, std::stoi(val)); } catch(...){} }
        else if (key == "loader_threads") { try { loader_threads_ = std::max(0, std::stoi(val)); } catch(...){} }
        else if (key == "k_down") { try { k_down_ = std::max(0, std::stoi(val)); } catch(...){} }
        else if (key == "k_up") { try { k_up_ = std::max(0, std::stoi(val)); } catch(...){} }
        else if (key == "k_prune_margin") { try { k_prune_margin_ = std::max(0, std::stoi(val)); } catch(...){} }
        else if (key == "face_keep_sec") { try { face_keep_time_cfg_s_ = std::max(0.0f, std::stof(val)); } catch(...){} }
    }
}

void VulkanApp::draw_frame() {
    // Wait for this frame slot to be free
    vkWaitForFences(device_, 1, &fences_in_flight_[current_frame_], VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &fences_in_flight_[current_frame_]);
    // Safe point: destroy any resources deferred for this frame slot
    for (auto& rc : trash_[current_frame_]) {
        if (rc.vbuf) vkDestroyBuffer(device_, rc.vbuf, nullptr);
        if (rc.vmem) vkFreeMemory(device_, rc.vmem, nullptr);
        if (rc.ibuf) vkDestroyBuffer(device_, rc.ibuf, nullptr);
        if (rc.imem) vkFreeMemory(device_, rc.imem, nullptr);
        // Free pooled mesh ranges if used
        if (!rc.vbuf && !rc.ibuf && rc.index_count > 0 && rc.vertex_count > 0) {
            chunk_renderer_.free_mesh(rc.first_index, rc.index_count, rc.base_vertex, rc.vertex_count);
        }
    }
    trash_[current_frame_].clear();
    // Streaming update (may schedule new loads and prune old chunks) and drain uploads
    update_streaming();
    drain_mesh_results();

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, sem_image_available_[current_frame_], VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(); return; }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) throw_if_failed(acq, "vkAcquireNextImageKHR failed");

    // Prepare overlay text only if needed for this frame slot (content changed or slot invalid)
    overlay_draw_slot_ = current_frame_;
    if (!hud_text_.empty() && (!overlay_text_valid_[overlay_draw_slot_] || overlay_last_text_ != hud_text_)) {
        overlay_.build_text(overlay_draw_slot_, hud_text_.c_str(), (int)swapchain_extent_.width, (int)swapchain_extent_.height);
        overlay_text_valid_[overlay_draw_slot_] = true;
        overlay_last_text_ = hud_text_;
    }

    // Overlay text is prepared once per frame above

    vkResetCommandBuffer(command_buffers_[imageIndex], 0);
    record_command_buffer(command_buffers_[imageIndex], imageIndex);

    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &sem_image_available_[current_frame_]; si.pWaitDstStageMask = waitStages;
    si.commandBufferCount = 1; si.pCommandBuffers = &command_buffers_[imageIndex];
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &sem_render_finished_[current_frame_];
    throw_if_failed(vkQueueSubmit(queue_graphics_, 1, &si, fences_in_flight_[current_frame_]), "vkQueueSubmit failed");

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &sem_render_finished_[current_frame_];
    pi.swapchainCount = 1; pi.pSwapchains = &swapchain_; pi.pImageIndices = &imageIndex;
    VkResult pres = vkQueuePresentKHR(queue_present_, &pi);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) { recreate_swapchain(); }
    else if (pres != VK_SUCCESS) throw_if_failed(pres, "vkQueuePresentKHR failed");

    current_frame_ = (current_frame_ + 1) % kFramesInFlight;
}

void VulkanApp::cleanup_swapchain() {
    if (pipeline_triangle_) { vkDestroyPipeline(device_, pipeline_triangle_, nullptr); pipeline_triangle_ = VK_NULL_HANDLE; }
    if (pipeline_layout_) { vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr); pipeline_layout_ = VK_NULL_HANDLE; }
    // Overlay pipelines are handled by overlay_ during recreate
    for (auto fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    framebuffers_.clear();
    if (depth_view_) { vkDestroyImageView(device_, depth_view_, nullptr); depth_view_ = VK_NULL_HANDLE; }
    if (depth_image_) { vkDestroyImage(device_, depth_image_, nullptr); depth_image_ = VK_NULL_HANDLE; }
    if (depth_mem_) { vkFreeMemory(device_, depth_mem_, nullptr); depth_mem_ = VK_NULL_HANDLE; }
    if (render_pass_) { vkDestroyRenderPass(device_, render_pass_, nullptr); render_pass_ = VK_NULL_HANDLE; }
    for (auto iv : swapchain_image_views_) vkDestroyImageView(device_, iv, nullptr);
    swapchain_image_views_.clear();
    if (swapchain_) { vkDestroySwapchainKHR(device_, swapchain_, nullptr); swapchain_ = VK_NULL_HANDLE; }
}

void VulkanApp::recreate_swapchain() {
    vkDeviceWaitIdle(device_);
    cleanup_swapchain();
    create_swapchain();
    create_image_views();
    create_render_pass();
    create_depth_resources();
    create_graphics_pipeline();
#include "wf_config.h"
    overlay_.recreate_swapchain(render_pass_, swapchain_extent_, WF_SHADER_DIR);
    chunk_renderer_.recreate(render_pass_, swapchain_extent_, WF_SHADER_DIR);
    overlay_text_valid_.fill(false);
    hud_force_refresh_ = true;
    create_framebuffers();
}

VkShaderModule VulkanApp::load_shader_module(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    std::vector<std::string> fallbacks = {
        std::string("build/shaders/") + base,
        std::string("shaders/") + base,
        std::string("shaders_build/") + base
    };
    return wf::vk::load_shader_module(device_, path, fallbacks);
}

void VulkanApp::create_graphics_pipeline() {
#include "wf_config.h"
    const std::string base = std::string(WF_SHADER_DIR);
    const std::string vsPath = base + "/triangle.vert.spv";
    const std::string fsPath = base + "/triangle.frag.spv";
    VkShaderModule vs = load_shader_module(vsPath);
    VkShaderModule fs = load_shader_module(fsPath);
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(device_, vs, nullptr);
        if (fs) vkDestroyShaderModule(device_, fs, nullptr);
        std::cout << "[info] Shaders not found (" << vsPath << ", " << fsPath << "). Triangle disabled." << std::endl;
        return;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{}; vp.x = 0; vp.y = (float)swapchain_extent_.height; vp.width = (float)swapchain_extent_.width; vp.height = -(float)swapchain_extent_.height; vp.minDepth = 0; vp.maxDepth = 1;
    VkRect2D sc{}; sc.offset = {0,0}; sc.extent = swapchain_extent_;
    VkPipelineViewportStateCreateInfo vpstate{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vpstate.viewportCount = 1; vpstate.pViewports = &vp; vpstate.scissorCount = 1; vpstate.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT; cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    // Depth state for chunk rendering
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);
        return;
    }

    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount = 2; gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vpstate;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = nullptr;
    gpi.pColorBlendState = &cb;
    gpi.layout = pipeline_layout_;
    gpi.renderPass = render_pass_;
    gpi.subpass = 0;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline_triangle_) != VK_SUCCESS) {
        std::cerr << "Failed to create graphics pipeline.\n";
        vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr); pipeline_layout_ = VK_NULL_HANDLE;
    }
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
}

// legacy chunk pipeline removed (ChunkRenderer owns chunk graphics pipeline)
// removed overlay pipeline (handled by OverlayRenderer)


// removed overlay buffer update (handled by OverlayRenderer)

void VulkanApp::create_compute_pipeline() {
    // Load no-op compute shader
    const std::string base = std::string(WF_SHADER_DIR);
    const std::string csPath = base + "/noop.comp.spv";
    VkShaderModule cs = load_shader_module(csPath);
    if (!cs) {
        std::cout << "[info] Compute shader not found (" << csPath << "). Compute disabled." << std::endl;
        return;
    }
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = cs;
    stage.pName = "main";

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &pipeline_layout_compute_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, cs, nullptr);
        return;
    }

    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = stage;
    ci.layout = pipeline_layout_compute_;
    if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline_compute_) != VK_SUCCESS) {
        std::cerr << "Failed to create compute pipeline." << std::endl;
        vkDestroyPipelineLayout(device_, pipeline_layout_compute_, nullptr); pipeline_layout_compute_ = VK_NULL_HANDLE;
    }
    vkDestroyShaderModule(device_, cs, nullptr);
}

uint32_t VulkanApp::find_memory_type(uint32_t typeBits, VkMemoryPropertyFlags properties) {
    return wf::vk::find_memory_type(physical_device_, typeBits, properties);
}

void VulkanApp::create_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf, VkDeviceMemory& mem, const void* data) {
    wf::vk::create_buffer(physical_device_, device_, size, usage,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          buf, mem);
    if (data) wf::vk::upload_host_visible(device_, mem, size, data, 0);
}

void VulkanApp::profile_append_csv(const std::string& line) {
    if (!profile_csv_enabled_) return;
    std::lock_guard<std::mutex> lk(profile_mutex_);
    std::ofstream out;
    out.open(profile_csv_path_, profile_header_written_ ? (std::ios::app) : (std::ios::out));
    if (!out.good()) return;
    if (!profile_header_written_) {
        out << "event,time_s,items,meshed,gen_ms,mesh_ms,total_or_frame_ms" << '\n';
        profile_header_written_ = true;
    }
    out << line;
}

void VulkanApp::loader_thread_main() {
    for (;;) {
        LoadRequest req;
        {
            std::unique_lock<std::mutex> lk(loader_mutex_);
            loader_cv_.wait(lk, [&]{ return loader_quit_ || !request_queue_.empty(); });
            if (loader_quit_) break;
            // Coalesce to latest request
            req = request_queue_.back();
            request_queue_.clear();
            loader_busy_ = true;
        }
        if (log_stream_) {
            std::cout << "[stream] process request: face=" << req.face << " ring=" << req.ring_radius
                      << " ci=" << req.ci << " cj=" << req.cj << " ck=" << req.ck
                      << " k_down=" << req.k_down << " k_up=" << req.k_up << "\n";
        }
        build_ring_job(req.face, req.ring_radius, req.ci, req.cj, req.ck, req.k_down, req.k_up, req.fwd_s, req.fwd_t, req.gen);
        {
            std::lock_guard<std::mutex> lk(loader_mutex_);
            loader_busy_ = false;
        }
    }
}
void VulkanApp::schedule_delete_chunk(const RenderChunk& rc) {
    // Defer destruction to the next frame slot to guarantee GPU is done using previous submissions.
    size_t slot = (current_frame_ + 1) % kFramesInFlight;
    trash_[slot].push_back(rc);
}

void VulkanApp::start_loader_thread() {
    if (!loader_thread_.joinable()) {
        std::lock_guard<std::mutex> lk(loader_mutex_);
        loader_quit_ = false;
        loader_busy_ = false;
        loader_thread_ = std::thread(&VulkanApp::loader_thread_main, this);
    }
}

void VulkanApp::enqueue_ring_request(int face, int ring_radius, std::int64_t center_i, std::int64_t center_j, std::int64_t center_k, int k_down, int k_up, float fwd_s, float fwd_t) {
    uint64_t gen = ++request_gen_;
    {
        std::lock_guard<std::mutex> lk(loader_mutex_);
        // Collapse to the latest request
        LoadRequest req{face, ring_radius, center_i, center_j, center_k, k_down, k_up, fwd_s, fwd_t, gen};
        request_queue_.clear();
        request_queue_.push_back(req);
    }
    loader_cv_.notify_one();
}

void VulkanApp::start_initial_ring_async() {
    // Initialize persistent worker and enqueue initial ring around current camera on the appropriate face
    start_loader_thread();
    const PlanetConfig& cfg = planet_cfg_;
    const int N = Chunk64::N;
    const double chunk_m = (double)N * cfg.voxel_size_m;
    // Determine face from camera position
    Float3 eye{(float)cam_pos_[0], (float)cam_pos_[1], (float)cam_pos_[2]};
    Float3 dir = normalize(eye);
    int face = face_from_direction(dir);
    Float3 right, up, forward; face_basis(face, right, up, forward);
    double s = eye.x * right.x + eye.y * right.y + eye.z * right.z;
    double t = eye.x * up.x    + eye.y * up.y    + eye.z * up.z;
    ring_center_i_ = (std::int64_t)std::floor(s / chunk_m);
    ring_center_j_ = (std::int64_t)std::floor(t / chunk_m);
    ring_center_k_ = (std::int64_t)std::floor((double)length(eye) / chunk_m);
    stream_face_ = face;
    // Project camera forward to face s/t for prioritization
    float cyaw = std::cos(cam_yaw_), syaw = std::sin(cam_yaw_);
    float cp = std::cos(cam_pitch_), sp = std::sin(cam_pitch_);
    float fwd[3] = { cp*cyaw, sp, cp*syaw };
    float fwd_s = fwd[0]*right.x + fwd[1]*right.y + fwd[2]*right.z;
    float fwd_t = fwd[0]*up.x    + fwd[1]*up.y    + fwd[2]*up.z;
    enqueue_ring_request(face, ring_radius_, ring_center_i_, ring_center_j_, ring_center_k_, k_down_, k_up_, fwd_s, fwd_t);
    if (log_stream_) {
        std::cout << "[stream] initial request: face=" << face << " ring=" << ring_radius_
                  << " ci=" << ring_center_i_ << " cj=" << ring_center_j_ << " ck=" << ring_center_k_
                  << " fwd_s=" << fwd_s << " fwd_t=" << fwd_t << " k_down=" << k_down_ << " k_up=" << k_up_ << "\n";
    }
}

void VulkanApp::build_ring_job(int face, int ring_radius, std::int64_t center_i, std::int64_t center_j, std::int64_t center_k, int k_down, int k_up, float fwd_s, float fwd_t, uint64_t job_gen) {
    const PlanetConfig& cfg = planet_cfg_;
    const int N = Chunk64::N;
    const float s = (float)cfg.voxel_size_m;
    const double chunk_m = (double)N * cfg.voxel_size_m;
    const std::int64_t k0 = center_k; // center radial shell from request
    Float3 right, up, forward; face_basis(face, right, up, forward);

    const int tile_span = ring_radius;
    const int W = 2*tile_span + 1;
    const int KD = k_down + k_up + 1;
    auto idx_of = [&](int di, int dj, int dk){ int ix = di + tile_span; int jy = dj + tile_span; int kz = dk + k_down; return (kz * W + jy) * W + ix; };
    std::vector<Chunk64> chunks(W * W * KD);

    // Build prioritized list of tile offsets: prefer ahead of camera (by fwd_s/fwd_t), then nearer distance
    struct Off { int di, dj; int dist2; float dot; };
    std::vector<Off> order; order.reserve(W*W);
    // Normalize the direction bias in the s/t plane
    float len = std::sqrt(fwd_s*fwd_s + fwd_t*fwd_t);
    float dir_s = (len > 1e-6f) ? (fwd_s / len) : 0.0f;
    float dir_t = (len > 1e-6f) ? (fwd_t / len) : 0.0f;
    for (int dj = -tile_span; dj <= tile_span; ++dj) {
        for (int di = -tile_span; di <= tile_span; ++di) {
            int d2 = di*di + dj*dj;
            float dot = di*dir_s + dj*dir_t;
            order.push_back(Off{di, dj, d2, dot});
        }
    }
    std::sort(order.begin(), order.end(), [&](const Off& a, const Off& b){
        if (a.dist2 != b.dist2) return a.dist2 < b.dist2; // always prefer nearer rings first
        if (a.dot != b.dot) return a.dot > b.dot;         // within ring, prefer ahead of camera
        if (a.dj != b.dj) return a.dj < b.dj;             // stable
        return a.di < b.di;
    });

    // Two-phase approach: parallel generation first, then neighbor-aware meshing pass.
    struct Task { int di, dj, dk; };
    std::vector<Task> tasks;
    tasks.reserve(W * W * KD);
    for (int dj = -tile_span; dj <= tile_span; ++dj) {
        for (int di = -tile_span; di <= tile_span; ++di) {
            for (int dk = -k_down; dk <= k_up; ++dk) tasks.push_back(Task{di, dj, dk});
        }
    }
    // Parallel generation
    auto t0 = std::chrono::steady_clock::now();
    std::atomic<size_t> ti{0};
    int nthreads = loader_threads_ > 0 ? loader_threads_ : (int)std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    workers.reserve(nthreads);
    for (int w = 0; w < nthreads; ++w) {
        workers.emplace_back([&, job_gen](){
            for (;;) {
                if (loader_quit_) return;
                if (request_gen_.load() != job_gen) return;
                size_t idx = ti.fetch_add(1, std::memory_order_relaxed);
                if (idx >= tasks.size()) break;
                const Task t = tasks[idx];
                int di = t.di, dj = t.dj, dk = t.dk;
                std::int64_t kk = k0 + dk;
                FaceChunkKey key{face, center_i + di, center_j + dj, kk};
                Chunk64& c = chunks[idx_of(di, dj, dk)];
                if (!RegionIO::load_chunk(key, c)) {
                    for (int z = 0; z < N; ++z) {
                        for (int y = 0; y < N; ++y) {
                            for (int x = 0; x < N; ++x) {
                                double s0 = (double)(center_i + di) * chunk_m + (x + 0.5) * cfg.voxel_size_m;
                                double t0 = (double)(center_j + dj) * chunk_m + (y + 0.5) * cfg.voxel_size_m;
                                double r0 = (double)kk * chunk_m + (z + 0.5) * cfg.voxel_size_m;
                                // Map face-local s/t at radius r0 to a spherical direction via face-basis cosines
                                float uc = (float)(s0 / r0);
                                float vc = (float)(t0 / r0);
                                float w2 = std::max(0.0f, 1.0f - (uc*uc + vc*vc));
                                float wc = std::sqrt(w2);
                                Float3 dir_sph = wf::normalize(Float3{ right.x*uc + up.x*vc + forward.x*wc,
                                                                       right.y*uc + up.y*vc + forward.y*wc,
                                                                       right.z*uc + up.z*vc + forward.z*wc });
                                Float3 p = dir_sph * (float)r0;
                                Int3 voxel{ (i64)std::llround(p.x / cfg.voxel_size_m), (i64)std::llround(p.y / cfg.voxel_size_m), (i64)std::llround(p.z / cfg.voxel_size_m) };
                                auto sb = sample_base(cfg, voxel);
                                c.set_voxel(x, y, z, sb.material);
                            }
                        }
                    }
                    if (save_chunks_enabled_) RegionIO::save_chunk(key, c);
                }
            }
        });
    }
    for (auto& th : workers) th.join();
    auto t1 = std::chrono::steady_clock::now();
    double gen_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    loader_last_gen_ms_.store(gen_ms);
    loader_last_chunks_.store((int)tasks.size());
    if (loader_quit_ || request_gen_.load() != job_gen) return;

    // Meshing pass: parallelize over prioritized tasks (di,dj,dk)
    int meshed_count = 0;
    // Precompute camera forward world vector from fwd_s/fwd_t (projective ratios)
    float u_cam = fwd_s;
    float v_cam = fwd_t;
    float inv_len_cam = 1.0f / std::sqrt(1.0f + u_cam*u_cam + v_cam*v_cam);
    Float3 fwd_world_cam = wf::normalize(Float3{
        right.x * (u_cam * inv_len_cam) + up.x * (v_cam * inv_len_cam) + forward.x * (1.0f * inv_len_cam),
        right.y * (u_cam * inv_len_cam) + up.y * (v_cam * inv_len_cam) + forward.y * (1.0f * inv_len_cam),
        right.z * (u_cam * inv_len_cam) + up.z * (v_cam * inv_len_cam) + forward.z * (1.0f * inv_len_cam)
    });
    float cone_cos = std::cos(stream_cone_deg_ * 0.01745329252f);
    struct MTask { int di, dj, dk; };
    std::vector<MTask> mtasks; mtasks.reserve(W * W * KD);
    for (const auto& off : order) {
        for (int dk = -k_down; dk <= k_up; ++dk) mtasks.push_back(MTask{off.di, off.dj, dk});
    }
    std::atomic<size_t> mi{0};
    std::atomic<int> meshed_accum{0};
    std::vector<std::thread> mesh_workers;
    mesh_workers.reserve(nthreads);
    auto mesh_worker = [&]() {
        int local_meshed = 0;
        while (true) {
            if (loader_quit_ || request_gen_.load() != job_gen) break;
            size_t idx = mi.fetch_add(1, std::memory_order_relaxed);
            if (idx >= mtasks.size()) break;
            const auto t = mtasks[idx];
            int di = t.di, dj = t.dj, dk = t.dk;
            std::int64_t kk = k0 + dk;
            const Chunk64& c = chunks[idx_of(di, dj, dk)];
            // Tile center direction for meshing cone test
            float S0 = (float)((center_i + di) * chunk_m);
            float T0 = (float)((center_j + dj) * chunk_m);
            float R0 = (float)(kk * chunk_m);
            const float halfm = (float)(N * s * 0.5f);
            float Sc = S0 + halfm, Tc = T0 + halfm, Rc = R0 + halfm;
            float cr = Sc / Rc, cu = Tc / Rc;
            float cf = std::sqrt(std::max(0.0f, 1.0f - (cr*cr + cu*cu)));
            Float3 dirc = wf::normalize(Float3{ right.x*cr + up.x*cu + forward.x*cf,
                                                right.y*cr + up.y*cu + forward.y*cf,
                                                right.z*cr + up.z*cu + forward.z*cf });
            float dcam = fwd_world_cam.x*dirc.x + fwd_world_cam.y*dirc.y + fwd_world_cam.z*dirc.z;
            if (dcam < cone_cos) {
                continue; // outside forward cone, skip meshing this tile for now
            }
            const Chunk64* nx = (di > -tile_span) ? &chunks[idx_of(di - 1, dj, dk)] : nullptr;
            const Chunk64* px = (di <  tile_span) ? &chunks[idx_of(di + 1, dj, dk)] : nullptr;
            const Chunk64* ny = (dj > -tile_span) ? &chunks[idx_of(di, dj - 1, dk)] : nullptr;
            const Chunk64* py = (dj <  tile_span) ? &chunks[idx_of(di, dj + 1, dk)] : nullptr;
            const Chunk64* nz = (dk > -k_down)    ? &chunks[idx_of(di, dj, dk - 1)] : nullptr;
            const Chunk64* pz = (dk <  k_up)      ? &chunks[idx_of(di, dj, dk + 1)] : nullptr;
            Mesh m;
            mesh_chunk_greedy_neighbors(c, nx, px, ny, py, nz, pz, m, s);
            if (m.indices.empty()) continue;
            local_meshed++;
            // S0/T0/R0 already computed above
            for (auto& vert : m.vertices) {
                Float3 lp{vert.x, vert.y, vert.z};
                float S = S0 + lp.x;
                float T = T0 + lp.y;
                float R = R0 + lp.z;
                float uc = (R != 0.0f) ? (S / R) : 0.0f;
                float vc = (R != 0.0f) ? (T / R) : 0.0f;
                float w2 = std::max(0.0f, 1.0f - (uc*uc + vc*vc));
                float wc = std::sqrt(w2);
                Float3 dir_sph = wf::normalize(Float3{ right.x*uc + up.x*vc + forward.x*wc,
                                                       right.y*uc + up.y*vc + forward.y*wc,
                                                       right.z*uc + up.z*vc + forward.z*wc });
                Float3 wp = dir_sph * R;
                vert.x = wp.x; vert.y = wp.y; vert.z = wp.z;
                // Temporarily keep existing normal; we will recompute face normals below
            }
            // Recompute flat face normals from world-space triangle geometry for clearer shading
            auto cross = [](Float3 a, Float3 b) -> Float3 {
                return Float3{ a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
            };
            auto sub = [](Float3 a, Float3 b) -> Float3 { return Float3{a.x-b.x, a.y-b.y, a.z-b.z}; };
            for (size_t ii = 0; ii + 2 < m.indices.size(); ii += 3) {
                uint32_t i0 = m.indices[ii+0];
                uint32_t i1 = m.indices[ii+1];
                uint32_t i2 = m.indices[ii+2];
                const Vertex &v0 = m.vertices[i0];
                const Vertex &v1 = m.vertices[i1];
                const Vertex &v2 = m.vertices[i2];
                Float3 p0{v0.x, v0.y, v0.z};
                Float3 p1{v1.x, v1.y, v1.z};
                Float3 p2{v2.x, v2.y, v2.z};
                Float3 e1 = sub(p1, p0);
                Float3 e2 = sub(p2, p0);
                Float3 n = wf::normalize(cross(e1, e2));
                // Optional outward push for near-horizontal (radial) faces to better align with continuous surface
                if (surface_push_m_ > 0.0f) {
                    Float3 r0 = wf::normalize(p0);
                    float radial = std::fabs(n.x*r0.x + n.y*r0.y + n.z*r0.z);
                    if (radial > 0.8f) {
                        Float3 push = Float3{ r0.x * surface_push_m_, r0.y * surface_push_m_, r0.z * surface_push_m_ };
                        m.vertices[i0].x = p0.x + push.x; m.vertices[i0].y = p0.y + push.y; m.vertices[i0].z = p0.z + push.z;
                        m.vertices[i1].x = p1.x + push.x; m.vertices[i1].y = p1.y + push.y; m.vertices[i1].z = p1.z + push.z;
                        m.vertices[i2].x = p2.x + push.x; m.vertices[i2].y = p2.y + push.y; m.vertices[i2].z = p2.z + push.z;
                        p0 = Float3{m.vertices[i0].x, m.vertices[i0].y, m.vertices[i0].z};
                        p1 = Float3{m.vertices[i1].x, m.vertices[i1].y, m.vertices[i1].z};
                        p2 = Float3{m.vertices[i2].x, m.vertices[i2].y, m.vertices[i2].z};
                        e1 = sub(p1, p0); e2 = sub(p2, p0);
                        n = wf::normalize(cross(e1, e2));
                    }
                }
                m.vertices[i0].nx = n.x; m.vertices[i0].ny = n.y; m.vertices[i0].nz = n.z;
                m.vertices[i1].nx = n.x; m.vertices[i1].ny = n.y; m.vertices[i1].nz = n.z;
                m.vertices[i2].nx = n.x; m.vertices[i2].ny = n.y; m.vertices[i2].nz = n.z;
            }
            MeshResult res;
            res.vertices = std::move(m.vertices);
            res.indices = std::move(m.indices);
            const float diag_half = halfm * 1.73205080757f; // sqrt(3)
            Float3 wc = dirc * Rc;
            res.center[0] = wc.x; res.center[1] = wc.y; res.center[2] = wc.z; res.radius = diag_half;
            res.key = FaceChunkKey{face, center_i + di, center_j + dj, kk};
            {
                std::lock_guard<std::mutex> lk(loader_mutex_);
                results_queue_.push_back(std::move(res));
            }
            loader_cv_.notify_one();
        }
        if (local_meshed) meshed_accum.fetch_add(local_meshed, std::memory_order_relaxed);
    };
    for (int w = 0; w < nthreads; ++w) mesh_workers.emplace_back(mesh_worker);
    for (auto& th : mesh_workers) th.join();
    meshed_count = meshed_accum.load();
    auto t2 = std::chrono::steady_clock::now();
    double mesh_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    loader_last_mesh_ms_.store(mesh_ms);
    loader_last_meshed_.store(meshed_count);
    loader_last_total_ms_.store(gen_ms + mesh_ms);
    if (profile_csv_enabled_) {
        double tsec = std::chrono::duration<double>(t2 - app_start_tp_).count();
        char line[256];
        std::snprintf(line, sizeof(line),
                      "job,%.3f,%d,%d,%.3f,%.3f,%.3f\n",
                      tsec, (int)tasks.size(), meshed_count, gen_ms, mesh_ms, gen_ms + mesh_ms);
        profile_append_csv(line);
    }
}

void VulkanApp::drain_mesh_results() {
    // Drain up to uploads_per_frame_limit_ results and create GPU buffers
    int uploaded = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        MeshResult res;
        {
            std::lock_guard<std::mutex> lk(loader_mutex_);
            if (results_queue_.empty()) break;
            res = std::move(results_queue_.front());
            results_queue_.pop_front();
        }
        RenderChunk rc;
        rc.index_count = (uint32_t)res.indices.size();
        rc.vertex_count = (uint32_t)res.vertices.size();
        // Upload into pooled buffers with free-list reuse
        bool ok_upload = chunk_renderer_.upload_mesh(res.vertices.data(), res.vertices.size(),
                                                    res.indices.data(), res.indices.size(),
                                                    rc.first_index, rc.base_vertex);
        if (!ok_upload) {
            if (log_stream_) {
                std::cerr << "[stream] skip upload (pool full): face=" << rc.key.face
                          << " i=" << rc.key.i << " j=" << rc.key.j << " k=" << rc.key.k
                          << " vtx=" << res.vertices.size() << " idx=" << res.indices.size() << "\n";
            }
            continue;
        }
        rc.vbuf = VK_NULL_HANDLE; rc.vmem = VK_NULL_HANDLE; rc.ibuf = VK_NULL_HANDLE; rc.imem = VK_NULL_HANDLE;
        rc.center[0] = res.center[0]; rc.center[1] = res.center[1]; rc.center[2] = res.center[2]; rc.radius = res.radius;
        rc.key = res.key;
        // Replace existing chunk with same key, if present
        bool replaced = false;
        for (size_t i = 0; i < render_chunks_.size(); ++i) {
            if (render_chunks_[i].key.face == rc.key.face && render_chunks_[i].key.i == rc.key.i && render_chunks_[i].key.j == rc.key.j && render_chunks_[i].key.k == rc.key.k) {
                // Defer deletion to avoid destroying resources still in use by the GPU
                schedule_delete_chunk(render_chunks_[i]);
                render_chunks_[i] = rc;
                replaced = true;
                if (log_stream_) {
                    std::cout << "[stream] replace: face=" << rc.key.face
                              << " i=" << rc.key.i << " j=" << rc.key.j << " k=" << rc.key.k
                              << " idx_count=" << rc.index_count << " vtx_count=" << rc.vertex_count
                              << " first_index=" << rc.first_index << " base_vertex=" << rc.base_vertex << "\n";
                }
                break;
            }
        }
        if (!replaced) {
            render_chunks_.push_back(rc);
            if (log_stream_) {
                std::cout << "[stream] add: face=" << rc.key.face
                          << " i=" << rc.key.i << " j=" << rc.key.j << " k=" << rc.key.k
                          << " idx_count=" << rc.index_count << " vtx_count=" << rc.vertex_count
                          << " first_index=" << rc.first_index << " base_vertex=" << rc.base_vertex << "\n";
            }
        }
        if (++uploaded >= uploads_per_frame_limit_) break;
    }
    auto t1 = std::chrono::steady_clock::now();
    last_upload_count_ = uploaded;
    last_upload_ms_ = std::chrono::duration<double, std::milli>(t1 - t0).count();
    // Simple moving average
    if (uploaded > 0) {
        if (upload_ms_avg_ <= 0.0) upload_ms_avg_ = last_upload_ms_;
        else upload_ms_avg_ = upload_ms_avg_ * 0.8 + last_upload_ms_ * 0.2;
    }
    if (profile_csv_enabled_ && uploaded > 0) {
        double tsec = std::chrono::duration<double>(t1 - app_start_tp_).count();
        char line[128];
        std::snprintf(line, sizeof(line), "upload,%.3f,%d,%.3f\n", tsec, uploaded, last_upload_ms_);
        profile_append_csv(line);
    }
}

void VulkanApp::prune_chunks_outside(int face, std::int64_t ci, std::int64_t cj, int span) {
    for (size_t i = 0; i < render_chunks_.size();) {
        const auto& rk = render_chunks_[i].key;
        if (rk.face != face || std::llabs(rk.i - ci) > span || std::llabs(rk.j - cj) > span) {
            if (log_stream_) {
                std::cout << "[stream] prune: face=" << rk.face << " i=" << rk.i << " j=" << rk.j << " k=" << rk.k
                          << " first_index=" << render_chunks_[i].first_index
                          << " base_vertex=" << render_chunks_[i].base_vertex
                          << " idx_count=" << render_chunks_[i].index_count
                          << " vtx_count=" << render_chunks_[i].vertex_count << "\n";
            }
            // Defer deletion/free; chunk might still be referenced by commands submitted last frame
            schedule_delete_chunk(render_chunks_[i]);
            render_chunks_.erase(render_chunks_.begin() + i);
        } else {
            ++i;
        }
    }
}

void VulkanApp::prune_chunks_multi(const std::vector<AllowRegion>& allows) {
    auto inside_any = [&](const FaceChunkKey& k) -> bool {
        for (const auto& a : allows) {
            if (k.face != a.face) continue;
            if (std::llabs(k.i - a.ci) > a.span) continue;
            if (std::llabs(k.j - a.cj) > a.span) continue;
            std::int64_t kmin = a.ck - a.k_down;
            std::int64_t kmax = a.ck + a.k_up;
            if (k.k < kmin || k.k > kmax) continue;
            return true;
        }
        return false;
    };
    for (size_t i = 0; i < render_chunks_.size();) {
        const auto& rc = render_chunks_[i];
        if (!inside_any(rc.key)) {
            if (log_stream_) {
                std::cout << "[stream] prune: face=" << rc.key.face << " i=" << rc.key.i << " j=" << rc.key.j << " k=" << rc.key.k
                          << " first_index=" << rc.first_index
                          << " base_vertex=" << rc.base_vertex
                          << " idx_count=" << rc.index_count
                          << " vtx_count=" << rc.vertex_count << "\n";
            }
            schedule_delete_chunk(rc);
            render_chunks_.erase(render_chunks_.begin() + i);
        } else {
            ++i;
        }
    }
}

void VulkanApp::update_streaming() {
    // Recenter the ring based on camera position; dynamically choose face by camera direction
    const PlanetConfig& cfg = planet_cfg_;
    const int N = Chunk64::N;
    const double chunk_m = (double)N * cfg.voxel_size_m;
    Float3 eye{cam_pos_[0], cam_pos_[1], cam_pos_[2]};
    Float3 dir = normalize(eye);
    int cur_face = face_from_direction(dir);
    Float3 right, up, forward; face_basis(cur_face, right, up, forward);
    double s = eye.x * right.x + eye.y * right.y + eye.z * right.z;
    double t = eye.x * up.x    + eye.y * up.y    + eye.z * up.z;
    std::int64_t ci = (std::int64_t)std::floor(s / chunk_m);
    std::int64_t cj = (std::int64_t)std::floor(t / chunk_m);
    std::int64_t ck = (std::int64_t)std::floor((double)length(eye) / chunk_m);

    bool face_changed = (cur_face != stream_face_);
    if (face_changed) {
        // Start holding previous face for a brief time to avoid popping while new face loads
        prev_face_ = stream_face_;
        prev_center_i_ = ring_center_i_;
        prev_center_j_ = ring_center_j_;
        prev_center_k_ = ring_center_k_;
        face_keep_timer_s_ = face_keep_time_cfg_s_;
        stream_face_ = cur_face;
        ring_center_i_ = ci;
        ring_center_j_ = cj;
        ring_center_k_ = ck;
        // Bias loading order by camera forward projected onto new face basis
        float cyaw = std::cos(cam_yaw_), syaw = std::sin(cam_yaw_);
        float cp = std::cos(cam_pitch_), sp = std::sin(cam_pitch_);
        float fwd[3] = { cp*cyaw, sp, cp*syaw };
        float fwd_s = fwd[0]*right.x + fwd[1]*right.y + fwd[2]*right.z;
        float fwd_t = fwd[0]*up.x    + fwd[1]*up.y    + fwd[2]*up.z;
        enqueue_ring_request(stream_face_, ring_radius_, ring_center_i_, ring_center_j_, ring_center_k_, k_down_, k_up_, fwd_s, fwd_t);
        if (log_stream_) {
            std::cout << "[stream] face switch -> face=" << stream_face_ << " ring=" << ring_radius_
                      << " ci=" << ring_center_i_ << " cj=" << ring_center_j_ << " fwd_s=" << fwd_s << " fwd_t=" << fwd_t << "\n";
        }
    } else {
        // Same face: if we've moved tiles, request an update
        if (ci != ring_center_i_ || cj != ring_center_j_ || ck != ring_center_k_) {
            ring_center_i_ = ci; ring_center_j_ = cj; ring_center_k_ = ck;
            float cyaw = std::cos(cam_yaw_), syaw = std::sin(cam_yaw_);
            float cp = std::cos(cam_pitch_), sp = std::sin(cam_pitch_);
            float fwd[3] = { cp*cyaw, sp, cp*syaw };
            float fwd_s = fwd[0]*right.x + fwd[1]*right.y + fwd[2]*right.z;
            float fwd_t = fwd[0]*up.x    + fwd[1]*up.y    + fwd[2]*up.z;
            enqueue_ring_request(stream_face_, ring_radius_, ring_center_i_, ring_center_j_, ring_center_k_, k_down_, k_up_, fwd_s, fwd_t);
            if (log_stream_) {
                std::cout << "[stream] move request: face=" << stream_face_ << " ring=" << ring_radius_
                          << " ci=" << ring_center_i_ << " cj=" << ring_center_j_ << " ck=" << ring_center_k_
                          << " fwd_s=" << fwd_s << " fwd_t=" << fwd_t << "\n";
            }
        }
    }

    // Count down previous-face hold timer
    if (face_keep_timer_s_ > 0.0f) {
        // Approximate frame time via HUD smoothing cadence; alternatively, compute dt from glfw each frame
        // Here, we decrement by a small fixed step per frame; more accurate would be to pass dt
        face_keep_timer_s_ = std::max(0.0f, face_keep_timer_s_ - 1.0f/60.0f);
    }

    // Build prune allow-list: keep current face ring with hysteresis in s/t and k; optionally keep previous face while timer active
    std::vector<AllowRegion> allows;
    allows.push_back(AllowRegion{stream_face_, ring_center_i_, ring_center_j_, ring_center_k_, ring_radius_ + prune_margin_, k_down_ + k_prune_margin_, k_up_ + k_prune_margin_});
    if (face_keep_timer_s_ > 0.0f && prev_face_ >= 0) {
        allows.push_back(AllowRegion{prev_face_, prev_center_i_, prev_center_j_, prev_center_k_, ring_radius_ + prune_margin_, k_down_ + k_prune_margin_, k_up_ + k_prune_margin_});
    }
    prune_chunks_multi(allows);
}
}
