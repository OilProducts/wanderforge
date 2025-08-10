// Minimal Vulkan helper utilities (convention-agnostic)
#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <vector>

namespace wf { namespace vk {

// Load a SPIR-V shader module from disk.
// If the exact path is not found, optionally tries a list of fallback paths.
VkShaderModule load_shader_module(VkDevice device,
                                  const std::string& path,
                                  const std::vector<std::string>& fallbacks = {});

// Find a memory type index on the given physical device with the required flags.
uint32_t find_memory_type(VkPhysicalDevice phys,
                          uint32_t typeBits,
                          VkMemoryPropertyFlags properties);

// Create a buffer and allocate/bind memory with the requested properties.
// Does not upload any data.
void create_buffer(VkPhysicalDevice phys,
                   VkDevice device,
                   VkDeviceSize size,
                   VkBufferUsageFlags usage,
                   VkMemoryPropertyFlags properties,
                   VkBuffer& outBuffer,
                   VkDeviceMemory& outMemory);

// Upload to a host-visible, host-coherent allocation.
void upload_host_visible(VkDevice device,
                         VkDeviceMemory memory,
                         VkDeviceSize size,
                         const void* data,
                         VkDeviceSize offset = 0);

// One-time command helpers (graphics queue): begin/end submit and wait.
VkCommandBuffer begin_one_time_commands(VkDevice device, VkCommandPool pool);
void end_one_time_commands(VkDevice device, VkQueue queue, VkCommandPool pool, VkCommandBuffer cmd);

} } // namespace wf::vk

