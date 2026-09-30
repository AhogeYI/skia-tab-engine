// Win32 platform behavior against real (never shown) windows: the
// client-bounds contract of set_client_bounds, and later the hide/show
// placement pair. Follows the hidden-HWND convention of
// windows_renderer_test.cpp - no visible window, no real input.

#include "tabengine/win32_platform.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <memory>

namespace {

int fail(const char* what) {
    std::printf("FAIL %s (GetLastError=%lu)\n", what, GetLastError());
    return 1;
}

} // namespace

int check_set_client_bounds(tabengine::IPlatform& platform) {
    constexpr tabengine::WindowId id = 1;
    if (!platform.create(id, {40, 50, 360, 240}, "bounds test", /*visible=*/false)) {
        return fail("create");
    }
    tabengine::Size size = platform.client_size(id);
    if (size.width != 360 || size.height != 240) {
        std::printf("FAIL initial client size %dx%d, want 360x240\n", size.width, size.height);
        return 1;
    }

    if (!platform.set_client_bounds(id, {100, 120, 500, 350})) {
        platform.destroy(id);
        return fail("set_client_bounds");
    }
    const tabengine::Point origin = platform.client_origin(id);
    size = platform.client_size(id);
    if (origin.x != 100 || origin.y != 120) {
        std::printf("FAIL client origin (%d,%d), want (100,120)\n", origin.x, origin.y);
        platform.destroy(id);
        return 1;
    }
    // The custom frame keeps client == window rect, so the placement is
    // exact (regression: AdjustWindowRectExForDpi margins used to shift
    // real windows by the frame size).
    if (size.width != 500 || size.height != 350) {
        std::printf("FAIL client size %dx%d, want 500x350\n", size.width, size.height);
        platform.destroy(id);
        return 1;
    }

    // A second move to a different rectangle keeps the contract.
    if (!platform.set_client_bounds(id, {260, 40, 200, 150})) {
        platform.destroy(id);
        return fail("set_client_bounds (second)");
    }
    const tabengine::Point second_origin = platform.client_origin(id);
    const tabengine::Size second_size = platform.client_size(id);
    if (second_origin.x != 260 || second_origin.y != 40 || second_size.width != 200 ||
        second_size.height != 150) {
        std::printf("FAIL second bounds: origin (%d,%d) size %dx%d, want (260,40) 200x150\n",
                    second_origin.x, second_origin.y, second_size.width, second_size.height);
        platform.destroy(id);
        return 1;
    }
    platform.destroy(id);
    return 0;
}

int main() {
    std::unique_ptr<tabengine::IPlatform> platform = tabengine::make_win32_platform();
    if (!platform) return fail("make_win32_platform");
    if (const int result = check_set_client_bounds(*platform)) return result;
    std::printf("win32 platform bounds test passed\n");
    return 0;
}
