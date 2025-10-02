#include "ui/ui_backend.h"

namespace wf::ui {

void UIBackend::begin_frame(const InputState& input, std::uint64_t frame_index) {
    InputState resolved = input;
    if (!has_prev_input_) {
        for (int i = 0; i < 3; ++i) {
            resolved.mouse_pressed[i] = input.mouse_down[i];
            resolved.mouse_released[i] = false;
        }
        resolved.scroll_y = input.scroll_y;
        has_prev_input_ = true;
    } else {
        for (int i = 0; i < 3; ++i) {
            bool was_down = prev_input_state_.mouse_down[i];
            bool is_down = input.mouse_down[i];
            resolved.mouse_pressed[i] = (!was_down && is_down);
            resolved.mouse_released[i] = (was_down && !is_down);
        }
    }
    prev_input_state_ = input_state_;
    input_state_ = resolved;
    frame_index_ = frame_index;
}

void UIBackend::end_frame() {
    // Currently nothing to do; placeholder for animations/state decay.
}

WidgetState& UIBackend::state(UIID id) {
    return widget_states_[id];
}

const WidgetState* UIBackend::find_state(UIID id) const {
    auto it = widget_states_.find(id);
    return (it != widget_states_.end()) ? &it->second : nullptr;
}

} // namespace wf::ui

