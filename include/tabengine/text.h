#pragma once

#include <cstdint>
#include <string_view>

class SkCanvas;

namespace tabengine {

// Skia text primitives used by the reference chrome and its demo. A platform
// font service provides the typeface; applications may paint with their own.
void paint_ui_text(SkCanvas& canvas, std::string_view utf8, float x, float baseline,
                   float size_px, std::uint32_t argb);
void paint_caption_symbol(SkCanvas& canvas, std::string_view utf8, float center_x,
                          float center_y, float size_px, std::uint32_t argb);
// Advance width of `utf8` in the UI face at `size_px`. Hosts that lay out
// text (address bars, lists) hit-test with the same number the paint used.
[[nodiscard]] float measure_ui_text(std::string_view utf8, float size_px);

} // namespace tabengine
