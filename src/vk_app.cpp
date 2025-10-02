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
#include "ui/ui_text.h"
#include "ui/ui_primitives.h"
#include "ui/ui_id.h"

namespace wf {

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* /*pUserData*/) {
    (void)messageSeverity;
    (void)messageType;
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

void VulkanApp::set_config_path(std::string path) {
    config_path_override_ = std::move(path);
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
        if (!glfwInit()) {
            std::cerr << "Failed to initialize GLFW\n";
            std::abort();
        }
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window_ = glfwCreateWindow(width_, height_, "Wanderforge", nullptr, nullptr);
        if (!window_) {
            std::cerr << "Failed to create GLFW window\n";
            std::abort();
        }
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
        const double voxel_m = cfg.voxel_size_m;
        const double chunk_m = (double)N * voxel_m;
        const double half_m = chunk_m * 0.5;
        const std::int64_t k0 = (std::int64_t)std::floor(cfg.radius_m / chunk_m);
        const double R0 = (double)k0 * chunk_m;
        const double Rc = R0 + half_m;
        Float3 right, up, forward; face_basis(0, right, up, forward);
        double Sc = half_m;
        double Tc = half_m;
        float cr = (float)(Sc / Rc);
        float cu = (float)(Tc / Rc);
        float cf = std::sqrt(std::max(0.0f, 1.0f - (cr*cr + cu*cu)));
        Float3 dirc = wf::normalize(Float3{
            right.x * cr + up.x * cu + forward.x * cf,
            right.y * cr + up.y * cu + forward.y * cf,
            right.z * cr + up.z * cu + forward.z * cf
        });
        Float3 chunk_center = dirc * (float)Rc;
        float view_back = 12.0f;
        Float3 eye = dirc * (float)(Rc + view_back) + up * 2.0f;
        cam_pos_[0] = eye.x; cam_pos_[1] = eye.y; cam_pos_[2] = eye.z;
        Float3 look = wf::normalize(chunk_center - eye);
        cam_yaw_ = std::atan2(look.z, look.x);
        cam_pitch_ = std::asin(std::clamp(look.y, -1.0f, 1.0f));
        bool chunk_outside = (Rc > (eye.x*dirc.x + eye.y*dirc.y + eye.z*dirc.z));
        if (chunk_outside) {
            cam_yaw_ += 3.14159265f;
            cam_pitch_ = -cam_pitch_;
        }
        std::cout << "[spawn] look=" << look.x << "," << look.y << "," << look.z
                  << " yaw=" << cam_yaw_ << " pitch=" << cam_pitch_ << "\n";
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

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
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
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
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
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = qf;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        qcis.push_back(qci);
    }
    enabled_device_exts_.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
#ifdef __APPLE__
    if (has_device_extension(physical_device_, "VK_KHR_portability_subset"))
        enabled_device_exts_.push_back("VK_KHR_portability_subset");
#endif

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = (uint32_t)qcis.size();
    dci.pQueueCreateInfos = qcis.data();
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

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
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
        VkImageViewCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
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
    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 2;
    rpci.pAttachments = atts;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    throw_if_failed(vkCreateRenderPass(device_, &rpci, nullptr, &render_pass_), "vkCreateRenderPass failed");
}

