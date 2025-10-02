#pragma once

#include <vector>

#include "ui/ui_types.h"

namespace wf::ui {

struct UIStyle {
    float scale = 1.0f;
    Color text_color{1.0f, 1.0f, 1.0f, 1.0f};
    bool enable_shadow = false;
    float shadow_offset_px = 1.0f;
    Color shadow_color{0.0f, 0.0f, 0.0f, 0.6f};
};

struct ContextParams {
    int screen_width = 1;
    int screen_height = 1;
    UIStyle style{};
};

class UIContext {
public:
    void begin(const ContextParams& params);

    // Basic primitive builders operating in screen-space pixels.
    void add_quad_pixels(const Rect& rect, const Color& color);
    void add_shadowed_quad_pixels(const Rect& rect, const Color& color);

    UIDrawData end();

    const ContextParams& params() const { return params_; }
    const std::vector<UIDrawVertex>& vertices() const { return vertices_; }

private:
    ContextParams params_{};
    std::vector<UIDrawVertex> vertices_;
    UIDrawData draw_data_{};

    void push_quad_ndc(const Rect& rect_px, const Color& color) noexcept;
};

} // namespace wf::ui
