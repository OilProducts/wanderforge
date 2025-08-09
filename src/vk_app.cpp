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

#include "chunk.h"
#include "mesh.h"
#include "planet.h"
#include "region_io.h"

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

    for (auto& rc : render_chunks_) {
        if (rc.vbuf) vkDestroyBuffer(device_, rc.vbuf, nullptr);
        if (rc.vmem) vkFreeMemory(device_, rc.vmem, nullptr);
        if (rc.ibuf) vkDestroyBuffer(device_, rc.ibuf, nullptr);
        if (rc.imem) vkFreeMemory(device_, rc.imem, nullptr);
    }

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
    init_vulkan();
    last_time_ = glfwGetTime();
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        double now = glfwGetTime();
        float dt = (float)std::max(0.0, now - last_time_);
        last_time_ = now;
        update_input(dt);
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
    create_graphics_pipeline_chunk();
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

    // Load or generate a small grid of chunks via Region IO and upload them
    {
        PlanetConfig cfg;
        const int face = 0; // +X face
        const int tile_span = 1; // loads (2*tile_span+1)^2 chunks centered at (0,0)
        const int N = Chunk64::N;
        const float s = (float)cfg.voxel_size_m;
        const double chunk_m = (double)N * cfg.voxel_size_m;
        const std::int64_t k0 = (std::int64_t)std::floor(cfg.radius_m / chunk_m);
        Float3 right, up, forward; face_basis(face, right, up, forward);

        for (int dj = -tile_span; dj <= tile_span; ++dj) {
            for (int di = -tile_span; di <= tile_span; ++di) {
                FaceChunkKey key{face, di, dj, k0};
                Chunk64 c;
                if (!RegionIO::load_chunk(key, c)) {
                    // Generate from base sampler
                    for (int z = 0; z < N; ++z) {
                        for (int y = 0; y < N; ++y) {
                            for (int x = 0; x < N; ++x) {
                                double s0 = (double)di * chunk_m + (x + 0.5) * cfg.voxel_size_m;
                                double t0 = (double)dj * chunk_m + (y + 0.5) * cfg.voxel_size_m;
                                double r0 = (double)k0 * chunk_m + (z + 0.5) * cfg.voxel_size_m;
                                Float3 p = right * (float)s0 + up * (float)t0 + forward * (float)r0;
                                Int3 v{ (i64)std::llround(p.x / cfg.voxel_size_m), (i64)std::llround(p.y / cfg.voxel_size_m), (i64)std::llround(p.z / cfg.voxel_size_m) };
                                auto sb = sample_base(cfg, v);
                                c.set_voxel(x, y, z, sb.material);
                            }
                        }
                    }
                    RegionIO::save_chunk(key, c);
                }

                Mesh m;
                mesh_chunk_greedy(c, m, s);
                if (!m.indices.empty()) {
                    // Transform vertices from face-local chunk to world space
                    float S0 = (float)(di * chunk_m);
                    float T0 = (float)(dj * chunk_m);
                    float R0 = (float)(k0 * chunk_m);
                    for (auto& vert : m.vertices) {
                        Float3 lp{vert.x, vert.y, vert.z};
                        Float3 wp = right * (S0 + lp.x) + up * (T0 + lp.y) + forward * (R0 + lp.z);
                        Float3 ln{vert.nx, vert.ny, vert.nz};
                        Float3 wn = right * ln.x + up * ln.y + forward * ln.z;
                        vert.x = wp.x; vert.y = wp.y; vert.z = wp.z;
                        vert.nx = wn.x; vert.ny = wn.y; vert.nz = wn.z;
                    }
                    RenderChunk rc;
                    rc.index_count = (uint32_t)m.indices.size();
                    create_host_buffer(sizeof(Vertex) * m.vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, rc.vbuf, rc.vmem, m.vertices.data());
                    create_host_buffer(sizeof(uint32_t) * m.indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, rc.ibuf, rc.imem, m.indices.data());
                    render_chunks_.push_back(rc);
                }
            }
        }

        // Place camera slightly outside the loaded shell, looking inward
        {
            float R0f = (float)(k0 * chunk_m);
            Float3 pos = forward * (R0f + 10.0f) + up * 4.0f; // 10m radially outward, 4m up
            cam_pos_[0] = pos.x; cam_pos_[1] = pos.y; cam_pos_[2] = pos.z;
            cam_yaw_ = 3.14159265f;   // look toward -forward (inward)
            cam_pitch_ = -0.05f;
        }
    }
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
    uint32_t mt = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
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
    if (pipeline_chunk_ && !render_chunks_.empty()) {
        // Push a simple MVP matrix
        float aspect = (float)swapchain_extent_.width / (float)swapchain_extent_.height;
        float fov = 60.0f * 0.01745329252f;
        float zn = 0.1f, zf = 1000.0f;
        float f = 1.0f / std::tan(fov * 0.5f);
        float P[16] = { f/aspect,0,0,0,  0,f,0,0,  0,0,(zf+zn)/(zn-zf),-1,  0,0,(2*zf*zn)/(zn-zf),0 };
        // Camera look-at from yaw/pitch/pos
        auto norm3=[&](std::array<float,3> v){ float l=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); return std::array<float,3>{v[0]/l,v[1]/l,v[2]/l}; };
        auto cross=[&](std::array<float,3> a,std::array<float,3> b){return std::array<float,3>{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};};
        float cy = std::cos(cam_yaw_), sy = std::sin(cam_yaw_);
        float cp = std::cos(cam_pitch_), sp = std::sin(cam_pitch_);
        std::array<float,3> fwd = { cp*cy, sp, cp*sy };
        std::array<float,3> upv = { 0.0f, 1.0f, 0.0f };
        auto s = norm3(cross(fwd, upv));
        auto u = cross(s, fwd);
        float eye[3] = { cam_pos_[0], cam_pos_[1], cam_pos_[2] };
        float V[16] = { s[0], u[0], -fwd[0], 0,
                        s[1], u[1], -fwd[1], 0,
                        s[2], u[2], -fwd[2], 0,
                        -(s[0]*eye[0]+s[1]*eye[1]+s[2]*eye[2]),
                        -(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]),
                        fwd[0]*eye[0]+fwd[1]*eye[1]+fwd[2]*eye[2], 1 };
        auto mul4=[&](const float A[16], const float B[16]){
            float R[16]{};
            for(int r=0;r<4;++r) for(int c=0;c<4;++c){ R[r*4+c]=A[r*4+0]*B[0*4+c]+A[r*4+1]*B[1*4+c]+A[r*4+2]*B[2*4+c]+A[r*4+3]*B[3*4+c]; }
            return std::array<float,16>{R[0],R[1],R[2],R[3],R[4],R[5],R[6],R[7],R[8],R[9],R[10],R[11],R[12],R[13],R[14],R[15]}; };
        auto MVP = mul4(V, P);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_chunk_);
        vkCmdPushConstants(cmd, pipeline_layout_chunk_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float)*16, MVP.data());
        for (const auto& rc : render_chunks_) {
            VkDeviceSize offs = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &rc.vbuf, &offs);
            vkCmdBindIndexBuffer(cmd, rc.ibuf, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, rc.index_count, 1, 0, 0, 0);
        }
    } else if (pipeline_triangle_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_triangle_);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
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
        if (!rmb_down_) { rmb_down_ = true; last_cursor_x_ = cx; last_cursor_y_ = cy; }
        double dx = cx - last_cursor_x_;
        double dy = cy - last_cursor_y_;
        last_cursor_x_ = cx; last_cursor_y_ = cy;
        cam_yaw_   += (float)(dx * cam_sensitivity_);
        cam_pitch_ -= (float)(dy * cam_sensitivity_);
        const float maxp = 1.55334306f; // ~89 deg
        if (cam_pitch_ > maxp) cam_pitch_ = maxp; if (cam_pitch_ < -maxp) cam_pitch_ = -maxp;
    } else {
        rmb_down_ = false;
    }

    // Compute basis from yaw/pitch
    float cyaw = std::cos(cam_yaw_), syaw = std::sin(cam_yaw_);
    float cp = std::cos(cam_pitch_), sp = std::sin(cam_pitch_);
    float fwd[3] = { cp*cyaw, sp, cp*syaw };
    float up[3]  = { 0.0f, 1.0f, 0.0f };
    float right[3] = { up[1]*fwd[2]-up[2]*fwd[1], up[2]*fwd[0]-up[0]*fwd[2], up[0]*fwd[1]-up[1]*fwd[0] };
    float rl = std::sqrt(right[0]*right[0]+right[1]*right[1]+right[2]*right[2]);
    if (rl > 0) { right[0]/=rl; right[1]/=rl; right[2]/=rl; }

    float speed = cam_speed_ * dt * (glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 3.0f : 1.0f);
    if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS) { cam_pos_[0]+=fwd[0]*speed; cam_pos_[1]+=fwd[1]*speed; cam_pos_[2]+=fwd[2]*speed; }
    if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS) { cam_pos_[0]-=fwd[0]*speed; cam_pos_[1]-=fwd[1]*speed; cam_pos_[2]-=fwd[2]*speed; }
    if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS) { cam_pos_[0]-=right[0]*speed; cam_pos_[1]-=right[1]*speed; cam_pos_[2]-=right[2]*speed; }
    if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS) { cam_pos_[0]+=right[0]*speed; cam_pos_[1]+=right[1]*speed; cam_pos_[2]+=right[2]*speed; }
    if (glfwGetKey(window_, GLFW_KEY_Q) == GLFW_PRESS) { cam_pos_[1]-=speed; }
    if (glfwGetKey(window_, GLFW_KEY_E) == GLFW_PRESS) { cam_pos_[1]+=speed; }
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
    if (pipeline_chunk_) { vkDestroyPipeline(device_, pipeline_chunk_, nullptr); pipeline_chunk_ = VK_NULL_HANDLE; }
    if (pipeline_layout_chunk_) { vkDestroyPipelineLayout(device_, pipeline_layout_chunk_, nullptr); pipeline_layout_chunk_ = VK_NULL_HANDLE; }
    if (pipeline_triangle_) { vkDestroyPipeline(device_, pipeline_triangle_, nullptr); pipeline_triangle_ = VK_NULL_HANDLE; }
    if (pipeline_layout_) { vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr); pipeline_layout_ = VK_NULL_HANDLE; }
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
    create_graphics_pipeline_chunk();
    create_graphics_pipeline();
    create_framebuffers();
}

