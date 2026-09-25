#include "tabengine/win32_platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tabengine {
namespace {

constexpr wchar_t kClassName[] = L"TabEngineWindow";
constexpr wchar_t kWakeClassName[] = L"TabEngineWake";
constexpr UINT_PTR kAnimationTimer = 1;

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                         nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    if (size) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                 out.data(), size);
    return out;
}

class Win32Platform final : public IPlatform {
public:
    Win32Platform() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = &Win32Platform::wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
        // Message-only window: its only job is to own the cross-thread wake
        // so a PostMessage can break the GetMessageW sleep.
        WNDCLASSEXW wake_wc{};
        wake_wc.cbSize = sizeof(wake_wc);
        wake_wc.lpfnWndProc = &Win32Platform::wake_proc;
        wake_wc.hInstance = GetModuleHandleW(nullptr);
        wake_wc.lpszClassName = kWakeClassName;
        RegisterClassExW(&wake_wc);
        wake_hwnd_ = CreateWindowExW(0, kWakeClassName, L"", WS_OVERLAPPED, 0, 0, 0, 0,
                                     HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), this);
    }

    ~Win32Platform() override {
        flush_destroy();
        while (!windows_.empty()) destroy(windows_.begin()->first);
        if (wake_hwnd_) {
            DestroyWindow(wake_hwnd_);
            wake_hwnd_ = nullptr;
        }
    }

    void set_event_handler(std::function<void(const Event&)> handler) override {
        events_ = std::move(handler);
    }

    void set_caption_hit_handler(std::function<bool(WindowId, Point)> handler) override {
        caption_hit_ = std::move(handler);
    }

    void set_wake_handler(std::function<void()> handler) override {
        wake_ = std::move(handler);
    }

    void wake() override {
        // Post, never send: a synchronous SendMessage would run the handler on
        // the waking thread instead of the pumping thread.
        if (wake_hwnd_) PostMessageW(wake_hwnd_, WM_APP, 0, 0);
    }

    bool create(WindowId id, Rect bounds, std::string_view title, bool visible) override {
        if (windows_.contains(id)) return false;
        auto native = std::make_unique<Native>();
        native->platform = this;
        native->id = id;
        const std::wstring wide = widen(title);
        native->hwnd = CreateWindowExW(0, kClassName, wide.c_str(), WS_OVERLAPPEDWINDOW,
                                       bounds.x, bounds.y, bounds.width, bounds.height,
                                       nullptr, nullptr, GetModuleHandleW(nullptr), native.get());
        if (!native->hwnd) return false;
        const HWND hwnd = native->hwnd;
        windows_.emplace(id, std::move(native));
        MARGINS margins{0, 0, 1, 0};
        DwmExtendFrameIntoClientArea(hwnd, &margins);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (visible) show(id);
        return true;
    }

    void show(WindowId id) override {
        if (HWND hwnd = handle(id)) {
            ShowWindow(hwnd, SW_SHOW);
            UpdateWindow(hwnd);
        }
    }

    void destroy(WindowId id) override {
        if (emitting_ > 0) {
            if (std::find(pending_destroy_.begin(), pending_destroy_.end(), id) ==
                pending_destroy_.end()) pending_destroy_.push_back(id);
            // A synchronous SendMessage may invoke the callback while GetMessage
            // is waiting. Wake the outer pump so it can destroy after WndProc.
            if (HWND hwnd = handle(id)) PostMessageW(hwnd, WM_NULL, 0, 0);
            return;
        }
        destroy_now(id);
    }

    void destroy_now(WindowId id) {
        auto it = windows_.find(id);
        if (it == windows_.end()) return;
        HWND hwnd = it->second->hwnd;
        if (hwnd) DestroyWindow(hwnd);
        windows_.erase(it);
        if (windows_.empty()) PostQuitMessage(0);
    }

    void invalidate(WindowId id) override {
        if (HWND hwnd = handle(id)) InvalidateRect(hwnd, nullptr, FALSE);
    }

    double monotonic_seconds() const override {
        return static_cast<double>(GetTickCount64()) / 1000.0;
    }

    void request_animation_frame(WindowId id) override {
        auto it = windows_.find(id);
        if (it == windows_.end() || it->second->frame_pending) return;
        if (SetTimer(it->second->hwnd, kAnimationTimer, 16, nullptr))
            it->second->frame_pending = true;
    }

    void capture_pointer(WindowId id) override {
        if (HWND hwnd = handle(id)) SetCapture(hwnd);
    }

    void release_pointer() override {
        if (GetCapture()) ReleaseCapture();
    }

    void minimize(WindowId id) override {
        if (HWND hwnd = handle(id)) ShowWindow(hwnd, SW_MINIMIZE);
    }

    void toggle_maximize(WindowId id) override {
        if (HWND hwnd = handle(id)) ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    }

    Size client_size(WindowId id) const override {
        RECT rect{};
        if (HWND hwnd = handle(id)) GetClientRect(hwnd, &rect);
        return {rect.right - rect.left, rect.bottom - rect.top};
    }

    Point client_origin(WindowId id) const override {
        POINT point{};
        if (HWND hwnd = handle(id)) ClientToScreen(hwnd, &point);
        return {point.x, point.y};
    }

    float scale(WindowId id) const override {
        if (HWND hwnd = handle(id)) return static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
        return 1.0f;
    }

    void* native_handle(WindowId id) const override { return handle(id); }

    WindowId window_at(Point screen, WindowId excluded) const override {
        for (HWND hwnd = GetTopWindow(nullptr); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
            if (!IsWindowVisible(hwnd)) continue;
            WindowId candidate = 0;
            for (const auto& [id, native] : windows_) {
                if (native->hwnd == hwnd) {
                    candidate = id;
                    break;
                }
            }
            if (!candidate || candidate == excluded) continue;
            RECT rect{};
            GetWindowRect(hwnd, &rect);
            if (screen.x >= rect.left && screen.x < rect.right &&
                screen.y >= rect.top && screen.y < rect.bottom) return candidate;
        }
        return 0;
    }

    bool supports_native_move_loop() const override { return true; }

    void set_client_origin(WindowId id, Point screen) override {
        if (HWND hwnd = handle(id)) {
            POINT origin{};
            ClientToScreen(hwnd, &origin);
            RECT rect{};
            GetWindowRect(hwnd, &rect);
            SetWindowPos(hwnd, nullptr, rect.left + screen.x - origin.x,
                         rect.top + screen.y - origin.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    MoveLoopResult run_native_move_loop(WindowId id) override {
        auto it = windows_.find(id);
        if (it == windows_.end() || it->second->in_move_loop) return MoveLoopResult::Unsupported;
        Native& native = *it->second;
        native.in_move_loop = true;
        native.move_loop_canceled = false;
        native.move_loop_mouse_up = false;
        native.skip_first_moving = true;
        release_pointer();
        SendMessageW(native.hwnd, WM_SYSCOMMAND, SC_MOVE | 0x0002,
                     static_cast<LPARAM>(GetMessagePos()));
        const bool canceled = native.move_loop_canceled;
        const bool mouse_up = native.move_loop_mouse_up;
        native.in_move_loop = false;
        native.skip_first_moving = false;
        if (canceled && !mouse_up) return MoveLoopResult::Canceled;
        return mouse_up ? MoveLoopResult::Completed : MoveLoopResult::Canceled;
    }

    void end_native_move_loop(WindowId id) override {
        auto it = windows_.find(id);
        if (it != windows_.end() && it->second->in_move_loop) {
            it->second->move_loop_canceled = true;
            SendMessageW(it->second->hwnd, WM_CANCELMODE, 0, 0);
        }
    }

    int run() override {
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            flush_destroy();
        }
        flush_destroy();
        return static_cast<int>(msg.wParam);
    }

private:
    struct Native {
        Win32Platform* platform = nullptr;
        WindowId id = 0;
        HWND hwnd = nullptr;
        SurrogateComposer chars;
        bool in_move_loop = false;
        bool move_loop_canceled = false;
        bool move_loop_mouse_up = false;
        bool skip_first_moving = false;
        bool tracking_mouse = false;
        bool frame_pending = false;
    };

    HWND handle(WindowId id) const {
        auto it = windows_.find(id);
        return it == windows_.end() ? nullptr : it->second->hwnd;
    }

    void emit(Native& native, Event event) {
        if (!events_) return;
        event.window = native.id;
        event.size = client_size(native.id);
        ++emitting_;
        events_(event);
        --emitting_;
    }

    void flush_destroy() {
        auto pending = std::move(pending_destroy_);
        pending_destroy_.clear();
        for (WindowId id : pending) destroy_now(id);
    }

    static LRESULT CALLBACK wake_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return DefWindowProcW(hwnd, message, wp, lp);
        }
        auto* self = reinterpret_cast<Win32Platform*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_APP) {
            if (self && self->wake_) self->wake_();
            return 0;
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        Native* native = reinterpret_cast<Native*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
            native = static_cast<Native*>(create->lpCreateParams);
            native->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(native));
        }
        if (!native) return DefWindowProcW(hwnd, message, wp, lp);
        Win32Platform& self = *native->platform;
        switch (message) {
        case WM_NCCALCSIZE:
            if (wp) return 0;
            break;
        case WM_NCHITTEST: {
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            const int border = GetSystemMetricsForDpi(SM_CXFRAME, GetDpiForWindow(hwnd)) +
                               GetSystemMetricsForDpi(SM_CXPADDEDBORDER, GetDpiForWindow(hwnd));
            if (!IsZoomed(hwnd)) {
                const bool left = x < wr.left + border;
                const bool right = x >= wr.right - border;
                const bool top = y < wr.top + border;
                const bool bottom = y >= wr.bottom - border;
                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;
            }
            POINT client{x, y};
            ScreenToClient(hwnd, &client);
            if (self.caption_hit_ && self.caption_hit_(native->id, {client.x, client.y}))
                return HTCAPTION;
            return HTCLIENT;
        }
        case WM_SIZE:
            self.emit(*native, {EventType::Resized});
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            self.emit(*native, {EventType::Paint});
            return 0;
        }
        case WM_TIMER:
            if (wp == kAnimationTimer) {
                KillTimer(hwnd, kAnimationTimer);
                native->frame_pending = false;
                self.emit(*native, {EventType::AnimationFrame});
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_MOUSEMOVE:
        case WM_LBUTTONUP: {
            if (native->in_move_loop) {
                if (message == WM_LBUTTONUP) native->move_loop_mouse_up = true;
                return 0;
            }
            if (message == WM_MOUSEMOVE && !native->tracking_mouse) {
                TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
                native->tracking_mouse = TrackMouseEvent(&track) != FALSE;
            }
            POINT screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &screen);
            const EventType type = message == WM_LBUTTONDOWN ? EventType::PointerDown
                                  : message == WM_LBUTTONUP ? EventType::PointerUp
                                                            : EventType::PointerMove;
            // Modifier keys ride along so hosts can Ctrl/Shift-click without
            // touching Win32 themselves; same source as KeyDown.
            self.emit(*native, {type, native->id,
                                {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)},
                                {screen.x, screen.y}, {},
                                0, (GetKeyState(VK_CONTROL) & 0x8000) != 0,
                                (GetKeyState(VK_SHIFT) & 0x8000) != 0});
            return 0;
        }
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
            if (native->in_move_loop) {
                native->move_loop_mouse_up = true;
                return 0;
            }
            break;
        case WM_MOUSELEAVE:
            native->tracking_mouse = false;
            self.emit(*native, {EventType::PointerLeave});
            return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            // The wheel is delivered to the focused window with screen
            // coordinates; the body needs the client point.
            POINT screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &screen);
            if (message == WM_MOUSEHWHEEL) return 0; // horizontal wheels: none yet
            Event wheel;
            wheel.type = EventType::PointerWheel;
            wheel.client = {screen.x, screen.y};
            wheel.screen = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            wheel.wheel = GET_WHEEL_DELTA_WPARAM(wp);
            self.emit(*native, wheel);
            return 0;
        }
        case WM_CAPTURECHANGED:
            self.emit(*native, {EventType::CaptureLost});
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const bool alt_held = (GetKeyState(VK_MENU) & 0x8000) != 0;
            self.emit(*native, {EventType::KeyDown, native->id, {}, {}, {},
                                static_cast<int>(wp), (GetKeyState(VK_CONTROL) & 0x8000) != 0,
                                (GetKeyState(VK_SHIFT) & 0x8000) != 0, alt_held});
            // Alt+F4 (close) and Alt+Space (system menu) stay with the system;
            // every other Alt combo (browser-style Back/Forward) is app input.
            if (message == WM_SYSKEYDOWN) {
                if (wp != VK_F4 && wp != VK_SPACE) return 0;
                break;
            }
            return 0;
        }
        case WM_CHAR: {
            // WM_SYSCHAR is not translated: an Alt+letter mnemonic is a command,
            // not text. Astral characters arrive as two surrogate units.
            if (native->chars.feed(static_cast<char32_t>(wp))) {
                Event text;
                text.type = EventType::TextInput;
                text.code_point = native->chars.code_point;
                self.emit(*native, text);
            }
            return 0;
        }
        case WM_ACTIVATE:
            self.emit(*native, {LOWORD(wp) == WA_INACTIVE ? EventType::WindowDeactivated
                                                       : EventType::WindowActivated});
            break;
        case WM_CLOSE:
            self.emit(*native, {EventType::CloseRequested});
            return 0;
        case WM_NCLBUTTONUP:
            if (native->in_move_loop) native->move_loop_mouse_up = true;
            break;
        case WM_CANCELMODE:
            if (native->in_move_loop) native->move_loop_canceled = true;
            break;
        case WM_MOVING:
            if (native->in_move_loop) {
                if (native->skip_first_moving) native->skip_first_moving = false;
                else {
                    POINT screen{};
                    GetCursorPos(&screen);
                    self.emit(*native, {EventType::Moving, native->id, {},
                                        {screen.x, screen.y}});
                }
            }
            return TRUE;
        case WM_EXITSIZEMOVE:
            self.emit(*native, {EventType::PlacementChanged});
            break;
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            self.emit(*native, {EventType::DpiChanged});
            return 0;
        }
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            native->hwnd = nullptr;
            break;
        default: break;
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }

    std::unordered_map<WindowId, std::unique_ptr<Native>> windows_;
    std::function<void(const Event&)> events_;
    std::function<bool(WindowId, Point)> caption_hit_;
    std::function<void()> wake_;
    HWND wake_hwnd_ = nullptr;
    std::vector<WindowId> pending_destroy_;
    int emitting_ = 0;
};

} // namespace

std::unique_ptr<IPlatform> make_win32_platform() {
    return std::make_unique<Win32Platform>();
}

} // namespace tabengine