void VulkanApp::create_framebuffers() {
    framebuffers_.resize(swapchain_image_views_.size());
    for (size_t i=0;i<swapchain_image_views_.size();++i) {
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
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
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
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mt;
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

void VulkanApp::create_command_pool_and_buffers() {
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family_graphics_;
    throw_if_failed(vkCreateCommandPool(device_, &pci, nullptr, &command_pool_), "vkCreateCommandPool failed");

    command_buffers_.resize(framebuffers_.size());
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = command_pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = (uint32_t)command_buffers_.size();
    throw_if_failed(vkAllocateCommandBuffers(device_, &ai, command_buffers_.data()), "vkAllocateCommandBuffers failed");
}

void VulkanApp::create_sync_objects() {
    sem_image_available_.resize(kFramesInFlight);
    sem_render_finished_.resize(kFramesInFlight);
    fences_in_flight_.resize(kFramesInFlight);
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i=0;i<kFramesInFlight;i++) {
        throw_if_failed(vkCreateSemaphore(device_, &sci, nullptr, &sem_image_available_[i]), "vkCreateSemaphore failed");
        throw_if_failed(vkCreateSemaphore(device_, &sci, nullptr, &sem_render_finished_[i]), "vkCreateSemaphore failed");
        throw_if_failed(vkCreateFence(device_, &fci, nullptr, &fences_in_flight_[i]), "vkCreateFence failed");
    }
}

void VulkanApp::record_command_buffer(VkCommandBuffer cmd, uint32_t imageIndex) {
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    throw_if_failed(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer failed");

    // No-op compute dispatch before rendering
    if (pipeline_compute_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_compute_);
        vkCmdDispatch(cmd, 1, 1, 1);
    }

    VkClearValue clear{ { {0.02f, 0.02f, 0.06f, 1.0f} } };
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = render_pass_;
    rbi.framebuffer = framebuffers_[imageIndex];
    rbi.renderArea.offset = {0,0}; rbi.renderArea.extent = swapchain_extent_;
    VkClearValue clears[2];
    clears[0] = clear;
    clears[1].depthStencil = {1.0f, 0};
    rbi.clearValueCount = 2; rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    if ((!render_chunks_.empty()) && chunk_renderer_.is_ready()) {
        float aspect = (float)swapchain_extent_.width / (float)swapchain_extent_.height;
        auto P = wf::perspective_from_deg(fov_deg_, aspect, near_m_, far_m_);
        float eye_arr[3] = { (float)cam_pos_[0], (float)cam_pos_[1], (float)cam_pos_[2] };
        Float3 eye{eye_arr[0], eye_arr[1], eye_arr[2]};

        wf::Mat4 V;
        Float3 forward{};
        Float3 up_vec{};
        Float3 right_vec{};

        float cyaw = std::cos(cam_yaw_), syaw = std::sin(cam_yaw_);
        float cp = std::cos(cam_pitch_), sp = std::sin(cam_pitch_);

        if (!walk_mode_) {
            V = wf::view_from_yaw_pitch(cam_yaw_, cam_pitch_, eye_arr);
            forward = Float3{cp * cyaw, sp, cp * syaw};
            Float3 world_up{0.0f, 1.0f, 0.0f};
            right_vec = wf::normalize(Float3{
                forward.y * world_up.z - forward.z * world_up.y,
                forward.z * world_up.x - forward.x * world_up.z,
                forward.x * world_up.y - forward.y * world_up.x
            });
            up_vec = Float3{
                right_vec.y * forward.z - right_vec.z * forward.y,
                right_vec.z * forward.x - right_vec.x * forward.z,
                right_vec.x * forward.y - right_vec.y * forward.x
            };
        } else {
            Float3 updir = wf::normalize(eye);
            Float3 view_dir{cp * cyaw, sp, cp * syaw};
            auto cross3 = [](const Float3& a, const Float3& b) {
                return Float3{
                    a.y * b.z - a.z * b.y,
                    a.z * b.x - a.x * b.z,
                    a.x * b.y - a.y * b.x
                };
            };
            auto dot3 = [](const Float3& a, const Float3& b) {
                return a.x * b.x + a.y * b.y + a.z * b.z;
            };
            Float3 forward_dir = wf::normalize(view_dir);
            Float3 right_candidate = cross3(updir, forward_dir);
            if (wf::length(right_candidate) < 1e-5f) {
                Float3 fallback = std::fabs(updir.y) < 0.9f ? Float3{0.0f, 1.0f, 0.0f} : Float3{1.0f, 0.0f, 0.0f};
                right_candidate = cross3(fallback, updir);
                if (wf::length(right_candidate) < 1e-5f) {
                    fallback = Float3{0.0f, 0.0f, 1.0f};
                    right_candidate = cross3(fallback, updir);
                }
            }
            right_vec = wf::normalize(right_candidate);
            up_vec = wf::normalize(cross3(forward_dir, right_vec));
            if (dot3(up_vec, updir) < 0.0f) {
                right_vec = Float3{-right_vec.x, -right_vec.y, -right_vec.z};
                up_vec = Float3{-up_vec.x, -up_vec.y, -up_vec.z};
            }
            forward = forward_dir;
            wf::Vec3 eye_v{eye.x, eye.y, eye.z};
            wf::Vec3 center{eye_v.x + forward.x, eye_v.y + forward.y, eye_v.z + forward.z};
            wf::Vec3 up_v{up_vec.x, up_vec.y, up_vec.z};
            V = wf::look_at_rh(eye_v, center, up_v);
        }

        forward = wf::normalize(forward);
        right_vec = wf::normalize(right_vec);
        up_vec = wf::normalize(up_vec);

        auto MVP = wf::mul(P, V);

        if (debug_chunk_keys_) {
            static bool logged_clip = false;
            if (!logged_clip) {
                std::cout << "VP matrix:\n";
                for (int r = 0; r < 4; ++r) {
                    std::cout << "  ["
                              << MVP.at(r, 0) << ", "
                              << MVP.at(r, 1) << ", "
                              << MVP.at(r, 2) << ", "
                              << MVP.at(r, 3) << "]\n";
                }
                if (!render_chunks_.empty()) {
                    const auto& rc0 = render_chunks_[0];
                    wf::Vec4 c0{rc0.center[0], rc0.center[1], rc0.center[2], 1.0f};
                    wf::Vec4 view = wf::mul(V, c0);
                    auto clip = wf::mul(P, view);
                    std::cout << "eye:" << eye.x << "," << eye.y << "," << eye.z
                              << "  forward:" << forward.x << "," << forward.y << "," << forward.z << "\n";
                    std::cout << "chunk0 center:" << rc0.center[0] << "," << rc0.center[1] << "," << rc0.center[2]
                              << " radius:" << rc0.radius << "\n";
                    std::cout << "delta:" << (rc0.center[0]-eye.x) << "," << (rc0.center[1]-eye.y) << "," << (rc0.center[2]-eye.z) << "\n";
                    std::cout << "view-space:" << view.x << "," << view.y << "," << view.z << "," << view.w << "\n";
                    std::cout << "clip test: "
                              << clip.x << ", "
                              << clip.y << ", "
                              << clip.z << ", "
                              << clip.w << "\n";
                }
                logged_clip = true;
            }
        }

        // Prepare preallocated container and compute draw stats
        chunk_items_tmp_.clear();
        chunk_items_tmp_.reserve(render_chunks_.size());
        last_draw_total_ = (int)render_chunks_.size();
        last_draw_visible_ = 0;
        last_draw_indices_ = 0;

        if (cull_enabled_) {
            const float deg_to_rad = 0.01745329252f;
            Float3 fwd_n = wf::normalize(forward);
            Float3 up_n = wf::normalize(up_vec);
            Float3 right_n = wf::normalize(right_vec);

            float tan_y = std::tan(0.5f * fov_deg_ * deg_to_rad);
            float tan_x = tan_y * aspect;

            Float3 plane_right = Float3{fwd_n.x * tan_x - right_n.x,
                                        fwd_n.y * tan_x - right_n.y,
                                        fwd_n.z * tan_x - right_n.z};
            Float3 plane_left  = Float3{fwd_n.x * tan_x + right_n.x,
                                        fwd_n.y * tan_x + right_n.y,
                                        fwd_n.z * tan_x + right_n.z};
            Float3 plane_top   = Float3{fwd_n.x * tan_y - up_n.x,
                                        fwd_n.y * tan_y - up_n.y,
                                        fwd_n.z * tan_y - up_n.z};
            Float3 plane_bottom= Float3{fwd_n.x * tan_y + up_n.x,
                                        fwd_n.y * tan_y + up_n.y,
                                        fwd_n.z * tan_y + up_n.z};

            float plane_side_norm = wf::length(plane_right);
            float plane_vert_norm = wf::length(plane_top);

            for (const auto& rc : render_chunks_) {
                float dx = rc.center[0] - eye.x;
                float dy = rc.center[1] - eye.y;
                float dz = rc.center[2] - eye.z;
                Float3 delta{dx, dy, dz};
                float dist_f = dx*fwd_n.x + dy*fwd_n.y + dz*fwd_n.z;
                // near/far
                if (dist_f + rc.radius < near_m_) continue;
                if (dist_f - rc.radius > far_m_) continue;

                float dist_right = delta.x * plane_right.x + delta.y * plane_right.y + delta.z * plane_right.z;
                if (dist_right < -rc.radius * plane_side_norm) continue;
                float dist_left = delta.x * plane_left.x + delta.y * plane_left.y + delta.z * plane_left.z;
                if (dist_left < -rc.radius * plane_side_norm) continue;
                float dist_top = delta.x * plane_top.x + delta.y * plane_top.y + delta.z * plane_top.z;
                if (dist_top < -rc.radius * plane_vert_norm) continue;
                float dist_bottom = delta.x * plane_bottom.x + delta.y * plane_bottom.y + delta.z * plane_bottom.z;
                if (dist_bottom < -rc.radius * plane_vert_norm) continue;
                ChunkDrawItem item{};
                item.vbuf = rc.vbuf; item.ibuf = rc.ibuf; item.index_count = rc.index_count;
                item.first_index = rc.first_index; item.base_vertex = rc.base_vertex;
                item.vertex_count = rc.vertex_count;
                item.center[0] = rc.center[0]; item.center[1] = rc.center[1]; item.center[2] = rc.center[2];
                item.radius = rc.radius;
                chunk_items_tmp_.push_back(item);
                last_draw_visible_++;
                last_draw_indices_ += rc.index_count;
            }
        } else {
            for (const auto& rc : render_chunks_) {
                ChunkDrawItem item{};
                item.vbuf = rc.vbuf; item.ibuf = rc.ibuf; item.index_count = rc.index_count;
                item.first_index = rc.first_index; item.base_vertex = rc.base_vertex;
                item.vertex_count = rc.vertex_count;
                item.center[0] = rc.center[0]; item.center[1] = rc.center[1]; item.center[2] = rc.center[2];
                item.radius = rc.radius;
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

    double cursor_x = 0.0;
    double cursor_y = 0.0;
    glfwGetCursorPos(window_, &cursor_x, &cursor_y);

    // Mouse look when RMB is held
    int rmb = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT);
    if (rmb == GLFW_PRESS) {
        if (!rmb_down_) {
            rmb_down_ = true;
            // Enable cursor-disabled/raw mode for consistent relative deltas
            set_mouse_capture(true);
            // Reset deltas to avoid an initial jump
            last_cursor_x_ = cursor_x; last_cursor_y_ = cursor_y;
        }
        double dx = cursor_x - last_cursor_x_;
        double dy = cursor_y - last_cursor_y_;
        last_cursor_x_ = cursor_x; last_cursor_y_ = cursor_y;
        float sx = invert_mouse_x_ ? -1.0f : 1.0f;
        float sy = invert_mouse_y_ ?  1.0f : -1.0f;
        float yaw_delta = sx * (float)(dx * cam_sensitivity_);
        float pitch_delta = sy * (float)(dy * cam_sensitivity_);
        if (!walk_mode_) {
            cam_yaw_ += yaw_delta;
            if (cam_yaw_ > 3.14159265f) cam_yaw_ -= 6.28318531f;
            if (cam_yaw_ < -3.14159265f) cam_yaw_ += 6.28318531f;
            cam_pitch_ += pitch_delta;
            float maxp = 1.55334306f; // ~89 deg
            cam_pitch_ = std::clamp(cam_pitch_, -maxp, maxp);
        } else {
            Float3 pos{static_cast<float>(cam_pos_[0]),
                       static_cast<float>(cam_pos_[1]),
                       static_cast<float>(cam_pos_[2])};
            Float3 updir = wf::normalize(pos);
            auto normalize_or = [](Float3 v, Float3 fallback) {
                float len = wf::length(v);
                return (len > 1e-5f) ? (v / len) : fallback;
            };
            auto cross3 = [](const Float3& a, const Float3& b) {
                return Float3{
                    a.y * b.z - a.z * b.y,
                    a.z * b.x - a.x * b.z,
                    a.x * b.y - a.y * b.x
                };
            };
            auto dot3 = [](const Float3& a, const Float3& b) {
                return a.x * b.x + a.y * b.y + a.z * b.z;
            };
            auto rotate_axis = [&](const Float3& v, const Float3& axis, float angle) {
                Float3 n = normalize_or(axis, Float3{0.0f, 1.0f, 0.0f});
                float c = std::cos(angle);
                float s = std::sin(angle);
                float dot = dot3(n, v);
                Float3 cross_nv = cross3(n, v);
                return Float3{
                    v.x * c + cross_nv.x * s + n.x * dot * (1.0f - c),
                    v.y * c + cross_nv.y * s + n.y * dot * (1.0f - c),
                    v.z * c + cross_nv.z * s + n.z * dot * (1.0f - c)
                };
            };

            Float3 forward = normalize_or(Float3{
                std::cos(cam_pitch_) * std::cos(cam_yaw_),
                std::sin(cam_pitch_),
                std::cos(cam_pitch_) * std::sin(cam_yaw_)
            }, Float3{1.0f, 0.0f, 0.0f});

            if (yaw_delta != 0.0f) {
                forward = rotate_axis(forward, updir, yaw_delta);
                forward = wf::normalize(forward);
            }

            if (pitch_delta != 0.0f) {
                Float3 right_axis = normalize_or(cross3(updir, forward), Float3{0.0f, 1.0f, 0.0f});
                Float3 candidate = rotate_axis(forward, right_axis, pitch_delta);
                candidate = wf::normalize(candidate);
                float sin_pitch = std::clamp(dot3(candidate, updir), -1.0f, 1.0f);
                float max_pitch = walk_pitch_max_deg_ * 0.01745329252f;
                float max_s = std::sin(max_pitch);
                if (sin_pitch > max_s || sin_pitch < -max_s) {
                    float clamped = std::clamp(sin_pitch, -max_s, max_s);
                    Float3 tangent = normalize_or(candidate - updir * sin_pitch, forward);
                    float tangent_scale = std::sqrt(std::max(0.0f, 1.0f - clamped * clamped));
                    candidate = wf::normalize(tangent * tangent_scale + updir * clamped);
                }
                forward = candidate;
            }

            cam_yaw_ = std::atan2(forward.z, forward.x);
            cam_pitch_ = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
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
            Float3 pos{static_cast<float>(cam_pos_[0]),
                       static_cast<float>(cam_pos_[1]),
                       static_cast<float>(cam_pos_[2])};
            Float3 updir = wf::normalize(pos);
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
            float maxp = walk_pitch_max_deg_ * 0.01745329252f;
            cam_pitch_ = std::clamp(cam_pitch_, -maxp, maxp);
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
        // Walk mode: move along surface using projected camera orientation
        const PlanetConfig& cfg = planet_cfg_;
        Float3 pos{(float)cam_pos_[0], (float)cam_pos_[1], (float)cam_pos_[2]};
        Float3 updir = normalize(pos);
        auto cross3 = [](Float3 a, Float3 b) {
            return Float3{ a.y * b.z - a.z * b.y,
                           a.z * b.x - a.x * b.z,
                           a.x * b.y - a.y * b.x };
        };
        auto normalize_or = [](Float3 v, Float3 fallback) {
            float len = wf::length(v);
            return (len > 1e-5f) ? (v / len) : fallback;
        };
        auto dot3 = [](Float3 a, Float3 b) {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        };

        Float3 view_dir{cp * cyaw, sp, cp * syaw};
        view_dir = normalize_or(view_dir, Float3{1.0f, 0.0f, 0.0f});
        Float3 fwd_t = view_dir - updir * dot3(view_dir, updir);
        fwd_t = normalize_or(fwd_t, normalize_or(cross3(updir, Float3{0.0f, 0.0f, 1.0f}), Float3{1.0f, 0.0f, 0.0f}));
        Float3 right_t = normalize_or(cross3(fwd_t, updir), Float3{0.0f, 1.0f, 0.0f});

        float speed = walk_speed_ * dt * (glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 2.0f : 1.0f);
        Float3 step{0,0,0};
        if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS) { step = step + fwd_t   * speed; }
        if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS) { step = step - fwd_t   * speed; }
        if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS) { step = step - right_t * speed; }
        if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS) { step = step + right_t * speed; }

        Float3 ndir = updir;
        float step_len = wf::length(step);
        if (step_len > 0.0f) {
            Float3 tdir = step / step_len;
            double cam_rd = std::sqrt(cam_pos_[0]*cam_pos_[0] + cam_pos_[1]*cam_pos_[1] + cam_pos_[2]*cam_pos_[2]);
            float phi = (float)(step_len / std::max(cam_rd, 1e-9));
            float c = std::cos(phi);
            float s = std::sin(phi);
            ndir = wf::normalize(Float3{ updir.x * c + tdir.x * s,
                                         updir.y * c + tdir.y * s,
                                         updir.z * c + tdir.z * s });
            updir = ndir;
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

    // Feed UI backend
    ui::UIBackend::InputState ui_input{};
    glfwGetCursorPos(window_, &cursor_x, &cursor_y);
    ui_input.mouse_x = cursor_x;
    ui_input.mouse_y = cursor_y;
    ui_input.mouse_down[0] = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    ui_input.mouse_down[1] = (rmb == GLFW_PRESS);
    ui_input.mouse_down[2] = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    ui_input.has_mouse = (glfwGetWindowAttrib(window_, GLFW_FOCUSED) == GLFW_TRUE) && !mouse_captured_;
    hud_ui_backend_.begin_frame(ui_input, hud_ui_frame_index_++);
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
    }
}

void VulkanApp::load_config() {
    AppConfig defaults = snapshot_config();
    AppConfig loaded = load_app_config(defaults, config_path_override_);
    apply_config(loaded);
}

AppConfig VulkanApp::snapshot_config() const {
    AppConfig cfg;
    cfg.invert_mouse_x = invert_mouse_x_;
    cfg.invert_mouse_y = invert_mouse_y_;
    cfg.cam_sensitivity = cam_sensitivity_;
    cfg.cam_speed = cam_speed_;
    cfg.fov_deg = fov_deg_;
    cfg.near_m = near_m_;
    cfg.far_m = far_m_;

    cfg.walk_mode = walk_mode_;
    cfg.eye_height_m = eye_height_m_;
    cfg.walk_speed = walk_speed_;
    cfg.walk_pitch_max_deg = walk_pitch_max_deg_;
    cfg.walk_surface_bias_m = walk_surface_bias_m_;
    cfg.surface_push_m = surface_push_m_;

    cfg.use_chunk_renderer = use_chunk_renderer_;
    cfg.ring_radius = ring_radius_;
    cfg.prune_margin = prune_margin_;
    cfg.cull_enabled = cull_enabled_;
    cfg.draw_stats_enabled = draw_stats_enabled_;

    cfg.hud_scale = hud_scale_;
    cfg.hud_shadow = hud_shadow_enabled_;
    cfg.hud_shadow_offset_px = hud_shadow_offset_px_;

    cfg.log_stream = log_stream_;
    cfg.log_pool = log_pool_;
    cfg.save_chunks_enabled = save_chunks_enabled_;
    cfg.debug_chunk_keys = debug_chunk_keys_;

    cfg.profile_csv_enabled = profile_csv_enabled_;
    cfg.profile_csv_path = profile_csv_path_;

    cfg.device_local_enabled = device_local_enabled_;
    cfg.pool_vtx_mb = pool_vtx_mb_;
    cfg.pool_idx_mb = pool_idx_mb_;

    cfg.uploads_per_frame_limit = uploads_per_frame_limit_;
    cfg.loader_threads = loader_threads_;
    cfg.k_down = k_down_;
    cfg.k_up = k_up_;
    cfg.k_prune_margin = k_prune_margin_;
    cfg.face_keep_time_cfg_s = face_keep_time_cfg_s_;

    cfg.planet_cfg = planet_cfg_;
    cfg.config_path = config_path_used_;
    cfg.region_root = region_root_;

    return cfg;
}

void VulkanApp::apply_config(const AppConfig& cfg) {
    invert_mouse_x_ = cfg.invert_mouse_x;
    invert_mouse_y_ = cfg.invert_mouse_y;
    cam_sensitivity_ = cfg.cam_sensitivity;
    cam_speed_ = cfg.cam_speed;
    fov_deg_ = cfg.fov_deg;
    near_m_ = cfg.near_m;
    far_m_ = cfg.far_m;

    walk_mode_ = cfg.walk_mode;
    eye_height_m_ = cfg.eye_height_m;
    walk_speed_ = cfg.walk_speed;
    walk_pitch_max_deg_ = cfg.walk_pitch_max_deg;
    walk_surface_bias_m_ = cfg.walk_surface_bias_m;
    surface_push_m_ = cfg.surface_push_m;

    use_chunk_renderer_ = cfg.use_chunk_renderer;
    ring_radius_ = cfg.ring_radius;
    prune_margin_ = cfg.prune_margin;
    cull_enabled_ = cfg.cull_enabled;
    draw_stats_enabled_ = cfg.draw_stats_enabled;

    hud_scale_ = cfg.hud_scale;
    hud_shadow_enabled_ = cfg.hud_shadow;
    hud_shadow_offset_px_ = cfg.hud_shadow_offset_px;

    log_stream_ = cfg.log_stream;
    log_pool_ = cfg.log_pool;
    save_chunks_enabled_ = cfg.save_chunks_enabled;
    debug_chunk_keys_ = cfg.debug_chunk_keys;

    profile_csv_enabled_ = cfg.profile_csv_enabled;
    profile_csv_path_ = cfg.profile_csv_path;

    device_local_enabled_ = cfg.device_local_enabled;
    pool_vtx_mb_ = cfg.pool_vtx_mb;
    pool_idx_mb_ = cfg.pool_idx_mb;

    uploads_per_frame_limit_ = cfg.uploads_per_frame_limit;
    loader_threads_ = cfg.loader_threads;
    k_down_ = cfg.k_down;
    k_up_ = cfg.k_up;
    k_prune_margin_ = cfg.k_prune_margin;
    face_keep_time_cfg_s_ = cfg.face_keep_time_cfg_s;

    planet_cfg_ = cfg.planet_cfg;
    config_path_used_ = cfg.config_path;
    region_root_ = cfg.region_root;

    std::cout << "[config] region_root=" << region_root_ << " (active)\n";
    std::cout << "[config] debug_chunk_keys=" << (debug_chunk_keys_ ? "true" : "false") << " (active)\n";

    hud_force_refresh_ = true;
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
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { hud_ui_backend_.end_frame(); recreate_swapchain(); return; }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) throw_if_failed(acq, "vkAcquireNextImageKHR failed");

    overlay_draw_slot_ = current_frame_;
    ui::ContextParams ui_params;
    ui_params.screen_width = static_cast<int>(swapchain_extent_.width);
    ui_params.screen_height = static_cast<int>(swapchain_extent_.height);
    ui_params.style.scale = hud_scale_;
    ui_params.style.enable_shadow = hud_shadow_enabled_;
    ui_params.style.shadow_offset_px = hud_shadow_offset_px_;
    ui_params.style.shadow_color = ui::Color{0.0f, 0.0f, 0.0f, 0.6f};
    hud_ui_context_.begin(ui_params);

    ui::TextDrawParams text_params;
    text_params.scale = 1.0f;
    text_params.color = ui::Color{1.0f, 1.0f, 1.0f, 1.0f};
    text_params.line_spacing_px = 4.0f;
    float text_height = 0.0f;
    if (!hud_text_.empty()) {
        text_height = ui::add_text_block(hud_ui_context_, hud_text_.c_str(), ui_params.screen_width, text_params);
    }

    ui::ButtonStyle button_style;
    button_style.text_scale = 1.0f;
    ui::Rect button_rect{6.0f, text_params.origin_px.y + text_height + 8.0f, 136.0f, 18.0f};
    std::string button_label = std::string("Cull: ") + (cull_enabled_ ? "ON" : "OFF");
    if (ui::button(hud_ui_context_, hud_ui_backend_, ui::hash_id("hud.cull"), button_rect, button_label, button_style)) {
        cull_enabled_ = !cull_enabled_;
        hud_force_refresh_ = true;
    }

    ui::UIDrawData draw_data = hud_ui_context_.end();
    overlay_.upload_draw_data(overlay_draw_slot_, draw_data);

    vkResetCommandBuffer(command_buffers_[imageIndex], 0);
    record_command_buffer(command_buffers_[imageIndex], imageIndex);

    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &sem_image_available_[current_frame_];
    si.pWaitDstStageMask = waitStages;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &command_buffers_[imageIndex];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &sem_render_finished_[current_frame_];
    throw_if_failed(vkQueueSubmit(queue_graphics_, 1, &si, fences_in_flight_[current_frame_]), "vkQueueSubmit failed");

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &sem_render_finished_[current_frame_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;
    VkResult pres = vkQueuePresentKHR(queue_present_, &pi);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) { recreate_swapchain(); }
    else if (pres != VK_SUCCESS) throw_if_failed(pres, "vkQueuePresentKHR failed");

    hud_ui_backend_.end_frame();

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

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{}; vp.x = 0; vp.y = (float)swapchain_extent_.height; vp.width = (float)swapchain_extent_.width; vp.height = -(float)swapchain_extent_.height; vp.minDepth = 0; vp.maxDepth = 1;
    VkRect2D sc{}; sc.offset = {0,0}; sc.extent = swapchain_extent_;
    VkPipelineViewportStateCreateInfo vpstate{};
    vpstate.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpstate.viewportCount = 1;
    vpstate.pViewports = &vp;
    vpstate.scissorCount = 1;
    vpstate.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT; cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);
        return;
    }

    VkGraphicsPipelineCreateInfo gpi{};
    gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount = 2;
    gpi.pStages = stages;
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
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = cs;
    stage.pName = "main";

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &pipeline_layout_compute_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, cs, nullptr);
        return;
    }

    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
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
    stream_face_ready_ = false;
    pending_request_gen_ = gen;
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
    prev_face_ = -1;
    stream_face_ready_ = false;
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
                if (debug_chunk_keys_) {
                    static std::atomic<int> debug_chunk_log_count{0};
                    if (debug_chunk_log_count.load(std::memory_order_relaxed) < 32) {
                        int prev = debug_chunk_log_count.fetch_add(1, std::memory_order_relaxed);
                        if (prev < 32) {
                            std::cout << "[chunk-load] face=" << key.face
                                      << " i=" << key.i << " j=" << key.j << " k=" << key.k << "\n";
                        }
                    }
                }
                Chunk64& c = chunks[idx_of(di, dj, dk)];
                if (!RegionIO::load_chunk(key, c, 32, region_root_)) {
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
                    if (save_chunks_enabled_) RegionIO::save_chunk(key, c, 32, region_root_);
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
            if (!debug_chunk_keys_ && dcam < cone_cos) {
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
            res.job_gen = job_gen;
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
        if (!stream_face_ready_ && res.key.face == stream_face_ && res.job_gen >= pending_request_gen_) {
            stream_face_ready_ = true;
        }
        if (debug_chunk_keys_) {
            if (!res.vertices.empty()) {
                const auto &v0 = res.vertices.front();
                std::cout << "[mesh] key face=" << rc.key.face << " i=" << rc.key.i << " j=" << rc.key.j << " k=" << rc.key.k
                          << " v0=(" << v0.x << "," << v0.y << "," << v0.z << ")" << std::endl;
            }
            std::cout << "[mesh] center=(" << rc.center[0] << "," << rc.center[1] << "," << rc.center[2]
                      << ") radius=" << rc.radius << " idx=" << rc.index_count << " vtx=" << rc.vertex_count << std::endl;
        }
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
        if (debug_chunk_keys_ && !debug_auto_aim_done_) {
            Float3 eye{(float)cam_pos_[0], (float)cam_pos_[1], (float)cam_pos_[2]};
            Float3 center{rc.center[0], rc.center[1], rc.center[2]};
            Float3 dir = wf::normalize(center - eye);
            cam_yaw_ = std::atan2(dir.z, dir.x);
            cam_pitch_ = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
            debug_auto_aim_done_ = true;
            std::cout << "[debug-aim] yaw=" << cam_yaw_ << " pitch=" << cam_pitch_ << "\n";
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
    Float3 eye{static_cast<float>(cam_pos_[0]),
               static_cast<float>(cam_pos_[1]),
               static_cast<float>(cam_pos_[2])};
    Float3 dir = normalize(eye);
    int raw_face = face_from_direction(dir);
    int cur_face = raw_face;
    if (stream_face_ >= 0) {
        Float3 cur_right, cur_up, cur_forward;
        face_basis(stream_face_, cur_right, cur_up, cur_forward);
        float cur_align = std::fabs(dot(dir, cur_forward));
        if (raw_face != stream_face_) {
            Float3 cand_right, cand_up, cand_forward;
            face_basis(raw_face, cand_right, cand_up, cand_forward);
            float cand_align = std::fabs(dot(dir, cand_forward));
            if (cand_align < cur_align + face_switch_hysteresis_) {
                cur_face = stream_face_;
            }
        }
    }
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
    if (!stream_face_ready_) {
        std::lock_guard<std::mutex> lk(loader_mutex_);
        if (!loader_busy_ && request_queue_.empty()) {
            stream_face_ready_ = true;
        }
    }

    std::vector<AllowRegion> allows;
    int base_span = ring_radius_ + prune_margin_;
    int base_k_down = k_down_ + k_prune_margin_;
    int base_k_up = k_up_ + k_prune_margin_;
    if (!stream_face_ready_) {
        base_span += 1;
        base_k_down += 1;
        base_k_up += 1;
    }
    allows.push_back(AllowRegion{stream_face_, ring_center_i_, ring_center_j_, ring_center_k_, base_span, base_k_down, base_k_up});
    bool keep_prev = (prev_face_ >= 0) && (face_keep_timer_s_ > 0.0f || !stream_face_ready_);
    if (keep_prev) {
        allows.push_back(AllowRegion{prev_face_, prev_center_i_, prev_center_j_, prev_center_k_, base_span, base_k_down, base_k_up});
    }
    prune_chunks_multi(allows);
}
}
