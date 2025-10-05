#include "platform_layer.h"

#include <GLFW/glfw3.h>

namespace wf {

PlatformLayer::~PlatformLayer() {
    shutdown();
}

void PlatformLayer::initialize(const Config& config) {
    if (initialized_) {
        return;
    }
    config_ = config;
    window_.initialize(config.initial_width, config.initial_height, config.title);
    initialized_ = true;
}

void PlatformLayer::shutdown() {
    if (!initialized_) {
        return;
    }
    window_.shutdown();
    initialized_ = false;
}

void PlatformLayer::poll_events() {
    window_.poll_events();
}

bool PlatformLayer::should_close() const {
    return window_.should_close();
}

void PlatformLayer::request_close() {
    if (GLFWwindow* window = window_.handle()) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
}

void PlatformLayer::set_mouse_capture(bool capture) {
    window_.set_mouse_capture(capture);
}

void PlatformLayer::get_window_size(int& width, int& height) const {
    window_.get_window_size(width, height);
}

void PlatformLayer::get_framebuffer_size(int& width, int& height) const {
    window_.get_framebuffer_size(width, height);
}

void PlatformLayer::set_window_size(int width, int height) {
    window_.set_window_size(width, height);
}

GLFWwindow* PlatformLayer::window_handle() const {
    return window_.handle();
}

PlatformInputState PlatformLayer::sample_input() const {
    PlatformInputState state{};
    int ww = 0;
    int wh = 0;
    window_.get_window_size(ww, wh);
    state.window_width = ww;
    state.window_height = wh;

    int fbw = 0;
    int fbh = 0;
    window_.get_framebuffer_size(fbw, fbh);
    state.framebuffer_width = fbw;
    state.framebuffer_height = fbh;

    if (GLFWwindow* window = window_.handle()) {
        glfwGetCursorPos(window, &state.mouse_x, &state.mouse_y);
        state.mouse_left = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        state.mouse_middle = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
        state.mouse_right = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        state.window_focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE;

        state.key_escape = glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
        state.key_reload = glfwGetKey(window, GLFW_KEY_F5) == GLFW_PRESS;
        state.key_walk_toggle = glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS;
        state.key_invert_x = glfwGetKey(window, GLFW_KEY_X) == GLFW_PRESS;
        state.key_invert_y = glfwGetKey(window, GLFW_KEY_Y) == GLFW_PRESS;
        state.key_place = glfwGetKey(window, GLFW_KEY_G) == GLFW_PRESS;

        state.key_shift_left = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
        state.key_shift_right = glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        state.key_w = glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS;
        state.key_a = glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS;
        state.key_s = glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS;
        state.key_d = glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS;
        state.key_q = glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS;
        state.key_e = glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS;
    }

    return state;
}

} // namespace wf
