// The IME contract on the real Win32 backend: posted WM_IME_* messages walk
// the actual wndproc - composition sessions open and close, the preedit and
// result strings become events, WM_CHAR stays silenced while composing (the
// dedup rule), and the candidate window follows the client caret provider.
// Only the string extraction itself needs a live IME (manual acceptance);
// every state transition here is the real message path.

#include "tabengine/win32_platform.h"

#include "check.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <imm.h>

#include <string>
#include <utility>
#include <vector>

namespace {

struct Recorded {
    tabengine::EventType type{};
    std::string text;
};

} // namespace

int main() {
    auto platform = tabengine::make_win32_platform();
    std::vector<Recorded> events;
    platform->set_event_handler([&](const tabengine::Event& event) {
        events.push_back({event.type, event.ime_text});
    });
    int caret_queries = 0;
    platform->set_ime_caret_provider([&](tabengine::WindowId) {
        ++caret_queries;
        return tabengine::Rect{10, 20, 2, 24};
    });

    CHECK(platform->create(1, {0, 0, 200, 100}, "ime", false));
    HWND hwnd = static_cast<HWND>(platform->native_handle(1));
    CHECK(hwnd != nullptr);
    events.clear(); // creation itself emits lifecycle events synchronously

    // A full composition session: open, preedit updates, commit, close.
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    CHECK(events.size() == 1 && events[0].type == tabengine::EventType::ImeStart);
    CHECK(caret_queries == 1); // the provider positioned the session
    // WM_CHAR during composition is the IME echo: no TextInput may leak.
    SendMessageW(hwnd, WM_CHAR, 'x', 0);
    CHECK(events.size() == 1);
    // Preedit update (empty from the bare context - the event is the point).
    SendMessageW(hwnd, WM_IME_COMPOSITION, 0, GCS_COMPSTR);
    CHECK(events.size() == 2 && events[1].type == tabengine::EventType::ImeUpdate);
    // Repeated identical updates are re-delivered verbatim; clients dedup.
    SendMessageW(hwnd, WM_IME_COMPOSITION, 0, GCS_COMPSTR);
    CHECK(events.size() == 3 && events[2].type == tabengine::EventType::ImeUpdate);
    // The result string commits the session.
    SendMessageW(hwnd, WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
    CHECK(events.size() == 4 && events[3].type == tabengine::EventType::ImeCommit);
    // Closing a committed session is NOT a cancel.
    SendMessageW(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
    CHECK(events.size() == 4);
    // Dedup window closed: plain characters flow again.
    SendMessageW(hwnd, WM_CHAR, 'y', 0);
    CHECK(events.size() == 5 && events[4].type == tabengine::EventType::TextInput);

    // A canceled session: open, then end without a result string.
    events.clear();
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(hwnd, WM_IME_COMPOSITION, 0, GCS_COMPSTR);
    SendMessageW(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
    CHECK(events.size() == 3);
    CHECK(events[0].type == tabengine::EventType::ImeStart);
    CHECK(events[1].type == tabengine::EventType::ImeUpdate);
    CHECK(events[2].type == tabengine::EventType::ImeCancel);

    // A composition message with neither result nor preedit bits is noise.
    events.clear();
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(hwnd, WM_IME_COMPOSITION, 0, 0);
    CHECK(events.size() == 1 && events[0].type == tabengine::EventType::ImeStart);
    SendMessageW(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
    CHECK(events.size() == 2 && events[1].type == tabengine::EventType::ImeCancel);
    return 0;
}
