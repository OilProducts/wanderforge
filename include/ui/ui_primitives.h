#pragma once

#include <string_view>
#include <span>

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

struct SelectableStyle {
    Color bg = Color{0.16f, 0.16f, 0.18f, 0.85f};
    Color bg_hover = Color{0.24f, 0.24f, 0.28f, 0.95f};
    Color bg_active = Color{0.10f, 0.10f, 0.12f, 1.0f};
    Color bg_selected = Color{0.30f, 0.25f, 0.12f, 0.95f};
    Color bg_selected_hover = Color{0.35f, 0.28f, 0.14f, 1.0f};
    Color text = Color{1.0f, 1.0f, 1.0f, 1.0f};
    float padding_px = 6.0f;
    float text_scale = 1.0f;
};

struct CrosshairStyle {
    Color color = Color{1.0f, 1.0f, 1.0f, 0.85f};
    float arm_length_px = 8.0f;
    float arm_thickness_px = 2.0f;
    float center_gap_px = 4.0f;
};

struct DropdownStyle {
    Color panel_bg = Color{0.15f, 0.15f, 0.18f, 0.92f};
    Color panel_bg_hover = Color{0.22f, 0.22f, 0.26f, 0.97f};
    Color panel_bg_active = Color{0.12f, 0.12f, 0.14f, 1.0f};
    Color panel_bg_open = Color{0.18f, 0.18f, 0.22f, 1.0f};
    Color text = Color{1.0f, 1.0f, 1.0f, 1.0f};
    Color popup_bg = Color{0.12f, 0.12f, 0.14f, 0.98f};
    Color option_bg = Color{0.0f, 0.0f, 0.0f, 0.0f};
    Color option_bg_hover = Color{0.25f, 0.25f, 0.32f, 0.95f};
    Color option_bg_selected = Color{0.20f, 0.20f, 0.26f, 0.95f};
    Color option_text = Color{1.0f, 1.0f, 1.0f, 1.0f};
    float padding_px = 4.0f;
    float text_scale = 1.0f;
    float item_height_px = 18.0f;
    float popup_margin_px = 2.0f;
};

struct DropdownResult {
    std::size_t selected_index = 0;
    bool selection_changed = false;
    bool is_open = false;
};

bool button(UIContext& ctx,
            UIBackend& backend,
            UIID id,
            const Rect& rect_px,
            std::string_view label,
            const ButtonStyle& style = ButtonStyle{});

bool selectable(UIContext& ctx,
                UIBackend& backend,
                UIID id,
                const Rect& rect_px,
                std::string_view label,
                bool selected,
                const SelectableStyle& style = SelectableStyle{});

void crosshair(UIContext& ctx,
               const Vec2& center_px,
               const CrosshairStyle& style = CrosshairStyle{});

DropdownResult dropdown(UIContext& ctx,
                        UIBackend& backend,
                        UIID id,
                        const Rect& rect_px,
                        std::span<const std::string_view> labels,
                        std::size_t current_index,
                        const DropdownStyle& style = DropdownStyle{});

} // namespace wf::ui
