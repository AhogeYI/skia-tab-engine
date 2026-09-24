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

} // namespace tabengine
