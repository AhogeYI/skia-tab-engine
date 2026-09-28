#include "tabengine/render.h"
#include "tabengine/text.h"
#include "tabengine/win32_platform.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkImageFilters.h"

#include <ft2build.h>
#include FT_FREETYPE_H

int main() {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(80, 32));
    if (!surface) {
        return 1;
    }
    SkPaint paint;
    paint.setColor(SK_ColorRED);
    surface->getCanvas()->drawRect(SkRect::MakeWH(80, 32), paint);
    tabengine::paint_ui_text(*surface->getCanvas(), "SDK", 2, 20, 14, SK_ColorBLACK);
    auto image = surface->makeImageSnapshot();
    auto filter = SkImageFilters::Blur(1.0f, 1.0f, nullptr);
    if (!image || !filter) {
        return 2;
    }
    auto platform = tabengine::make_win32_platform();
    auto renderer = tabengine::make_skia_raster_renderer();
    if (!platform || !renderer) {
        return 3;
    }
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0) {
        return 4;
    }
    FT_Done_FreeType(library);
    return 0;
}
