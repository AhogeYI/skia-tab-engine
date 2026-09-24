#pragma once

#include "tabengine/types.h"

#include <cstddef>
#include <vector>

namespace tabengine {

struct ChromeOptions {
    int leading_slot_width_dp = 36;
    int extra_caption_buttons = 0; // Drawn and handled by the embedding application.
};

struct StripLayout {
    std::vector<Rect> tabs;
    Rect new_tab;
    Rect leading_slot;
    int caption_start = 0;
    int extra_caption_buttons = 0;
    int caption_button_count = 3;
    int height = 40;
};

struct DragVisual {
    Rect tab;
    Rect new_tab;
};

class Layout {
public:
    [[nodiscard]] static StripLayout tab_strip(int width_px, std::size_t count, float scale,
                                               ChromeOptions options = {});
    [[nodiscard]] static StripLayout tab_strip(int width_px,
                                               const std::vector<bool>& closing, float scale,
                                               ChromeOptions options = {});
    [[nodiscard]] static std::size_t insertion_index(const StripLayout& layout, int x,
                                                     std::size_t dragged_index);
    [[nodiscard]] static DragVisual drag_visual(const StripLayout& layout,
                                                std::size_t dragged_index,
                                                int pointer_x, int grab_x, float scale);
};

} // namespace tabengine
