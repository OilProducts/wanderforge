#pragma once

#include <string>

struct GLFWwindow;

namespace wf {

class WindowInput {
public:
    WindowInput() = default;
    ~WindowInput();

    void initialize(int width, int height, const std::string& title);
    void shutdown();

    GLFWwindow* handle() const { return window_; }

    void poll_events() const;
    bool should_close() const;

    void set_mouse_capture(bool capture);

    void get_window_size(int& width, int& height) const;
    void get_framebuffer_size(int& width, int& height) const;

    void set_window_size(int width, int height);

private:
    GLFWwindow* window_ = nullptr;
    mutable int window_width_ = 0;
    mutable int window_height_ = 0;
    mutable int framebuffer_width_ = 0;
    mutable int framebuffer_height_ = 0;
};

} // namespace wf
