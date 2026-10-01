#pragma once

#include "tabengine/types.h"

#include <functional>
#include <string>
#include <string_view>

namespace tabengine {

enum class EventType {
    Paint, Resized, PointerDown, PointerMove, PointerUp, PointerLeave, CaptureLost,
    KeyDown, TextInput, PointerWheel, CloseRequested, Moving,
    WindowActivated, WindowDeactivated, DpiChanged, PlacementChanged,
    AnimationFrame,
    // IME composition, delivered straight to IClient::body_event like
    // TextInput (no shortcut hook, no default bindings). ImeStart opens a
    // composition session; ImeUpdate carries the current preedit string;
    // ImeCommit carries the final string (insert it as text); ImeCancel
    // reports the session ended without a commit.
    ImeStart, ImeUpdate, ImeCommit, ImeCancel,
    // KeyUp mirrors KeyDown for clients that track held keys. No shortcut
    // hook runs on it and the engine binds nothing to releases.
    KeyUp,
    // MoveEnded reports a stepwise move session's completion (see
    // IPlatform::begin_move_session); the native move loop returns directly
    // instead and never emits this.
    MoveEnded
};

enum class MoveLoopResult { Unsupported, Completed, Canceled };

// Which physical pointer button produced a pointer press or release. Moves
// carry the button currently held (None while hovering with no button down).
enum class PointerButton : unsigned char { None, Left, Right, Middle };

// Full held-button state for pointer moves and releases. The single-button
// `Event::button` stays authoritative for which button produced a press.
inline constexpr unsigned kPointerButtonLeft = 1u;
inline constexpr unsigned kPointerButtonRight = 2u;
inline constexpr unsigned kPointerButtonMiddle = 4u;

struct Event {
    EventType type;
    WindowId window = 0;
    Point client{};
    Point screen{};
    Size size{};
    int key = 0;
    // Keyboard state carried on KeyDown (the pressed key's modifiers) and on
    // pointer presses (Ctrl/Shift-click selection).
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    // TextInput only: one completed Unicode code point. Characters are text,
    // never shortcuts: Shell forwards TextInput straight to IClient::body_event
    // without the handle_shortcut hook or the engine's default key bindings.
    char32_t code_point = 0;
    // PointerWheel only: signed vertical wheel delta in native units (+120 per
    // notch on Windows). The position fields carry the wheel's client point.
    int wheel = 0;
    // ImeUpdate / ImeCommit only: the composition (preedit) or committed
    // string, UTF-8. An empty ImeUpdate means the preedit emptied; clients
    // replace their preview with the string verbatim.
    std::string ime_text;
    // PointerDown/PointerUp: the physical button of this press/release.
    // PointerMove: the held button (None when hovering). Left by default so
    // plain presses and simple injection sites stay left clicks.
    PointerButton button = PointerButton::Left;
    // PointerWheel only: signed horizontal wheel delta in native units
    // (+120 per notch). The vertical component stays in `wheel`.
    int wheel_x = 0;
    // PointerDown/PointerUp: 1 for a plain click, 2 when the platform
    // recognized a double click (WM_*BUTTONDBLCLK) or an automation
    // synthesizer emitted the second press of an explicit multi-click.
    int click_count = 0;
    // PointerMove/PointerUp: the complete set of held buttons as a bitmask
    // of kPointerButton*. Supersedes `button` for state tracking; a plain
    // left press leaves it 0 for compatibility with existing consumers.
    unsigned buttons = 0;
    // KeyDown only: true when the system repeat generated this event.
    bool repeat = false;
    // MoveEnded only: how the stepwise move session finished.
    MoveLoopResult move_result = MoveLoopResult::Canceled;
};

// UTF-16 backends receive an astral character as a high + low surrogate pair
// across two messages. Feed each unit; a code point completes when feed()
// returns true. A dangling high surrogate before a non-surrogate unit is
// dropped; a lone low surrogate is also dropped because it is not a Unicode
// scalar value and cannot be encoded as valid UTF-8 by clients.
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
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return false;
        }
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
    // Cross-thread wake for background work: a worker thread finished
    // something the UI thread must drain. wake() is safe from any thread and
    // never blocks; the handler set here runs on the thread that pumps the
    // platform loop, never inline on the waking thread. Wakes may coalesce,
    // so handlers must drain pending state rather than count calls.
    // set_wake_handler is UI-thread only.
    virtual void set_wake_handler(std::function<void()> handler) = 0;
    virtual void wake() = 0;
    // Caret rectangle (window client pixels) the IME candidate window should
    // sit next to; a backend queries this when a composition session opens.
    // Return an empty rect when no editor has focus (the system default
    // placement applies). UI-thread only, called synchronously from the
    // platform layer.
    virtual void set_ime_caret_provider(std::function<Rect(WindowId)> provider) {}
    // Optional Windows UI Automation delegation: the backend answers
    // WM_GETOBJECT for the UIA root object by handing the registered
    // provider to UiaReturnRawElementProvider. The engine never interprets
    // the pointer (the application owns the provider object and its
    // lifetime); pass nullptr to clear. UI-thread only.
    virtual void set_uia_root_provider(WindowId id, void* provider) {
        (void)id;
        (void)provider;
    }
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
    // Remove the window from the screen without destroying it - resident
    // instances stay alive (surfaces, tabs, input sessions) while hidden.
    // Restore with show(); unlike minimize, hiding never animates or
    // activates anything. Absent by default.
    virtual void hide(WindowId id) { (void)id; }
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

    // ---- optional capabilities (default implementations report absence) ----

    // Bring the window to the foreground and give it keyboard focus.
    virtual void activate(WindowId id) { (void)id; }
    // Move and resize the window so its client area occupies the given
    // screen-pixel rectangle. False when the platform cannot do this.
    virtual bool set_client_bounds(WindowId id, Rect screen_bounds) {
        (void)id;
        (void)screen_bounds;
        return false;
    }
    // Stepwise move sessions let logic-driven drags (automation) run the
    // same tear-off state machine without entering the synchronous OS move
    // loop, which would block the UI thread. When begin_move_session
    // returns true the session is armed: Moving events drive the window,
    // end_move_session (or end_native_move_loop from an attach request)
    // finishes it, and completion arrives as a MoveEnded event. Platforms
    // without support return false and Shell falls back to the native loop.
    virtual bool begin_move_session(WindowId id) {
        (void)id;
        return false;
    }
    virtual void end_move_session(WindowId id, MoveLoopResult result) {
        (void)id;
        (void)result;
    }
    // One-shot delayed UI wake, for hosts and subsystems that poll with a
    // deadline (automation wait conditions). May coalesce: a newer request
    // replaces the pending one. Default: unsupported; callers then only
    // make progress on other activity.
    virtual void request_delayed_wake(double delay_seconds) { (void)delay_seconds; }

    virtual int run() = 0;
};

} // namespace tabengine
