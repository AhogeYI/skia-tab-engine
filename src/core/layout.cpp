#include "tabengine/layout.h"
#include "chrome_metrics.h"

#include <algorithm>
#include <cmath>

namespace tabengine {
namespace {
int px(float dp, float scale) { return static_cast<int>(std::lround(dp * std::max(scale, 0.5f))); }
}

StripLayout Layout::tab_strip(int width_px, std::size_t count, float scale,
                              ChromeOptions options) {
    return tab_strip(width_px, std::vector<bool>(count, false), scale, options);
}

StripLayout Layout::tab_strip(int width_px, const std::vector<bool>& closing, float scale,
                              ChromeOptions options) {
    using M = detail::ChromeMetrics;
    StripLayout out;
    out.height = px(M::strip_height, scale);
    const int leading = px(std::clamp(options.leading_slot_width_dp, 0, 256), scale);
    out.extra_caption_buttons = std::clamp(options.extra_caption_buttons, 0, 4);
    out.caption_button_count = M::caption_button_count + out.extra_caption_buttons;
    const int caption_width = px(M::caption_button_width * out.caption_button_count +
                                 M::caption_button_spacing * (out.caption_button_count - 1), scale);
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
    const std::size_t live_count = static_cast<std::size_t>(
        std::count(closing.begin(), closing.end(), false));
    const int raw = live_count ?
        (available + static_cast<int>(live_count - 1) * overlap) /
            static_cast<int>(live_count) : maximum;
    const int tab_width = std::clamp(raw, minimum, maximum);
    int x = leading;
    std::vector<Rect> live_tabs;
    live_tabs.reserve(live_count);
    for (std::size_t i = 0; i < live_count; ++i) {
        int width = tab_width;
        if (i + 1 == live_count) {
            const int remaining = available - (x - leading);
            if (remaining > 0) width = std::min(width, remaining);
        }
        live_tabs.push_back({x, padding, width, px(M::tab_height, scale)});
        x += width - overlap;
    }
    out.tabs.resize(closing.size());
    std::size_t live_index = 0;
    for (std::size_t i = 0; i < closing.size(); ++i) {
        if (!closing[i]) {
            out.tabs[i] = live_tabs[live_index++];
        } else {
            const int close_x = live_index > 0
                ? live_tabs[live_index - 1].right() - overlap
                : live_tabs.empty() ? leading : live_tabs.front().x;
            out.tabs[i] = {close_x, padding, overlap, px(M::tab_height, scale)};
        }
    }
    // Chromium's button begins in the last tab's lower-corner gutter.
    const int tab_right = live_tabs.empty() ? leading : live_tabs.back().right();
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

DragVisual Layout::drag_visual(const StripLayout& layout, std::size_t dragged_index,
                               int pointer_x, int grab_x, float scale) {
    if (dragged_index >= layout.tabs.size()) return {{}, layout.new_tab};
    using M = detail::ChromeMetrics;
    Rect dragged = layout.tabs[dragged_index];
    const int reserved = px(M::new_tab_size - M::bottom_radius + M::strip_padding, scale);
    const int max_x = std::max(0, layout.caption_start - reserved - dragged.width);
    const int min_x = std::min(layout.leading_slot.right(), max_x);
    dragged.x = std::clamp(pointer_x - grab_x, min_x, max_x);

    int right = dragged.right();
    for (std::size_t i = 0; i < layout.tabs.size(); ++i) {
        if (i != dragged_index) right = std::max(right, layout.tabs[i].right());
    }
    Rect new_tab = layout.new_tab;
    new_tab.x = std::clamp(right - px(M::bottom_radius, scale) +
                               px(M::strip_padding, scale),
                           0, std::max(0, layout.caption_start - new_tab.width));
    return {dragged, new_tab};
}

} // namespace tabengine
