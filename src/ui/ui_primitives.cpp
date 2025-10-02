#include "ui/ui_primitives.h"

#include <algorithm>
#include <string>

namespace wf::ui {
namespace {
inline bool point_in_rect(double x, double y, const Rect& rect) {
    return x >= rect.x && x <= rect.x + rect.w && y >= rect.y && y <= rect.y + rect.h;
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

} // namespace wf::ui
