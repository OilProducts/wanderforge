#pragma once

#include <cstdint>
#include <cstddef>

namespace wf::ui {

using UIID = std::uint64_t;

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Color {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

struct UIDrawVertex {
    float x = 0.0f;
    float y = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct UIDrawData {
    const UIDrawVertex* vertices = nullptr;
    std::size_t vertex_count = 0;
};

} // namespace wf::ui
