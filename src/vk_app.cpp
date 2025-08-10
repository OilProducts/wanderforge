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

#include "chunk.h"
#include "mesh.h"
#include "planet.h"
#include "region_io.h"
#include "vk_utils.h"

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
#ifdef WF_DEBUG
    enable_validation_ = true;
#else
    enable_validation_ = false;
#endif
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
    // ChunkRenderer encapsulates the chunk pipeline now
    create_graphics_pipeline();
#include "wf_config.h"
    overlay_.init(physical_device_, device_, render_pass_, swapchain_extent_, WF_SHADER_DIR);
    chunk_renderer_.init(device_, render_pass_, swapchain_extent_, WF_SHADER_DIR);
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

        const int W = 2*tile_span + 1;
        std::vector<Chunk64> chunks(W * W);
        auto idx_of = [&](int di, int dj){ int ix = di + tile_span; int jy = dj + tile_span; return jy * W + ix; };
        // First pass: load or generate all chunks
        for (int dj = -tile_span; dj <= tile_span; ++dj) {
            for (int di = -tile_span; di <= tile_span; ++di) {
                FaceChunkKey key{face, di, dj, k0};
                Chunk64& c = chunks[idx_of(di, dj)];
                if (!RegionIO::load_chunk(key, c)) {
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
            }
        }
        // Second pass: mesh each with neighbor awareness
        for (int dj = -tile_span; dj <= tile_span; ++dj) {
            for (int di = -tile_span; di <= tile_span; ++di) {
                const Chunk64& c = chunks[idx_of(di, dj)];
                const Chunk64* nx = (di > -tile_span) ? &chunks[idx_of(di - 1, dj)] : nullptr;
                const Chunk64* px = (di <  tile_span) ? &chunks[idx_of(di + 1, dj)] : nullptr;
                const Chunk64* ny = (dj > -tile_span) ? &chunks[idx_of(di, dj - 1)] : nullptr;
                const Chunk64* py = (dj <  tile_span) ? &chunks[idx_of(di, dj + 1)] : nullptr;
                Mesh m;
                mesh_chunk_greedy_neighbors(c, nx, px, ny, py, nullptr, nullptr, m, s);
                if (!m.indices.empty()) {
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
                    VkDeviceSize vbytes = sizeof(Vertex) * m.vertices.size();
                    VkDeviceSize ibytes = sizeof(uint32_t) * m.indices.size();
                    VkBuffer vstage = VK_NULL_HANDLE; VkDeviceMemory vstageMem = VK_NULL_HANDLE;
                    wf::vk::create_buffer(physical_device_, device_, vbytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                          vstage, vstageMem);
                    wf::vk::upload_host_visible(device_, vstageMem, vbytes, m.vertices.data(), 0);
                    wf::vk::create_buffer(physical_device_, device_, vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, rc.vbuf, rc.vmem);
                    VkBuffer istage = VK_NULL_HANDLE; VkDeviceMemory istageMem = VK_NULL_HANDLE;
                    wf::vk::create_buffer(physical_device_, device_, ibytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                          istage, istageMem);
                    wf::vk::upload_host_visible(device_, istageMem, ibytes, m.indices.data(), 0);
                    wf::vk::create_buffer(physical_device_, device_, ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, rc.ibuf, rc.imem);
                    {
                        VkCommandBuffer cmd = wf::vk::begin_one_time_commands(device_, command_pool_);
                        VkBufferCopy c0{0,0,vbytes};
                        vkCmdCopyBuffer(cmd, vstage, rc.vbuf, 1, &c0);
                        VkBufferCopy c1{0,0,ibytes};
                        vkCmdCopyBuffer(cmd, istage, rc.ibuf, 1, &c1);
                        wf::vk::end_one_time_commands(device_, queue_graphics_, command_pool_, cmd);
                    }
                    vkDestroyBuffer(device_, vstage, nullptr); vkFreeMemory(device_, vstageMem, nullptr);
                    vkDestroyBuffer(device_, istage, nullptr); vkFreeMemory(device_, istageMem, nullptr);
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
    std::string pmOverride;
    if (const char* s = std::getenv("WF_PRESENT_MODE")) pmOverride = s;
    auto choose_pm = [&](VkPresentModeKHR want){ for (auto m: pms) if (m==want) { chosenPm = want; return true; } return false; };
    if (!pmOverride.empty()) {
        std::string v; v.resize(pmOverride.size());
        std::transform(pmOverride.begin(), pmOverride.end(), v.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        if      (v=="immediate") choose_pm(VK_PRESENT_MODE_IMMEDIATE_KHR);
        else if (v=="mailbox")  choose_pm(VK_PRESENT_MODE_MAILBOX_KHR);
        else if (v=="fifo_relaxed") choose_pm(VK_PRESENT_MODE_FIFO_RELAXED_KHR);
        else choose_pm(VK_PRESENT_MODE_FIFO_KHR);
    } else {
        if (!choose_pm(VK_PRESENT_MODE_IMMEDIATE_KHR))
            if (!choose_pm(VK_PRESENT_MODE_MAILBOX_KHR))
                choose_pm(VK_PRESENT_MODE_FIFO_KHR);
    }

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

    // Optional no-op compute dispatch (disabled by default)
    if (compute_enabled_ && pipeline_compute_) {
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
    if (!render_chunks_.empty() && chunk_renderer_.is_ready()) {
        // Row-major projection (OpenGL-style) and view; multiply as row-major V*P and pass directly
        float aspect = (float)swapchain_extent_.width / (float)swapchain_extent_.height;
        float fov = 60.0f * 0.01745329252f;
        float zn = 0.1f, zf = 1000.0f;
        float f = 1.0f / std::tan(fov * 0.5f);
        float P[16] = { f/aspect,0,0,0,  0,f,0,0,  0,0,(zf+zn)/(zn-zf),-1,  0,0,(2*zf*zn)/(zn-zf),0 };
        // Camera look-at from yaw/pitch/pos (row-major)
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

        if (chunk_items_tmp_.size() != render_chunks_.size()) chunk_items_tmp_.resize(render_chunks_.size());
        for (size_t i=0;i<render_chunks_.size();++i) {
            chunk_items_tmp_[i] = ChunkDrawItem{render_chunks_[i].vbuf, render_chunks_[i].ibuf, render_chunks_[i].index_count};
        }
        chunk_renderer_.record(cmd, MVP.data(), chunk_items_tmp_);
    } else if (pipeline_triangle_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_triangle_);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    if (overlay_enabled_) overlay_.record_draw(cmd, overlay_draw_slot_);
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
        float sx = invert_mouse_x_ ? -1.0f : 1.0f;
        float sy = invert_mouse_y_ ?  1.0f : -1.0f;
        cam_yaw_   += sx * (float)(dx * cam_sensitivity_);
        cam_pitch_ += sy * (float)(dy * cam_sensitivity_);
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
    // Right-handed basis: right = cross(fwd, up)
    float right[3] = { fwd[1]*up[2]-fwd[2]*up[1], fwd[2]*up[0]-fwd[0]*up[2], fwd[0]*up[1]-fwd[1]*up[0] };
    float rl = std::sqrt(right[0]*right[0]+right[1]*right[1]+right[2]*right[2]);
    if (rl > 0) { right[0]/=rl; right[1]/=rl; right[2]/=rl; }

    float speed = cam_speed_ * dt * (glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 3.0f : 1.0f);
    if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS) { cam_pos_[0]+=fwd[0]*speed; cam_pos_[1]+=fwd[1]*speed; cam_pos_[2]+=fwd[2]*speed; }
    if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS) { cam_pos_[0]-=fwd[0]*speed; cam_pos_[1]-=fwd[1]*speed; cam_pos_[2]-=fwd[2]*speed; }
    if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS) { cam_pos_[0]-=right[0]*speed; cam_pos_[1]-=right[1]*speed; cam_pos_[2]-=right[2]*speed; }
    if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS) { cam_pos_[0]+=right[0]*speed; cam_pos_[1]+=right[1]*speed; cam_pos_[2]+=right[2]*speed; }
    if (glfwGetKey(window_, GLFW_KEY_Q) == GLFW_PRESS) { cam_pos_[1]-=speed; }
    if (glfwGetKey(window_, GLFW_KEY_E) == GLFW_PRESS) { cam_pos_[1]+=speed; }

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
    if (titlebar_enabled_) glfwSetWindowTitle(window_, title);

    char hud[256];
    std::snprintf(hud, sizeof(hud), "FPS: %.1f  Pos:(%.1f,%.1f,%.1f)  Yaw/Pitch:(%.1f,%.1f)  InvX:%d InvY:%d  Speed:%.1f",
                  fps_smooth_, cam_pos_[0], cam_pos_[1], cam_pos_[2], yaw_deg, pitch_deg,
                  invert_mouse_x_?1:0, invert_mouse_y_?1:0, cam_speed_);

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
    if (const char* s = std::getenv("WF_ENABLE_COMPUTE_NOOP")) compute_enabled_ = parse_bool(s, compute_enabled_);
    if (const char* s = std::getenv("WF_DISABLE_OVERLAY")) overlay_enabled_ = !parse_bool(s, false);
    if (const char* s = std::getenv("WF_DISABLE_TITLEBAR")) titlebar_enabled_ = !parse_bool(s, false);

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
        else if (key == "enable_compute_noop") compute_enabled_ = parse_bool(val, compute_enabled_);
        else if (key == "overlay") overlay_enabled_ = parse_bool(val, overlay_enabled_);
        else if (key == "titlebar") titlebar_enabled_ = parse_bool(val, titlebar_enabled_);
    }
}

void VulkanApp::draw_frame() {
    vkWaitForFences(device_, 1, &fences_in_flight_[current_frame_], VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &fences_in_flight_[current_frame_]);

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, sem_image_available_[current_frame_], VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(); return; }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) throw_if_failed(acq, "vkAcquireNextImageKHR failed");

    // Prepare overlay text only if needed and overlay enabled
    if (overlay_enabled_) {
        overlay_draw_slot_ = current_frame_;
        if (!hud_text_.empty() && (!overlay_text_valid_[overlay_draw_slot_] || overlay_last_text_ != hud_text_)) {
            overlay_.build_text(overlay_draw_slot_, hud_text_.c_str(), (int)swapchain_extent_.width, (int)swapchain_extent_.height);
            overlay_text_valid_[overlay_draw_slot_] = true;
            overlay_last_text_ = hud_text_;
        }
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
    // ChunkRenderer encapsulates the chunk pipeline now
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

    VkViewport vp{}; vp.x = 0; vp.y = 0; vp.width = (float)swapchain_extent_.width; vp.height = (float)swapchain_extent_.height; vp.minDepth = 0; vp.maxDepth = 1;
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

// legacy chunk pipeline removed; handled by ChunkRenderer

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

} // namespace wf
