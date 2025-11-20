#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ui/ui_backend.h"
#include "ui/ui_context.h"

namespace wf::ui {

class UiController {
public:
    struct Settings {
        float scale = 2.0f;
        bool shadow_enabled = false;
        float shadow_offset_px = 1.5f;
    };

    struct ToolButton {
        std::string_view label;
        bool selected = false;
    };

    struct FrameInput {
        int screen_width = 0;
        int screen_height = 0;
        bool cull_enabled = false;
        bool debug_show_axes = false;
        bool debug_show_test_triangle = false;
        bool config_auto_reload_enabled = false;
        std::span<const std::string_view> resolution_labels{};
        std::size_t resolution_index = 0;
        std::span<const ToolButton> tool_buttons{};
    };

    struct FrameOutput {
        bool toggle_cull = false;
        bool toggle_debug_axes = false;
        bool toggle_debug_triangle = false;
        bool request_reload = false;
        bool request_save = false;
        bool toggle_auto_reload = false;
        bool resolution_changed = false;
        std::size_t resolution_index = 0;
        std::optional<std::size_t> tool_clicked;
    };

    void set_settings(const Settings& settings);
    void set_hud_text(std::string text);

    void begin_backend_frame(const UIBackend::InputState& input_state);
    void end_backend_frame();

    FrameOutput build_frame(const FrameInput& input);
    const UIDrawData& draw_data() const;

private:
    Settings settings_{};
    UIContext context_{};
    UIBackend backend_{};
    UIDrawData draw_data_{};
    std::string hud_text_{};
    std::uint64_t frame_index_ = 0;
};

} // namespace wf::ui

