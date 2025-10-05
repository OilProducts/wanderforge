#pragma once

namespace wf {

struct PlatformInputState {
    double mouse_x = 0.0;
    double mouse_y = 0.0;
    bool mouse_left = false;
    bool mouse_middle = false;
    bool mouse_right = false;
    bool window_focused = false;

    bool key_escape = false;
    bool key_reload = false;      // F5
    bool key_walk_toggle = false; // F
    bool key_invert_x = false;    // X
    bool key_invert_y = false;    // Y
    bool key_place = false;       // G

    bool key_shift_left = false;
    bool key_shift_right = false;
    bool key_w = false;
    bool key_a = false;
    bool key_s = false;
    bool key_d = false;
    bool key_q = false;
    bool key_e = false;

    int window_width = 0;
    int window_height = 0;
    int framebuffer_width = 0;
    int framebuffer_height = 0;
};

struct ControllerActions {
    float move_forward = 0.0f;
    float move_strafe = 0.0f;
    float move_vertical = 0.0f;
    bool sprint = false;
    bool walk_toggle = false;
    bool invert_x_toggle = false;
    bool invert_y_toggle = false;
    bool place_pressed = false;
    bool dig_pressed = false;
};

} // namespace wf
