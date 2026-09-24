#pragma once

namespace tabengine::detail {

// Desktop tab-strip defaults aligned with desktop Chromium tab-strip metrics.
// The leading slot is application-painted; its width is a default, not a brand.
struct ChromeMetrics {
    static constexpr int strip_height = 41;
    static constexpr int strip_padding = 6;
    static constexpr int tab_height = 35;
    static constexpr int top_radius = 10;
    static constexpr int bottom_radius = 12;
    static constexpr int overlap = 18;
    static constexpr int standard_tab_width = 256;
    static constexpr int min_inactive_width = 32;
    static constexpr int min_active_width = 56;
    static constexpr int close_hide_width = 100;
    static constexpr int new_tab_size = 28;
    static constexpr int caption_button_width = 45;
    static constexpr int caption_button_spacing = 1;
    static constexpr int caption_button_count = 3;
    static constexpr int separator_width = 2;
    static constexpr int separator_height = 16;
    static constexpr double bounds_duration_s = 0.200;
    static constexpr double hover_duration_s = 0.120;
};

} // namespace tabengine::detail
