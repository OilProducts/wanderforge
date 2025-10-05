#pragma once

#include <string>

#include "window_input.h"
#include "platform_input.h"

struct GLFWwindow;

namespace wf {

class PlatformLayer {
public:
    struct Config {
        int initial_width = 1280;
        int initial_height = 720;
        std::string title = "Wanderforge";
    };

    PlatformLayer() = default;
    ~PlatformLayer();

    void initialize(const Config& config);
    void shutdown();

    void poll_events();
    bool should_close() const;
    void request_close();

    void set_mouse_capture(bool capture);
    void get_window_size(int& width, int& height) const;
    void get_framebuffer_size(int& width, int& height) const;
    void set_window_size(int width, int height);

    GLFWwindow* window_handle() const;

    PlatformInputState sample_input() const;

private:
    WindowInput window_;
    Config config_{};
    bool initialized_ = false;
};

} // namespace wf
