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
    vkDeviceWaitIdle(device_);

    cleanup_swapchain();

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
    init_vulkan();
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        draw_frame();
    }
}

void VulkanApp::init_window() {
    if (!glfwInit()) { std::abort(); }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window_ = glfwCreateWindow(width_, height_, "Wanderforge", nullptr, nullptr);
}

void VulkanApp::init_vulkan() {
    create_instance();
    setup_debug_messenger();
    create_surface();
    pick_physical_device();
    create_logical_device();
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
    create_graphics_pipeline();
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
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1; rpci.pAttachments = &color;
    rpci.subpassCount = 1; rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1; rpci.pDependencies = &dep;
    throw_if_failed(vkCreateRenderPass(device_, &rpci, nullptr, &render_pass_), "vkCreateRenderPass failed");
}

void VulkanApp::create_framebuffers() {
    framebuffers_.resize(swapchain_image_views_.size());
    for (size_t i=0;i<swapchain_image_views_.size();++i) {
        VkImageView attachments[] = { swapchain_image_views_[i] };
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = render_pass_;
        fci.attachmentCount = 1; fci.pAttachments = attachments;
        fci.width = swapchain_extent_.width; fci.height = swapchain_extent_.height; fci.layers = 1;
        throw_if_failed(vkCreateFramebuffer(device_, &fci, nullptr, &framebuffers_[i]), "vkCreateFramebuffer failed");
    }
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

    VkClearValue clear{ { {0.02f, 0.02f, 0.06f, 1.0f} } };
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = render_pass_;
    rbi.framebuffer = framebuffers_[imageIndex];
    rbi.renderArea.offset = {0,0}; rbi.renderArea.extent = swapchain_extent_;
    rbi.clearValueCount = 1; rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    if (pipeline_triangle_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_triangle_);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd);

    throw_if_failed(vkEndCommandBuffer(cmd), "vkEndCommandBuffer failed");
}

void VulkanApp::draw_frame() {
    vkWaitForFences(device_, 1, &fences_in_flight_[current_frame_], VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &fences_in_flight_[current_frame_]);

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, sem_image_available_[current_frame_], VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(); return; }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) throw_if_failed(acq, "vkAcquireNextImageKHR failed");

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
    for (auto fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    framebuffers_.clear();
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
    create_graphics_pipeline();
    create_framebuffers();
}

VkShaderModule VulkanApp::load_shader_module(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return VK_NULL_HANDLE;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<char> buf(len);
    if (fread(buf.data(), 1, buf.size(), f) != buf.size()) { fclose(f); return VK_NULL_HANDLE; }
    fclose(f);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = buf.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(buf.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &ci, nullptr, &mod) != VK_SUCCESS) return VK_NULL_HANDLE;
    return mod;
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
        std::cerr << "Shaders not found (" << vsPath << ", " << fsPath << "). Triangle disabled.\n";
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

    VkViewport vp{}; vp.x = 0; vp.y = 0; vp.width = (float)swapchain_extent_.width; vp.height = (float)swapchain_extent_.height; vp.minDepth = 0; vp.maxDepth = 1;
    VkRect2D sc{}; sc.offset = {0,0}; sc.extent = swapchain_extent_;
    VkPipelineViewportStateCreateInfo vpstate{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vpstate.viewportCount = 1; vpstate.pViewports = &vp; vpstate.scissorCount = 1; vpstate.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_BACK_BIT; rs.frontFace = VK_FRONT_FACE_CLOCKWISE; rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT; cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;

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

} // namespace wf
