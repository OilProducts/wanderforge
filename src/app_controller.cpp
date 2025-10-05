#include "app_controller.h"

namespace wf {

void AppController::set_config_path(std::string path) {
    app_.set_config_path(std::move(path));
}

int AppController::run() {
    PlatformLayer::Config platform_cfg;
    platform_.initialize(platform_cfg);
    app_.set_platform(&platform_);
    app_.initialize();
    while (!app_.should_close()) {
        app_.poll_events();
        float dt = app_.advance_time();
        PlatformInputState input = platform_.sample_input();
        ControllerActions actions;
        actions.move_forward = (input.key_w ? 1.0f : 0.0f) - (input.key_s ? 1.0f : 0.0f);
        actions.move_strafe = (input.key_d ? 1.0f : 0.0f) - (input.key_a ? 1.0f : 0.0f);
        actions.move_vertical = (input.key_e ? 1.0f : 0.0f) - (input.key_q ? 1.0f : 0.0f);
        actions.sprint = input.key_shift_left || input.key_shift_right;

        if (prev_input_valid_) {
            bool reload_pressed = input.key_reload && !prev_input_.key_reload;
            if (reload_pressed) {
                bool shift_down = input.key_shift_left || input.key_shift_right;
                if (shift_down) {
                    app_.request_save_config();
                } else {
                    app_.request_reload_config();
                }
            }

            actions.walk_toggle = input.key_walk_toggle && !prev_input_.key_walk_toggle;
            actions.invert_x_toggle = input.key_invert_x && !prev_input_.key_invert_x;
            actions.invert_y_toggle = input.key_invert_y && !prev_input_.key_invert_y;
            actions.place_pressed = input.key_place && !prev_input_.key_place;
            actions.dig_pressed = input.mouse_left && !prev_input_.mouse_left;
        }

        app_.update_input(dt, input, actions);
        prev_input_ = input;
        prev_input_valid_ = true;
        app_.update_hud(dt);
        app_.draw_frame();
    }
    app_.set_platform(nullptr);
    platform_.shutdown();
    return 0;
}

} // namespace wf
