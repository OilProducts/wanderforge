#include "app_controller.h"
#include "world_runtime.h"

namespace wf {

void AppController::set_config_path(std::string path) {
    app_.set_config_path(std::move(path));
}

int AppController::run() {
    PlatformLayer::Config platform_cfg;
    platform_.initialize(platform_cfg);
    app_.set_platform(&platform_);
    if (!world_runtime_) {
        world_runtime_ = std::make_unique<WorldRuntime>();
    }
    app_.set_world_runtime(world_runtime_.get());
    app_.initialize();
    while (!app_.should_close()) {
        app_.poll_events();
        float dt = app_.advance_time();
        PlatformInputState input = platform_.sample_input();

        ControllerFrameInput frame{};
        frame.dt = dt;
        frame.platform = input;

        frame.actions.move_forward = (input.key_w ? 1.0f : 0.0f) - (input.key_s ? 1.0f : 0.0f);
        frame.actions.move_strafe = (input.key_d ? 1.0f : 0.0f) - (input.key_a ? 1.0f : 0.0f);
        frame.actions.move_vertical = (input.key_e ? 1.0f : 0.0f) - (input.key_q ? 1.0f : 0.0f);
        frame.actions.sprint = input.key_shift_left || input.key_shift_right;

        if (prev_input_valid_) {
            bool reload_pressed = input.key_reload && !prev_input_.key_reload;
            if (reload_pressed) {
                bool shift_down = input.key_shift_left || input.key_shift_right;
                if (shift_down) {
                    frame.save_requested = true;
                } else {
                    frame.reload_requested = true;
                }
            }

            frame.actions.walk_toggle = input.key_walk_toggle && !prev_input_.key_walk_toggle;
            frame.actions.invert_x_toggle = input.key_invert_x && !prev_input_.key_invert_x;
            frame.actions.invert_y_toggle = input.key_invert_y && !prev_input_.key_invert_y;
            frame.actions.place_pressed = input.key_place && !prev_input_.key_place;
            frame.actions.dig_pressed = input.mouse_left && !prev_input_.mouse_left;
        }

        if (input.key_escape) {
            platform_.request_close();
            frame.request_close = true;
        }

        double cursor_x = input.mouse_x;
        double cursor_y = input.mouse_y;
        bool rmb = input.mouse_right;
        if (rmb) {
            if (!rmb_down_) {
                rmb_down_ = true;
                mouse_captured_ = true;
                last_cursor_x_ = cursor_x;
                last_cursor_y_ = cursor_y;
            } else {
                frame.look_yaw_delta = static_cast<float>(cursor_x - last_cursor_x_);
                frame.look_pitch_delta = static_cast<float>(cursor_y - last_cursor_y_);
                last_cursor_x_ = cursor_x;
                last_cursor_y_ = cursor_y;
            }
        } else {
            if (rmb_down_) {
                rmb_down_ = false;
                mouse_captured_ = false;
            }
            last_cursor_x_ = cursor_x;
            last_cursor_y_ = cursor_y;
        }

        frame.mouse_captured = mouse_captured_;

        app_.update_input(frame);
        prev_input_ = input;
        prev_input_valid_ = true;
        app_.update_hud(dt);
        app_.draw_frame();
    }
    app_.shutdown_runtime();
    app_.set_world_runtime(nullptr);
    world_runtime_.reset();
    app_.set_platform(nullptr);
    platform_.shutdown();
    return 0;
}

} // namespace wf
