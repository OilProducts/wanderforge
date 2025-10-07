#include "ui/ui_controller.h"

#include <algorithm>
#include <array>
#include <string>

#include "ui/ui_id.h"
#include "ui/ui_primitives.h"
#include "ui/ui_text.h"

namespace wf::ui {

void UiController::set_settings(const Settings& settings) {
    settings_ = settings;
}

void UiController::set_hud_text(std::string text) {
    hud_text_ = std::move(text);
}

void UiController::begin_backend_frame(const UIBackend::InputState& input_state) {
    backend_.begin_frame(input_state, frame_index_++);
}

void UiController::end_backend_frame() {
    backend_.end_frame();
}

UiController::FrameOutput UiController::build_frame(const FrameInput& input) {
    ContextParams params;
    params.screen_width = input.screen_width;
    params.screen_height = input.screen_height;
    params.style.scale = settings_.scale;
    params.style.enable_shadow = settings_.shadow_enabled;
    params.style.shadow_offset_px = settings_.shadow_offset_px;
    params.style.shadow_color = Color{0.0f, 0.0f, 0.0f, 0.6f};

    context_.begin(params);

    FrameOutput output;
    output.resolution_index = input.resolution_index;

    TextDrawParams text_params;
    text_params.scale = 1.0f;
    text_params.color = Color{1.0f, 1.0f, 1.0f, 1.0f};
    text_params.line_spacing_px = 4.0f;
    float text_height = 0.0f;
    if (!hud_text_.empty()) {
        text_height = add_text_block(context_, hud_text_.c_str(), params.screen_width, text_params);
    }

    ButtonStyle button_style;
    button_style.text_scale = 1.0f;
    float button_y = text_params.origin_px.y + text_height + 8.0f;
    const float button_height = 18.0f;
    const float button_width = 136.0f;
    const float button_spacing = 4.0f;

    auto button_rect_at = [&](float y) -> Rect {
        return Rect{6.0f, y, button_width, button_height};
    };

    std::string button_label = std::string("Cull: ") + (input.cull_enabled ? "ON" : "OFF");
    if (button(context_, backend_, hash_id("hud.cull"), button_rect_at(button_y), button_label, button_style)) {
        output.toggle_cull = true;
    }
    button_y += button_height + button_spacing;

    std::string axes_label = std::string("Axes: ") + (input.debug_show_axes ? "ON" : "OFF");
    if (button(context_, backend_, hash_id("hud.axes"), button_rect_at(button_y), axes_label, button_style)) {
        output.toggle_debug_axes = true;
    }
    button_y += button_height + button_spacing;

    std::string tri_label = std::string("Tri: ") + (input.debug_show_test_triangle ? "ON" : "OFF");
    if (button(context_, backend_, hash_id("hud.triangle"), button_rect_at(button_y), tri_label, button_style)) {
        output.toggle_debug_triangle = true;
    }
    button_y += button_height + button_spacing;

    if (button(context_, backend_, hash_id("hud.reload_config"), button_rect_at(button_y), "Reload Config", button_style)) {
        output.request_reload = true;
    }
    button_y += button_height + button_spacing;

    if (button(context_, backend_, hash_id("hud.save_config"), button_rect_at(button_y), "Save Config", button_style)) {
        output.request_save = true;
    }
    button_y += button_height + button_spacing;

    std::string auto_label = std::string("Auto Reload: ") + (input.config_auto_reload_enabled ? "ON" : "OFF");
    if (button(context_, backend_, hash_id("hud.auto_reload"), button_rect_at(button_y), auto_label, button_style)) {
        output.toggle_auto_reload = true;
    }
    button_y += button_height + button_spacing;

    if (!input.resolution_labels.empty()) {
        Rect dropdown_rect{6.0f, button_y, button_width, button_height};
        std::size_t selected_index = std::min(input.resolution_index, input.resolution_labels.size() - 1);
        DropdownResult dropdown_res = dropdown(context_, backend_, hash_id("hud.resolution"), dropdown_rect, input.resolution_labels, selected_index);
        if (dropdown_res.selection_changed) {
            output.resolution_changed = true;
            output.resolution_index = dropdown_res.selected_index;
        } else {
            output.resolution_index = dropdown_res.selected_index;
        }
        button_y += button_height + button_spacing;
    }

    const float hud_scale = (settings_.scale > 0.0f) ? settings_.scale : 1.0f;
    auto unscale = [&](float px) { return px / hud_scale; };

    const float tool_slot_px = 72.0f;
    const float tool_slot_spacing_px = 16.0f;
    const float tool_bottom_margin_px = 32.0f;
    const std::size_t tool_count = input.tool_buttons.size();
    if (tool_count > 0) {
        const float tool_total_width_px = tool_slot_px * static_cast<float>(tool_count) + tool_slot_spacing_px * static_cast<float>(tool_count - 1);
        const float tool_start_x_px = (static_cast<float>(input.screen_width) - tool_total_width_px) * 0.5f;
        const float tool_y_px = std::max(0.0f, static_cast<float>(input.screen_height) - tool_bottom_margin_px - tool_slot_px);

        SelectableStyle tool_style;
        tool_style.text_scale = 0.9f;
        tool_style.padding_px = 8.0f;

        for (std::size_t i = 0; i < tool_count; ++i) {
            float x_px = tool_start_x_px + static_cast<float>(i) * (tool_slot_px + tool_slot_spacing_px);
            Rect rect{unscale(x_px), unscale(tool_y_px), unscale(tool_slot_px), unscale(tool_slot_px)};
            std::string id = "hud.tool." + std::to_string(i);
            if (selectable(context_, backend_, hash_id(id.c_str()), rect, input.tool_buttons[i].label, input.tool_buttons[i].selected, tool_style)) {
                output.tool_clicked = i;
            }
        }
    }

    Vec2 crosshair_center{static_cast<float>(input.screen_width) * 0.5f, static_cast<float>(input.screen_height) * 0.5f};
    CrosshairStyle crosshair_style;
    crosshair_style.color = Color{1.0f, 1.0f, 1.0f, 0.9f};
    crosshair_style.arm_length_px = 9.0f;
    crosshair_style.center_gap_px = 6.0f;
    crosshair_style.arm_thickness_px = 2.0f;
    crosshair(context_, crosshair_center, crosshair_style);

    draw_data_ = context_.end();
    return output;
}

const UIDrawData& UiController::draw_data() const {
    return draw_data_;
}

} // namespace wf::ui
