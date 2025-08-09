#include <vulkan/vulkan.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

static std::string apiVersionToString(uint32_t apiVersion) {
    uint32_t major = VK_API_VERSION_MAJOR(apiVersion);
    uint32_t minor = VK_API_VERSION_MINOR(apiVersion);
    uint32_t patch = VK_API_VERSION_PATCH(apiVersion);
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

static const char* deviceTypeToString(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "Integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "Discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "Virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU";
        default:                                     return "Other";
    }
}

int main() {
    // Application info (keep API version conservative for portability)
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "wanderforge";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "none";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

#ifdef WF_DEBUG
    // Optionally enable validation layer in debug builds if present.
    const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    if (layerCount > 0) {
        vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    }
    bool haveValidation = false;
    for (const auto& lp : layers) {
        if (std::strcmp(lp.layerName, validationLayer) == 0) {
            haveValidation = true;
            break;
        }
    }
    if (haveValidation) {
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = &validationLayer;
    }
#endif

    VkInstance instance = VK_NULL_HANDLE;
    VkResult res = vkCreateInstance(&createInfo, nullptr, &instance);
    if (res != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan instance (error code " << res << ")\n";
        return EXIT_FAILURE;
    }

    // Enumerate physical devices
    uint32_t deviceCount = 0;
    res = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (res != VK_SUCCESS || deviceCount == 0) {
        std::cerr << "No Vulkan-capable devices found or enumeration failed.\n";
        vkDestroyInstance(instance, nullptr);
        return EXIT_FAILURE;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    res = vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    if (res != VK_SUCCESS) {
        std::cerr << "Failed to enumerate Vulkan devices.\n";
        vkDestroyInstance(instance, nullptr);
        return EXIT_FAILURE;
    }

    std::cout << "Vulkan devices: " << deviceCount << "\n";
    for (uint32_t i = 0; i < deviceCount; ++i) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(devices[i], &props);
        std::cout << " - " << props.deviceName
                  << " (" << deviceTypeToString(props.deviceType) << ")"
                  << ", API " << apiVersionToString(props.apiVersion)
                  << ", Driver " << apiVersionToString(props.driverVersion)
                  << "\n";
    }

    vkDestroyInstance(instance, nullptr);
    return EXIT_SUCCESS;
}

