#include "ui/ui_context.h"

namespace wf::ui {

void UIContext::begin(const ContextParams& params) {
    params_ = params;
    vertices_.clear();
}

void UIContext::add_quad_pixels(const Rect& rect, const Color& color) {
    push_quad_ndc(rect, color);
}

void UIContext::add_shadowed_quad_pixels(const Rect& rect, const Color& color) {
    if (params_.style.enable_shadow) {
        Rect shadow_rect = rect;
        shadow_rect.x += params_.style.shadow_offset_px;
        shadow_rect.y += params_.style.shadow_offset_px;
        push_quad_ndc(shadow_rect, params_.style.shadow_color);
    }
    push_quad_ndc(rect, color);
}

UIDrawData UIContext::end() {
    draw_data_.vertices = vertices_.empty() ? nullptr : vertices_.data();
    draw_data_.vertex_count = vertices_.size();
    return draw_data_;
}

void UIContext::push_quad_ndc(const Rect& rect_px, const Color& color) noexcept {
    if (params_.screen_width <= 0 || params_.screen_height <= 0) return;
    auto to_ndc = [&](float px, float py) {
        float xn = (px / static_cast<float>(params_.screen_width)) * 2.0f - 1.0f;
        float yn = (py / static_cast<float>(params_.screen_height)) * 2.0f - 1.0f;
        return Vec2{xn, yn};
    };

    Rect scaled = rect_px;
    float scale = params_.style.scale;
    scaled.x *= scale;
    scaled.y *= scale;
    scaled.w *= scale;
    scaled.h *= scale;

    Vec2 p0 = to_ndc(scaled.x, scaled.y);
    Vec2 p1 = to_ndc(scaled.x + scaled.w, scaled.y);
    Vec2 p2 = to_ndc(scaled.x + scaled.w, scaled.y + scaled.h);
    Vec2 p3 = to_ndc(scaled.x, scaled.y + scaled.h);

    UIDrawVertex v0{p0.x, p0.y, color.r, color.g, color.b, color.a};
    UIDrawVertex v1{p1.x, p1.y, color.r, color.g, color.b, color.a};
    UIDrawVertex v2{p2.x, p2.y, color.r, color.g, color.b, color.a};
    UIDrawVertex v3{p3.x, p3.y, color.r, color.g, color.b, color.a};

    vertices_.push_back(v0);
    vertices_.push_back(v1);
    vertices_.push_back(v2);
    vertices_.push_back(v0);
    vertices_.push_back(v2);
    vertices_.push_back(v3);
}

} // namespace wf::ui

