#include "tabengine/text.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"
#include "include/ports/SkTypeface_win.h"

namespace tabengine {
namespace {

sk_sp<SkTypeface> find_typeface(bool symbols) {
    sk_sp<SkFontMgr> fonts = SkFontMgr_New_DirectWrite();
    if (!fonts) return nullptr;
    if (symbols) {
        for (const char* name : {"Segoe Fluent Icons", "Segoe MDL2 Assets"}) {
            if (auto face = fonts->matchFamilyStyle(name, SkFontStyle::Normal())) return face;
        }
    } else {
        for (const char* name : {"Microsoft YaHei UI", "Microsoft YaHei", "Segoe UI"}) {
            if (auto face = fonts->matchFamilyStyle(name, SkFontStyle::Normal())) return face;
        }
    }
    return fonts->legacyMakeTypeface(nullptr, SkFontStyle::Normal());
}

sk_sp<SkTypeface> ui_typeface() {
    static sk_sp<SkTypeface> face = find_typeface(false);
    return face;
}

sk_sp<SkTypeface> caption_typeface() {
    static sk_sp<SkTypeface> face = find_typeface(true);
    return face;
}

} // namespace

void paint_ui_text(SkCanvas& canvas, std::string_view utf8, float x, float baseline,
                   float size_px, std::uint32_t argb) {
    if (utf8.empty() || size_px <= 0.0f) return;
    SkFont font(ui_typeface(), size_px);
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(argb);
    canvas.drawSimpleText(utf8.data(), utf8.size(), SkTextEncoding::kUTF8,
                          x, baseline, font, paint);
}

void paint_caption_symbol(SkCanvas& canvas, std::string_view utf8, float center_x,
                          float center_y, float size_px, std::uint32_t argb) {
    if (utf8.empty() || size_px <= 0.0f) return;
    SkFont font(caption_typeface(), size_px);
    SkRect ink{};
    font.measureText(utf8.data(), utf8.size(), SkTextEncoding::kUTF8, &ink);
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(argb);
    canvas.drawSimpleText(utf8.data(), utf8.size(), SkTextEncoding::kUTF8,
                          center_x - ink.centerX(), center_y - ink.centerY(), font, paint);
}

} // namespace tabengine
