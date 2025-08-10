#include "vk_utils.h"

#include <cstdio>
#include <vector>
#include <string>
#include <cstring>

namespace wf { namespace vk {

static bool read_file_all(const std::string& p, std::vector<char>& out) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize((size_t)len);
    size_t rd = fread(out.data(), 1, out.size(), f);
    fclose(f);
    return rd == out.size();
}

VkShaderModule load_shader_module(VkDevice device,
                                  const std::string& path,
                                  const std::vector<std::string>& fallbacks) {
    std::vector<char> buf;
    if (!read_file_all(path, buf)) {
        bool ok = false;
        for (const auto& alt : fallbacks) {
            if (read_file_all(alt, buf)) { ok = true; break; }
        }
        if (!ok) return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = buf.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(buf.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &mod) != VK_SUCCESS) return VK_NULL_HANDLE;
    return mod;
}

uint32_t find_memory_type(VkPhysicalDevice phys,
                          uint32_t typeBits,
                          VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) return i;
    }
    return UINT32_MAX;
}

void create_buffer(VkPhysicalDevice phys,
                   VkDevice device,
                   VkDeviceSize size,
                   VkBufferUsageFlags usage,
                   VkMemoryPropertyFlags properties,
                   VkBuffer& outBuffer,
                   VkDeviceMemory& outMemory) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size; bci.usage = usage; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device, &bci, nullptr, &outBuffer);
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device, outBuffer, &req);
    uint32_t mt = find_memory_type(phys, req.memoryTypeBits, properties);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
    vkAllocateMemory(device, &mai, nullptr, &outMemory);
    vkBindBufferMemory(device, outBuffer, outMemory, 0);
}

void upload_host_visible(VkDevice device,
                         VkDeviceMemory memory,
                         VkDeviceSize size,
                         const void* data,
                         VkDeviceSize offset) {
    if (!data || size == 0) return;
    void* p = nullptr;
    vkMapMemory(device, memory, offset, size, 0, &p);
    std::memcpy(p, data, (size_t)size);
    vkUnmapMemory(device, memory);
}

VkCommandBuffer begin_one_time_commands(VkDevice device, VkCommandPool pool) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &ai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void end_one_time_commands(VkDevice device, VkQueue queue, VkCommandPool pool, VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, pool, 1, &cmd);
}

} } // namespace wf::vk
