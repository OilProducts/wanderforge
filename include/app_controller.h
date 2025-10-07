#pragma once

#include <memory>
#include <string>

#include "vk_app.h"
#include "platform_layer.h"
#include "render_system.h"

namespace wf {

class WorldRuntime;

class AppController {
public:
    AppController() = default;
    void set_config_path(std::string path);
    int run();

private:
    PlatformLayer platform_;
    PlatformInputState prev_input_{};
    bool prev_input_valid_ = false;
    double last_cursor_x_ = 0.0;
    double last_cursor_y_ = 0.0;
    bool rmb_down_ = false;
    bool mouse_captured_ = false;
    std::unique_ptr<WorldRuntime> world_runtime_;
    RenderSystem render_system_;
    VulkanApp app_;
};

} // namespace wf
