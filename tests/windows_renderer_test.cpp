#include "tabengine/render.h"
#include "tabengine/text.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <memory>

namespace {

// Pixel at (x, y) of a captured BGRA frame as 0xAARRGGBB.
std::uint32_t pixel_at(const tabengine::CapturedFrame& frame, int x, int y) {
    const std::size_t offset = (static_cast<std::size_t>(y) * frame.size.width + x) * 4;
    const std::uint8_t b = frame.pixels[offset];
    const std::uint8_t g = frame.pixels[offset + 1];
    const std::uint8_t r = frame.pixels[offset + 2];
    const std::uint8_t a = frame.pixels[offset + 3];
    return (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(r) << 16) |
           (static_cast<std::uint32_t>(g) << 8) | b;
}

// Captures a cleared window on the given renderer and checks deterministic
// pixels through the public capture path. Returns 0 on success.
int check_capture(tabengine::IRenderer& renderer, tabengine::WindowId id, HWND hwnd,
                  std::uint32_t clear_color, const char* label) {
    renderer.attach(id, hwnd, {160, 120});
    SkCanvas* canvas = renderer.canvas(id);
    if (!canvas) return 1;
    canvas->clear(clear_color);
    renderer.present(id, hwnd);
    if (!renderer.supports_capture()) return 2;
    tabengine::CapturedFrame frame;
    if (!renderer.capture_frame(id, frame) || !frame.valid()) return 3;
    const std::uint32_t expected = clear_color | 0xFF000000u;
    const std::uint32_t corner = pixel_at(frame, 3, 3);
    const std::uint32_t center = pixel_at(frame, frame.size.width / 2,
                                          frame.size.height / 2);
    if ((corner & 0x00FFFFFFu) != (expected & 0x00FFFFFFu)) {
        std::fprintf(stderr, "%s capture mismatch: %08x\n", label, corner);
        return 4;
    }
    if ((center & 0x00FFFFFFu) != (expected & 0x00FFFFFFu)) {
        std::fprintf(stderr, "%s capture mismatch (center): %08x\n", label, center);
        return 5;
    }
    renderer.detach(id);
    return 0;
}

} // namespace

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
    const auto initial = renderer->info(id);
    if (initial.surface_size.width != 320) return 8;
    SkCanvas* canvas = renderer->canvas(id);
    if (!canvas) return 6;
    canvas->clear(SK_ColorRED);
    renderer->present(id, hwnd);
    renderer->resize(id, {480, 300});
    if (renderer->info(id).surface_size.width != 480 ||
        renderer->info(id).surface_size.height != 300) return 9;
    if (renderer->info(id).backend != initial.backend) return 10;
    canvas = renderer->canvas(id);
    if (!canvas) return 7;
    canvas->clear(SK_ColorBLUE);
    renderer->present(id, hwnd);

    // The real capture path on the backend actually in use: verified pixel
    // content after a clear+present, not just "PNG is non-empty".
    tabengine::CapturedFrame frame;
    if (!renderer->supports_capture()) return 11;
    if (!renderer->capture_frame(id, frame) || !frame.valid()) return 12;
    if (frame.backend != initial.backend) return 13;
    if ((pixel_at(frame, 5, 5) & 0x00FFFFFFu) != (SK_ColorBLUE & 0x00FFFFFFu)) {
        std::fprintf(stderr, "live capture mismatch: %08x\n", pixel_at(frame, 5, 5));
        return 14;
    }
    std::fprintf(stderr, "renderer capture: backend=%d size=%dx%d\n",
                 static_cast<int>(frame.backend), frame.size.width, frame.size.height);
    renderer->detach(id);

    // Dedicated raster check with a different color so both public
    // renderer factories are covered by pixel assertions.
    auto raster = tabengine::make_skia_raster_renderer();
    if (const int result = check_capture(*raster, 2, hwnd, SK_ColorGREEN, "raster")) {
        return 20 + result;
    }
    DestroyWindow(hwnd);
    return 0;
}
