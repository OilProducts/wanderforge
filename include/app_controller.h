#pragma once

#include <string>

#include "vk_app.h"
#include "platform_layer.h"

namespace wf {

class AppController {
public:
    AppController() = default;
    void set_config_path(std::string path);
    int run();

private:
    PlatformLayer platform_;
    PlatformInputState prev_input_{};
    bool prev_input_valid_ = false;
    VulkanApp app_;
};

} // namespace wf
