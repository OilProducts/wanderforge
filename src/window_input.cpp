#include "window_input.h"

#include <GLFW/glfw3.h>

#include <iostream>

namespace wf {

namespace {
int g_glfw_init_count = 0;
}

WindowInput::~WindowInput() {
    shutdown();
}

void WindowInput::initialize(int width, int height, const std::string& title) {
    if (window_) {
        return;
    }

    if (g_glfw_init_count == 0) {
        if (!glfwInit()) {
            std::cerr << "Failed to initialize GLFW\n";
            std::abort();
        }
    }
    ++g_glfw_init_count;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!window_) {
        std::cerr << "Failed to create GLFW window\n";
        std::abort();
    }

    glfwGetWindowSize(window_, &window_width_, &window_height_);
    glfwGetFramebufferSize(window_, &framebuffer_width_, &framebuffer_height_);
}

void WindowInput::shutdown() {
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }

    if (g_glfw_init_count > 0) {
        --g_glfw_init_count;
        if (g_glfw_init_count == 0) {
            glfwTerminate();
        }
    }
}

void WindowInput::poll_events() const {
    glfwPollEvents();
}

bool WindowInput::should_close() const {
    if (!window_) return true;
    return glfwWindowShouldClose(window_);
}

void WindowInput::set_mouse_capture(bool capture) {
    if (!window_) return;
    if (capture) {
        glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
#if GLFW_VERSION_MAJOR >= 3
        if (glfwRawMouseMotionSupported()) {
            glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        }
#endif
    } else {
        glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
#if GLFW_VERSION_MAJOR >= 3
        if (glfwRawMouseMotionSupported()) {
            glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
        }
#endif
    }
}

void WindowInput::get_window_size(int& width, int& height) const {
    if (window_) {
        glfwGetWindowSize(window_, &window_width_, &window_height_);
    }
    width = window_width_;
    height = window_height_;
}

void WindowInput::get_framebuffer_size(int& width, int& height) const {
    if (window_) {
        glfwGetFramebufferSize(window_, &framebuffer_width_, &framebuffer_height_);
    }
    width = framebuffer_width_;
    height = framebuffer_height_;
}

void WindowInput::set_window_size(int width, int height) {
    if (!window_) return;
    glfwSetWindowSize(window_, width, height);
    glfwGetWindowSize(window_, &window_width_, &window_height_);
    glfwGetFramebufferSize(window_, &framebuffer_width_, &framebuffer_height_);
}

} // namespace wf
