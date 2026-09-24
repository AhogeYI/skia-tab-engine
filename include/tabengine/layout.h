#pragma once

#include "tabengine/types.h"

#include <cstddef>
#include <vector>

namespace tabengine {

struct StripLayout {
    std::vector<Rect> tabs;
    Rect new_tab;
    Rect leading_slot;
    int caption_start = 0;
    int height = 40;
};

class Layout {
public:
    [[nodiscard]] static StripLayout tab_strip(int width_px, std::size_t count, float scale);
    [[nodiscard]] static std::size_t insertion_index(const StripLayout& layout, int x,
                                                     std::size_t dragged_index);
};

} // namespace tabengine
