#include "tabengine/render.h"
#include "tabengine/text.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int main() {
    auto text_surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(240, 80));
    if (!text_surface) return 1;
    text_surface->getCanvas()->clear(SK_ColorWHITE);
    tabengine::paint_ui_text(*text_surface->getCanvas(), "Tab 123", 12, 42, 20,
                             SK_ColorBLACK);
    tabengine::paint_caption_symbol(*text_surface->getCanvas(), "\xEE\xA2\xBB",
                                    190, 35, 14, SK_ColorBLACK);
    SkPixmap pixels;
    if (!text_surface->peekPixels(&pixels)) return 2;
    int text_ink = 0;
    int symbol_ink = 0;
    for (int y = 10; y < 60; ++y) {
        for (int x = 10; x < 230; ++x) {
            if (pixels.getColor(x, y) == SK_ColorWHITE) continue;
            if (x < 150) ++text_ink;
            if (x >= 170) ++symbol_ink;
        }
    }
    if (text_ink < 50 || symbol_ink < 4) return 3;

    // A real, hidden HWND exercises swapchain creation and raster fallback
    // without opening a visible test window or requiring a mouse/keyboard.
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"TabEngine renderer test", WS_OVERLAPPEDWINDOW,
                                0, 0, 360, 240, nullptr, nullptr, GetModuleHandleW(nullptr),
                                nullptr);
    if (!hwnd) return 4;
    auto renderer = tabengine::make_skia_windows_renderer();
    constexpr tabengine::WindowId id = 1;
    if (!renderer->attach(id, hwnd, {320, 200})) return 5;
    SkCanvas* canvas = renderer->canvas(id);
    if (!canvas) return 6;
    canvas->clear(SK_ColorRED);
    renderer->present(id, hwnd);
    renderer->resize(id, {480, 300});
    canvas = renderer->canvas(id);
    if (!canvas) return 7;
    canvas->clear(SK_ColorBLUE);
    renderer->present(id, hwnd);
    renderer->detach(id);
    DestroyWindow(hwnd);
    return 0;
}