static bool read_file_all(const std::string& p, std::vector<char>& out) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(len);
    size_t rd = fread(out.data(), 1, out.size(), f);
    fclose(f);
    return rd == out.size();
}

VkShaderModule VulkanApp::load_shader_module(const std::string& path) {
    std::vector<char> buf;
    // Try provided path
    if (!read_file_all(path, buf)) {
        // Fallbacks: try common relative paths
        size_t slash = path.find_last_of('/');
        std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
        const char* candidates[] = {
            (std::string("build/shaders/") + base).c_str(),
            (std::string("shaders/") + base).c_str(),
            (std::string("shaders_build/") + base).c_str()
        };
        bool ok = false;
        for (const char* cand : candidates) {
            if (read_file_all(cand, buf)) { ok = true; break; }
        }
        if (!ok) return VK_NULL_HANDLE;
    }
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

void VulkanApp::create_graphics_pipeline_chunk() {
    // Vertex-input pipeline for chunk rendering
    const std::string base = std::string(WF_SHADER_DIR);
    const std::string vsPath = base + "/chunk.vert.spv";
    const std::string fsPath = base + "/chunk.frag.spv";
    VkShaderModule vs = load_shader_module(vsPath);
    VkShaderModule fs = load_shader_module(fsPath);
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(device_, vs, nullptr);
        if (fs) vkDestroyShaderModule(device_, fs, nullptr);
        std::cout << "[info] Chunk shaders not found. Chunk rendering disabled." << std::endl;
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

    // Only position attribute used; stride is sizeof(Vertex) from mesh.h (x,y,z,...)
    VkVertexInputBindingDescription bind{};
    bind.binding = 0; bind.stride = sizeof(float)*7; bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0; attrs[0].binding = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = 0;   // position
    attrs[1].location = 1; attrs[1].binding = 0; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[1].offset = 12;  // normal
    attrs[2].location = 2; attrs[2].binding = 0; attrs[2].format = VK_FORMAT_R16_UINT;          attrs[2].offset = 24;  // material id
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 3; vi.pVertexAttributeDescriptions = attrs;

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
    // Depth state
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    // Push constant for MVP matrix
    VkPushConstantRange pcr{}; pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT; pcr.offset = 0; pcr.size = sizeof(float)*16;
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &pipeline_layout_chunk_) != VK_SUCCESS) {
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
    gpi.pDepthStencilState = &ds;
    gpi.pColorBlendState = &cb;
    gpi.layout = pipeline_layout_chunk_;
    gpi.renderPass = render_pass_;
    gpi.subpass = 0;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline_chunk_) != VK_SUCCESS) {
        std::cerr << "Failed to create chunk graphics pipeline.\n";
        vkDestroyPipelineLayout(device_, pipeline_layout_chunk_, nullptr); pipeline_layout_chunk_ = VK_NULL_HANDLE;
    }
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
}
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
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) return i;
    }
    return UINT32_MAX;
}

void VulkanApp::create_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf, VkDeviceMemory& mem, const void* data) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size; bci.usage = usage; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    throw_if_failed(vkCreateBuffer(device_, &bci, nullptr, &buf), "vkCreateBuffer failed");
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device_, buf, &req);
    uint32_t mt = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) {
        std::cerr << "No host-visible memory type for buffers" << std::endl; std::abort();
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
    throw_if_failed(vkAllocateMemory(device_, &mai, nullptr, &mem), "vkAllocateMemory failed");
    throw_if_failed(vkBindBufferMemory(device_, buf, mem, 0), "vkBindBufferMemory failed");
    if (data) {
        void* p = nullptr; vkMapMemory(device_, mem, 0, size, 0, &p);
        std::memcpy(p, data, (size_t)size);
        vkUnmapMemory(device_, mem);
    }
}

} // namespace wf
