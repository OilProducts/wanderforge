#pragma once

#include <cstdint>
#include <string_view>

#include "ui/ui_types.h"

namespace wf::ui {

inline constexpr UIID hash_id(std::string_view value) {
    UIID hash = 14695981039346656037ull; // FNV-1a offset basis
    for (unsigned char c : value) {
        hash ^= static_cast<UIID>(c);
        hash *= 1099511628211ull; // FNV prime
    }
    return hash;
}

inline constexpr UIID hash_id(UIID seed, std::string_view value) {
    UIID hash = seed;
    for (unsigned char c : value) {
        hash ^= static_cast<UIID>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace wf::ui
