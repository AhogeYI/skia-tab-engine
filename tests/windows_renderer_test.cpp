#include "tabengine/render.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int main() {
    // A real, hidden HWND exercises swapchain creation and raster fallback
    // without opening a visible test window or requiring a mouse/keyboard.
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"TabEngine renderer test", WS_OVERLAPPEDWINDOW,
                                0, 0, 360, 240, nullptr, nullptr, GetModuleHandleW(nullptr),
                                nullptr);
    if (!hwnd) return 1;
    auto renderer = tabengine::make_skia_windows_renderer();
    constexpr tabengine::WindowId id = 1;
    if (!renderer->attach(id, hwnd, {320, 200})) return 2;
    SkCanvas* canvas = renderer->canvas(id);
    if (!canvas) return 3;
    canvas->clear(SK_ColorRED);
    renderer->present(id, hwnd);
    renderer->resize(id, {480, 300});
    canvas = renderer->canvas(id);
    if (!canvas) return 4;
    canvas->clear(SK_ColorBLUE);
    renderer->present(id, hwnd);
    renderer->detach(id);
    DestroyWindow(hwnd);
    return 0;
}
