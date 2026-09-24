#include "tabengine/layout.h"

#include <algorithm>
#include <cmath>

namespace tabengine {
namespace {
int px(float dp, float scale) { return static_cast<int>(std::lround(dp * std::max(scale, 0.5f))); }
}

StripLayout Layout::tab_strip(int width_px, std::size_t count, float scale) {
    StripLayout out;
    out.height = px(41, scale);
    const int left = px(40, scale);  // application-owned leading slot
    const int right = px(170, scale); // new-tab button and three caption controls
    const int overlap = px(18, scale);
    const int available = std::max(0, width_px - left - right);
    const int maximum = px(256, scale);
    const int minimum = px(56, scale);
    const int raw = count ? (available + static_cast<int>(count - 1) * overlap) /
                                static_cast<int>(count) : maximum;
    const int tab_width = std::clamp(raw, minimum, maximum);
    int x = left;
    out.tabs.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.tabs.push_back({x, px(6, scale), tab_width, px(35, scale)});
        x += tab_width - overlap;
    }
    out.new_tab = {x - px(12, scale) + px(6, scale), px(6, scale),
                   px(28, scale), px(28, scale)};
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
