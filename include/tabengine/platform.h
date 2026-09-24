#pragma once

#include "tabengine/types.h"

#include <functional>
#include <string_view>

namespace tabengine {

enum class EventType {
    Paint, Resized, PointerDown, PointerMove, PointerUp, PointerLeave, CaptureLost,
    KeyDown, CloseRequested, Moving, NativeMoveEnded
};

struct Event {
    EventType type;
    WindowId window = 0;
    Point client{};
    Point screen{};
    Size size{};
    int key = 0;
    bool ctrl = false;
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
    virtual void run_native_move_loop(WindowId id) = 0;
    virtual void end_native_move_loop(WindowId id) = 0;
    virtual int run() = 0;
};

} // namespace tabengine
