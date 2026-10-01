// Win32 platform behavior against real (never shown) windows: the
// client-bounds contract of set_client_bounds, and later the hide/show
// placement pair. Follows the hidden-HWND convention of
// windows_renderer_test.cpp - no visible window, no real input.

#include "tabengine/win32_platform.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#pragma comment(lib, "UIAutomationCore.lib")

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

// A hidden window keeps its client geometry and accepts new placement -
// the resident-instance contract behind Window.setPlacement hide.
int check_hide_show(tabengine::IPlatform& platform) {
    constexpr tabengine::WindowId id = 2;
    if (!platform.create(id, {80, 90, 300, 200}, "hide test", /*visible=*/false)) {
        return fail("create");
    }
    platform.hide(id);
    const tabengine::Size hidden_size = platform.client_size(id);
    if (hidden_size.width != 300 || hidden_size.height != 200) {
        std::printf("FAIL hidden client size %dx%d, want 300x200\n", hidden_size.width,
                    hidden_size.height);
        platform.destroy(id);
        return 1;
    }
    if (!platform.set_client_bounds(id, {10, 20, 240, 160})) {
        platform.destroy(id);
        return fail("set_client_bounds while hidden");
    }
    platform.show(id);
    const tabengine::Size shown_size = platform.client_size(id);
    if (shown_size.width != 240 || shown_size.height != 160) {
        std::printf("FAIL shown client size %dx%d, want 240x160\n", shown_size.width,
                    shown_size.height);
        platform.destroy(id);
        return 1;
    }
    platform.destroy(id);
    return 0;
}

// A minimal IRawElementProviderSimple stand-in: UiaReturnRawElementProvider
// only passes the pointer through, so the delegation test needs a stable
// address, not a working provider.
struct StandInProvider : IRawElementProviderSimple {
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 0; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID, VARIANT*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(
        IRawElementProviderSimple**) override {
        return E_NOTIMPL;
    }
};

int check_uia_root_delegation(tabengine::IPlatform& platform) {
    constexpr tabengine::WindowId id = 41;
    if (!platform.create(id, {40, 50, 360, 240}, "uia test", false)) {
        return fail("create (uia)");
    }
    HWND hwnd = static_cast<HWND>(platform.native_handle(id));
    // Without a provider the message keeps its default answer.
    const LRESULT unregistered =
        SendMessageW(hwnd, WM_GETOBJECT, 0, static_cast<LPARAM>(UiaRootObjectId));
    // With one, the window returns the UIA element handle for it.
    StandInProvider stand_in;
    platform.set_uia_root_provider(id, &stand_in);
    const LRESULT delegated =
        SendMessageW(hwnd, WM_GETOBJECT, 0, static_cast<LPARAM>(UiaRootObjectId));
    // Clearing restores the default answer.
    platform.set_uia_root_provider(id, nullptr);
    const LRESULT cleared =
        SendMessageW(hwnd, WM_GETOBJECT, 0, static_cast<LPARAM>(UiaRootObjectId));
    platform.destroy(id);
    if (delegated == 0) {
        std::printf("FAIL uia delegation returned 0\n");
        return 1;
    }
    if (cleared != unregistered) {
        std::printf("FAIL uia clear did not restore the default answer\n");
        return 1;
    }
    std::printf("uia root delegation passed\n");
    return 0;
}

int main() {
    std::unique_ptr<tabengine::IPlatform> platform = tabengine::make_win32_platform();
    if (!platform) return fail("make_win32_platform");
    if (const int result = check_set_client_bounds(*platform)) return result;
    if (const int result = check_hide_show(*platform)) return result;
    if (const int result = check_uia_root_delegation(*platform)) return result;
    std::printf("win32 platform bounds test passed\n");
    return 0;
}
