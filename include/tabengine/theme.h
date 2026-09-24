#pragma once

#include <cstdint>

namespace tabengine {

// Colors for the library-owned window chrome. The embedding application owns
// the body, toolbar, branding, and any product-specific controls.
struct Theme {
    std::uint32_t strip = 0xFF0D1526;
    std::uint32_t body = 0xFF182238;
    std::uint32_t tab_active = 0xFF182238;
    std::uint32_t tab_inactive = 0xFF202B45;
    std::uint32_t tab_hover = 0xFF2A3757;
    std::uint32_t text = 0xFFD9E4FF;
    std::uint32_t text_muted = 0xFF93A1C4;
    std::uint32_t tab_close = 0xFFAAB8D8;
    std::uint32_t separator = 0x66FFFFFF;
    std::uint32_t new_tab = 0xFF223050;
    std::uint32_t new_tab_hover = 0xFF2B3A5C;
    std::uint32_t hover_card = 0xFF202B45;
    std::uint32_t hover_card_border = 0xFF3C4B6A;
    std::uint32_t caption_hover = 0xFF2A3757;
    std::uint32_t caption_close_hover = 0xFFE81123;

    [[nodiscard]] static Theme light();
};

} // namespace tabengine
