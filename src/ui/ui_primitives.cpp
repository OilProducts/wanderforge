#include "ui/ui_primitives.h"

#include <algorithm>
#include <span>
#include <string>

namespace wf::ui {
namespace {
inline bool point_in_rect(double x, double y, const Rect& rect) {
    return x >= rect.x && x <= rect.x + rect.w && y >= rect.y && y <= rect.y + rect.h;
}

inline Rect scale_rect(Rect rect, float scale) {
    rect.x *= scale;
    rect.y *= scale;
    rect.w *= scale;
    rect.h *= scale;
    return rect;
}
}

bool button(UIContext& ctx,
            UIBackend& backend,
            UIID id,
            const Rect& rect_px,
            std::string_view label,
            const ButtonStyle& style) {
    UIBackend::InputState input = backend.input();
    WidgetState& state = backend.state(id);

    Rect scaled_rect = rect_px;
    float global_scale = ctx.params().style.scale;
    scaled_rect.x *= global_scale;
    scaled_rect.y *= global_scale;
    scaled_rect.w *= global_scale;
    scaled_rect.h *= global_scale;

    bool inside = input.has_mouse && point_in_rect(input.mouse_x, input.mouse_y, scaled_rect);
    bool mouse_down = input.mouse_down[0];
    bool just_pressed = input.mouse_pressed[0] && inside;

    state.hovered = inside;
    state.active = inside && mouse_down;
    state.pressed = just_pressed;

    Color fill = style.bg;
    if (state.active) fill = style.bg_active;
    else if (state.hovered) fill = style.bg_hover;

    ctx.add_shadowed_quad_pixels(rect_px, fill);

    TextDrawParams text_params;
    text_params.origin_px = Vec2{rect_px.x + style.padding_px, rect_px.y + style.padding_px};
    float margin_right = rect_px.w - style.padding_px;
    text_params.margin_right_px = std::max(0.0f, margin_right);
    text_params.line_spacing_px = 0.0f;
    text_params.scale = style.text_scale;
    text_params.color = style.text;
    text_params.ellipsis = false;
    std::string label_str(label);
    add_text_block(ctx, label_str.c_str(), ctx.params().screen_width, text_params);

    return state.pressed;
}

bool selectable(UIContext& ctx,
                UIBackend& backend,
                UIID id,
                const Rect& rect_px,
                std::string_view label,
                bool selected,
                const SelectableStyle& style) {
    UIBackend::InputState input = backend.input();
    WidgetState& state = backend.state(id);

    Rect scaled_rect = scale_rect(rect_px, ctx.params().style.scale);
    bool inside = input.has_mouse && point_in_rect(input.mouse_x, input.mouse_y, scaled_rect);
    bool just_pressed = input.mouse_pressed[0] && inside;

    state.hovered = inside;
    state.active = inside && input.mouse_down[0];
    state.pressed = just_pressed;
    state.value = selected ? 1.0f : 0.0f;

    Color fill = selected ? style.bg_selected : style.bg;
    if (state.active) {
        fill = style.bg_active;
    } else if (inside) {
        fill = selected ? style.bg_selected_hover : style.bg_hover;
    }

    ctx.add_shadowed_quad_pixels(rect_px, fill);

    TextDrawParams text_params;
    text_params.origin_px = Vec2{rect_px.x + style.padding_px, rect_px.y + style.padding_px};
    text_params.margin_right_px = std::max(0.0f, rect_px.w - style.padding_px * 2.0f);
    text_params.line_spacing_px = 2.0f;
    text_params.scale = style.text_scale;
    text_params.color = style.text;
    text_params.ellipsis = false;

    std::string label_str(label);
    add_text_block(ctx, label_str.c_str(), ctx.params().screen_width, text_params);

    return state.pressed;
}

void crosshair(UIContext& ctx,
               const Vec2& center_px,
               const CrosshairStyle& style) {
    float scale = ctx.params().style.scale;
    if (scale <= 0.0f) {
        scale = 1.0f;
    }

    const float arm_length = std::max(style.arm_length_px, 0.0f);
    const float gap = std::max(style.center_gap_px, 0.0f);
    const float thickness = std::max(style.arm_thickness_px, 0.1f);
    const float half_gap = gap * 0.5f;
    const float half_thickness = thickness * 0.5f;

    if (arm_length <= 0.0f) {
        return;
    }

    auto to_rect = [scale](float x, float y, float w, float h) {
        return Rect{x / scale, y / scale, w / scale, h / scale};
    };

    Rect left = to_rect(center_px.x - half_gap - arm_length,
                        center_px.y - half_thickness,
                        arm_length,
                        thickness);
    Rect right = to_rect(center_px.x + half_gap,
                         center_px.y - half_thickness,
                         arm_length,
                         thickness);
    Rect top = to_rect(center_px.x - half_thickness,
                       center_px.y - half_gap - arm_length,
                       thickness,
                       arm_length);
    Rect bottom = to_rect(center_px.x - half_thickness,
                          center_px.y + half_gap,
                          thickness,
                          arm_length);

    ctx.add_quad_pixels(left, style.color);
    ctx.add_quad_pixels(right, style.color);
    ctx.add_quad_pixels(top, style.color);
    ctx.add_quad_pixels(bottom, style.color);
}

DropdownResult dropdown(UIContext& ctx,
                        UIBackend& backend,
                        UIID id,
                        const Rect& rect_px,
                        std::span<const std::string_view> labels,
                        std::size_t current_index,
                        const DropdownStyle& style) {
    DropdownResult result{};
    if (!labels.empty()) {
        result.selected_index = (current_index < labels.size()) ? current_index : labels.size() - 1;
    } else {
        result.selected_index = 0;
    }

    WidgetState& state = backend.state(id);
    const auto& input = backend.input();
    const float scale = ctx.params().style.scale > 0.0f ? ctx.params().style.scale : 1.0f;

    Rect scaled_rect = scale_rect(rect_px, scale);
    const bool inside_panel = input.has_mouse && point_in_rect(input.mouse_x, input.mouse_y, scaled_rect);
    bool open = state.value > 0.5f;
    bool toggled = false;

    if (inside_panel && input.mouse_pressed[0]) {
        open = !open;
        toggled = true;
    }

    Rect popup_rect = rect_px;
    popup_rect.y += rect_px.h + style.popup_margin_px;
    popup_rect.h = style.item_height_px * static_cast<float>(labels.size());
    Rect popup_rect_scaled = scale_rect(popup_rect, scale);
    const bool inside_popup = open && input.has_mouse && point_in_rect(input.mouse_x, input.mouse_y, popup_rect_scaled);

    if (open && input.mouse_pressed[0] && !inside_panel && !inside_popup) {
        open = false;
    }

    if (labels.empty()) {
        open = false;
    }

    Color panel_color = style.panel_bg;
    if (open) panel_color = style.panel_bg_open;
    else if (inside_panel && input.mouse_down[0]) panel_color = style.panel_bg_active;
    else if (inside_panel) panel_color = style.panel_bg_hover;

    ctx.add_shadowed_quad_pixels(rect_px, panel_color);

    TextDrawParams text_params;
    text_params.origin_px = Vec2{rect_px.x + style.padding_px, rect_px.y + style.padding_px};
    text_params.margin_right_px = std::max(0.0f, rect_px.w - style.padding_px * 2.0f);
    text_params.line_spacing_px = 0.0f;
    text_params.scale = style.text_scale;
    text_params.color = style.text;
    text_params.ellipsis = true;

    std::string current_label;
    if (result.selected_index < labels.size()) {
        current_label.assign(labels[result.selected_index]);
    }
    add_text_block(ctx, current_label.c_str(), ctx.params().screen_width, text_params);

    int hovered_index = -1;

    if (open && !labels.empty()) {
        ctx.add_shadowed_quad_pixels(popup_rect, style.popup_bg);

        for (std::size_t i = 0; i < labels.size(); ++i) {
            Rect row_rect = popup_rect;
            row_rect.y = popup_rect.y + style.item_height_px * static_cast<float>(i);
            row_rect.h = style.item_height_px;
            Rect row_rect_scaled = scale_rect(row_rect, scale);

            bool row_hover = input.has_mouse && point_in_rect(input.mouse_x, input.mouse_y, row_rect_scaled);
            if (row_hover) hovered_index = static_cast<int>(i);

            Color row_color = style.option_bg;
            if (result.selected_index == i) row_color = style.option_bg_selected;
            if (row_hover) row_color = style.option_bg_hover;
            if (row_color.a > 0.0f) {
                ctx.add_quad_pixels(row_rect, row_color);
            }

            TextDrawParams row_text = text_params;
            row_text.origin_px = Vec2{row_rect.x + style.padding_px, row_rect.y + style.padding_px};
            row_text.margin_right_px = std::max(0.0f, row_rect.w - style.padding_px * 2.0f);
            row_text.color = style.option_text;
            row_text.scale = style.text_scale;
            std::string label_str(labels[i]);
            add_text_block(ctx, label_str.c_str(), ctx.params().screen_width, row_text);
        }

        if (!toggled && hovered_index >= 0 && input.mouse_pressed[0]) {
            if (result.selected_index != static_cast<std::size_t>(hovered_index)) {
                result.selected_index = static_cast<std::size_t>(hovered_index);
                result.selection_changed = true;
            }
            open = false;
        }
    }

    state.hovered = inside_panel || inside_popup;
    state.active = open;
    state.pressed = inside_panel && input.mouse_pressed[0];
    state.value = open ? 1.0f : 0.0f;
    state.hot_index = hovered_index;

    result.is_open = open;
    return result;
}

} // namespace wf::ui
