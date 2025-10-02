#pragma once

#include <cstdint>
#include <unordered_map>

namespace wf::ui {

using UIID = std::uint64_t;

struct WidgetState {
    bool hovered = false;
    bool active = false;
    bool pressed = false;
    float value = 0.0f;
};

class UIBackend {
public:
    struct InputState {
        double mouse_x = 0.0;
        double mouse_y = 0.0;
        bool mouse_down[3] = {false, false, false};
        bool mouse_pressed[3] = {false, false, false};
        bool mouse_released[3] = {false, false, false};
        double scroll_y = 0.0;
        bool has_mouse = false;
    };

    void begin_frame(const InputState& input, std::uint64_t frame_index);
    void end_frame();

    const InputState& input() const { return input_state_; }

    WidgetState& state(UIID id);
    const WidgetState* find_state(UIID id) const;

private:
    InputState input_state_{};
    InputState prev_input_state_{};
    bool has_prev_input_ = false;
    std::uint64_t frame_index_ = 0;
    std::unordered_map<UIID, WidgetState> widget_states_;
};

} // namespace wf::ui
