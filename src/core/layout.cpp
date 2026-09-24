#include "tabengine/layout.h"
#include "chrome_metrics.h"

#include <algorithm>
#include <cmath>

namespace tabengine {
namespace {
int px(float dp, float scale) { return static_cast<int>(std::lround(dp * std::max(scale, 0.5f))); }
}

StripLayout Layout::tab_strip(int width_px, std::size_t count, float scale) {
    using M = detail::ChromeMetrics;
    StripLayout out;
    out.height = px(M::strip_height, scale);
    const int leading = px(M::leading_slot, scale);
    const int caption_width = px(M::caption_button_width * M::caption_button_count +
                                 M::caption_button_spacing * (M::caption_button_count - 1), scale);
    const int strip_width = std::max(0, width_px - caption_width);
    out.caption_start = strip_width;
    out.leading_slot = {0, 0, leading, out.height};
    const int new_tab_size = px(M::new_tab_size, scale);
    const int bottom_radius = px(M::bottom_radius, scale);
    const int padding = px(M::strip_padding, scale);
    const int new_tab_reserve = new_tab_size - bottom_radius + padding;
    const int overlap = px(M::overlap, scale);
    const int available = std::max(0, strip_width - leading - new_tab_reserve);
    const int maximum = px(M::standard_tab_width, scale);
    const int minimum = px(M::min_inactive_width, scale);
    const int raw = count ? (available + static_cast<int>(count - 1) * overlap) /
                                static_cast<int>(count) : maximum;
    const int tab_width = std::clamp(raw, minimum, maximum);
    int x = leading;
    out.tabs.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        int width = tab_width;
        if (i + 1 == count) {
            const int remaining = available - (x - leading);
            if (remaining > 0) width = std::min(width, remaining);
        }
        out.tabs.push_back({x, padding, width, px(M::tab_height, scale)});
        x += width - overlap;
    }
    // Chromium's button begins in the last tab's lower-corner gutter.
    const int tab_right = out.tabs.empty() ? leading : out.tabs.back().right();
    const int button_x = std::clamp(tab_right - bottom_radius + padding, 0,
                                    std::max(0, strip_width - new_tab_size));
    out.new_tab = {button_x, padding, new_tab_size, new_tab_size};
    return out;
}

std::size_t Layout::insertion_index(const StripLayout& layout, int x,
                                    std::size_t dragged_index) {
    std::size_t index = 0;
    for (std::size_t i = 0; i < layout.tabs.size(); ++i) {
        if (i == dragged_index) continue;
        const auto& tab = layout.tabs[i];
        if (x >= tab.x + tab.width / 2) ++index;
    }
    return index;
}

} // namespace tabengine
