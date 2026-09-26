// The pointer button contract on the real Win32 backend: posted mouse
// messages walk the actual wndproc and leave as PointerDown/Up/Move events
// carrying the physical button, the client point and the modifier keys.
// Right-button releases return 0, so DefWindowProc never synthesizes
// WM_CONTEXTMENU - the product owns context menus.

#include "tabengine/win32_platform.h"

#include "check.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <utility>
#include <vector>

namespace {

struct Recorded {
    tabengine::EventType type{};
    tabengine::Point client{};
    tabengine::PointerButton button{};
    bool ctrl = false;
    bool shift = false;
};

} // namespace

int main() {
    auto platform = tabengine::make_win32_platform();
    std::vector<Recorded> events;
    platform->set_event_handler([&](const tabengine::Event& event) {
        if (event.type == tabengine::EventType::PointerDown ||
            event.type == tabengine::EventType::PointerUp ||
            event.type == tabengine::EventType::PointerMove) {
            events.push_back({event.type, event.client, event.button, event.ctrl,
                              event.shift});
        }
    });

    CHECK(platform->create(1, {0, 0, 200, 100}, "pointer", false));
    HWND hwnd = static_cast<HWND>(platform->native_handle(1));
    CHECK(hwnd != nullptr);
    events.clear();

    // Left stays left (regression for the unified message path).
    SendMessageW(hwnd, WM_LBUTTONDOWN, 0, MAKELPARAM(30, 40));
    CHECK(events.size() == 1);
    CHECK(events[0].type == tabengine::EventType::PointerDown);
    CHECK(events[0].button == tabengine::PointerButton::Left);
    CHECK(events[0].client.x == 30 && events[0].client.y == 40);
    CHECK(!events[0].ctrl && !events[0].shift);
    SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(30, 40));
    CHECK(events.size() == 2);
    CHECK(events[1].type == tabengine::EventType::PointerUp);
    CHECK(events[1].button == tabengine::PointerButton::Left);

    // Right press/release keep their button and coordinates.
    SendMessageW(hwnd, WM_RBUTTONDOWN, 0, MAKELPARAM(60, 70));
    CHECK(events.size() == 3);
    CHECK(events[2].type == tabengine::EventType::PointerDown);
    CHECK(events[2].button == tabengine::PointerButton::Right);
    CHECK(events[2].client.x == 60 && events[2].client.y == 70);
    SendMessageW(hwnd, WM_RBUTTONUP, 0, MAKELPARAM(60, 70));
    CHECK(events.size() == 4);
    CHECK(events[3].type == tabengine::EventType::PointerUp);
    CHECK(events[3].button == tabengine::PointerButton::Right);

    // Middle press maps to the generic third button.
    SendMessageW(hwnd, WM_MBUTTONDOWN, 0, MAKELPARAM(10, 10));
    CHECK(events.size() == 5);
    CHECK(events[4].type == tabengine::EventType::PointerDown);
    CHECK(events[4].button == tabengine::PointerButton::Middle);
    SendMessageW(hwnd, WM_MBUTTONUP, 0, MAKELPARAM(10, 10));
    CHECK(events.size() == 6);

    // Moves report the held button, or None while hovering.
    SendMessageW(hwnd, WM_MOUSEMOVE, MK_RBUTTON, MAKELPARAM(80, 90));
    CHECK(events.size() == 7);
    CHECK(events[6].type == tabengine::EventType::PointerMove);
    CHECK(events[6].button == tabengine::PointerButton::Right);
    SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(80, 90));
    CHECK(events.size() == 8);
    CHECK(events[7].button == tabengine::PointerButton::Left);
    SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(80, 90));
    CHECK(events.size() == 9);
    CHECK(events[8].button == tabengine::PointerButton::None);
    return 0;
}
