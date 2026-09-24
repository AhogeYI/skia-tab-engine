#pragma once

#include "tabengine/types.h"

#include <functional>
#include <string_view>

namespace tabengine {

enum class EventType {
    Paint, Resized, PointerDown, PointerMove, PointerUp, PointerLeave, CaptureLost,
    KeyDown, TextInput, CloseRequested, Moving,
    WindowActivated, WindowDeactivated, DpiChanged, PlacementChanged,
    AnimationFrame
};

enum class MoveLoopResult { Unsupported, Completed, Canceled };

struct Event {
    EventType type;
    WindowId window = 0;
    Point client{};
    Point screen{};
    Size size{};
    int key = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    // TextInput only: one completed Unicode code point. Characters are text,
    // never shortcuts: Shell forwards TextInput straight to IClient::body_event
    // without the handle_shortcut hook or the engine's default key bindings.
    char32_t code_point = 0;
};

// UTF-16 backends receive an astral character as a high + low surrogate pair
// across two messages. Feed each unit; a code point completes when feed()
// returns true. A dangling high surrogate before a non-surrogate unit is
// dropped; a lone low surrogate passes through unchanged.
struct SurrogateComposer {
    char32_t pending_high = 0;
    char32_t code_point = 0;
    bool feed(char32_t unit) {
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            pending_high = unit;
            return false;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF && pending_high != 0) {
            code_point = 0x10000 + ((pending_high - 0xD800) << 10) + (unit - 0xDC00);
            pending_high = 0;
            return true;
        }
        pending_high = 0;
        code_point = unit;
        return true;
    }
};

// The platform contract deliberately contains no HWND, NSWindow, or X11 type.
// A backend may report no native move loop and use system drag-and-drop instead.
class IPlatform {
public:
    virtual ~IPlatform() = default;
    virtual void set_event_handler(std::function<void(const Event&)> handler) = 0;
    virtual void set_caption_hit_handler(std::function<bool(WindowId, Point)> handler) = 0;
    virtual bool create(WindowId id, Rect bounds, std::string_view title, bool visible) = 0;
    virtual void show(WindowId id) = 0;
    virtual void destroy(WindowId id) = 0;
    virtual void invalidate(WindowId id) = 0;
    // Monotonic time and one-shot frame scheduling keep animation policy in Shell.
    [[nodiscard]] virtual double monotonic_seconds() const = 0;
    virtual void request_animation_frame(WindowId id) = 0;
    virtual void capture_pointer(WindowId id) = 0;
    virtual void release_pointer() = 0;
    virtual void minimize(WindowId id) = 0;
    virtual void toggle_maximize(WindowId id) = 0;
    [[nodiscard]] virtual Size client_size(WindowId id) const = 0;
    [[nodiscard]] virtual Point client_origin(WindowId id) const = 0;
    [[nodiscard]] virtual float scale(WindowId id) const = 0;
    [[nodiscard]] virtual void* native_handle(WindowId id) const = 0;
    [[nodiscard]] virtual WindowId window_at(Point screen, WindowId excluded) const = 0;
    [[nodiscard]] virtual bool supports_native_move_loop() const = 0;
    virtual void set_client_origin(WindowId id, Point screen) = 0;
    // Runs synchronously. A programmatic end may report Canceled even when
    // Shell requested it to attach the tab to another window.
    [[nodiscard]] virtual MoveLoopResult run_native_move_loop(WindowId id) = 0;
    virtual void end_native_move_loop(WindowId id) = 0;
    virtual int run() = 0;
};

} // namespace tabengine
