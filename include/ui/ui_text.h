#pragma once

#include <string>

#include "ui/ui_context.h"

namespace wf::ui {

struct TextDrawParams {
    Vec2 origin_px{6.0f, 6.0f};
    float margin_right_px = 6.0f;
    float line_spacing_px = 4.0f;
    float scale = 2.0f;
    Color color{1.0f, 1.0f, 1.0f, 1.0f};
    bool ellipsis = true;
};

float add_text_block(UIContext& ctx, const char* text, int screen_width, const TextDrawParams& params);

inline constexpr int kFont6x8Width = 6;
inline constexpr int kFont6x8Height = 8;

} // namespace wf::ui
