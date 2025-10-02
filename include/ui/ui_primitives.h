#pragma once

#include <string_view>

#include "ui/ui_context.h"
#include "ui/ui_backend.h"
#include "ui/ui_text.h"
#include "ui/ui_id.h"

namespace wf::ui {

struct ButtonStyle {
    Color bg = Color{0.15f, 0.15f, 0.18f, 0.85f};
    Color bg_hover = Color{0.25f, 0.25f, 0.30f, 0.95f};
    Color bg_active = Color{0.10f, 0.10f, 0.12f, 1.0f};
    Color text = Color{1.0f, 1.0f, 1.0f, 1.0f};
    float padding_px = 4.0f;
    float text_scale = 1.0f;
};

bool button(UIContext& ctx,
            UIBackend& backend,
            UIID id,
            const Rect& rect_px,
            std::string_view label,
            const ButtonStyle& style = ButtonStyle{});

} // namespace wf::ui
