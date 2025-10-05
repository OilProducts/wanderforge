#pragma once

#include <utility>
#include <vulkan/vulkan.h>

namespace wf::vk {

template <typename Handle, auto DestroyFn>
class UniqueHandle {
public:
    UniqueHandle() = default;
    UniqueHandle(VkDevice device, Handle handle) : device_(device), handle_(handle) {}

    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept { move_from(std::move(other)); }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(std::move(other));
        }
        return *this;
    }

    void reset() {
        if (handle_ != Handle{} && device_ != VK_NULL_HANDLE) {
            DestroyFn(device_, handle_, nullptr);
        }
        handle_ = Handle{};
        device_ = VK_NULL_HANDLE;
    }

    void reset(VkDevice device, Handle handle) {
        if (handle_ == handle && device_ == device) {
            return;
        }
        reset();
        device_ = device;
        handle_ = handle;
    }

    [[nodiscard]] Handle get() const { return handle_; }
    [[nodiscard]] VkDevice device() const { return device_; }
    explicit operator bool() const { return handle_ != Handle{}; }

    Handle release() {
        Handle value = handle_;
        handle_ = Handle{};
        device_ = VK_NULL_HANDLE;
        return value;
    }

private:
    void move_from(UniqueHandle&& other) noexcept {
        device_ = other.device_;
        handle_ = other.handle_;
        other.device_ = VK_NULL_HANDLE;
        other.handle_ = Handle{};
    }

    VkDevice device_ = VK_NULL_HANDLE;
    Handle handle_ = Handle{};
};

using UniqueBuffer = UniqueHandle<VkBuffer, vkDestroyBuffer>;
using UniqueDeviceMemory = UniqueHandle<VkDeviceMemory, vkFreeMemory>;
using UniquePipeline = UniqueHandle<VkPipeline, vkDestroyPipeline>;
using UniquePipelineLayout = UniqueHandle<VkPipelineLayout, vkDestroyPipelineLayout>;
using UniqueShaderModule = UniqueHandle<VkShaderModule, vkDestroyShaderModule>;

} // namespace wf::vk
