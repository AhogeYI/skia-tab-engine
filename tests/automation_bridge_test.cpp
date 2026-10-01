// Shell bridge integration: real protocol calls through the runtime against
// a faked platform and renderer. Covers Window/Tab commands, semantic
// snapshots, node-mode clicks, input sessions, and in-strip drags.

#include "tabengine/automation/shell_bridge.h"
#include "tabengine/automation/transport.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/encode/SkPngEncoder.h"

#include "check.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace automation = tabengine::automation;
using automation::AutomationRuntime;
using automation::InstanceIdentity;
using automation::JsonValue;
using automation::RuntimeBudgets;
using automation::ShellAutomationBridge;

namespace {

struct Clock {
    double now_value = 500.0;
    double operator()() const { return now_value; }
};

class FakeRenderer final : public tabengine::IRenderer {
public:
    bool capture_supported = true;

    bool attach(tabengine::WindowId id, void*, tabengine::Size size) override {
        info_[id] = {tabengine::RenderBackend::Raster, size};
        return true;
    }
    void resize(tabengine::WindowId id, tabengine::Size size) override {
        info_[id].surface_size = size;
    }
    void detach(tabengine::WindowId id) override { info_.erase(id); }
    SkCanvas* canvas(tabengine::WindowId) override { return nullptr; }
    void present(tabengine::WindowId, void*) override {}
    tabengine::RenderInfo info(tabengine::WindowId id) const override {
        const auto it = info_.find(id);
        return it == info_.end() ? tabengine::RenderInfo{} : it->second;
    }
    bool supports_capture() const override { return capture_supported; }
    // Produces a deterministic synthetic frame: base color everywhere plus
    // sentinel pixels at known positions for byte-exact crop checks.
    bool capture_frame(tabengine::WindowId id, tabengine::CapturedFrame& out) override {
        const auto it = info_.find(id);
        if (it == info_.end()) return false;
        const int width = it->second.surface_size.width;
        const int height = it->second.surface_size.height;
        if (width <= 0 || height <= 0) return false;
        out.backend = tabengine::RenderBackend::Raster;
        out.size = {width, height};
        out.pixels.assign(static_cast<std::size_t>(width) * height * 4, 0);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                std::uint8_t* pixel =
                    out.pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
                const std::uint32_t color =
                    (x == 10 && y == 20)                 ? 0xFF00FF00u   // opaque green (BGRA)
                    : (x == width - 1 && y == height - 1) ? 0xFFFF0000u
                                                          : 0xFF202080u;
                pixel[0] = static_cast<std::uint8_t>(color & 0xFF);
                pixel[1] = static_cast<std::uint8_t>((color >> 8) & 0xFF);
                pixel[2] = static_cast<std::uint8_t>((color >> 16) & 0xFF);
                pixel[3] = static_cast<std::uint8_t>((color >> 24) & 0xFF);
            }
        }
        return true;
    }

private:
    std::map<tabengine::WindowId, tabengine::RenderInfo> info_;
};

class FakePlatform final : public tabengine::IPlatform {
public:
    double platform_now = 100.0;
    void set_event_handler(std::function<void(const tabengine::Event&)> handler) override {
        handler_ = std::move(handler);
    }
    void set_caption_hit_handler(std::function<bool(tabengine::WindowId, tabengine::Point)>)
        override {}
    void set_wake_handler(std::function<void()>) override {}
    void wake() override {}
    bool create(tabengine::WindowId id, tabengine::Rect bounds, std::string_view, bool) override {
        windows_.emplace(id, bounds);
        return true;
    }
    void show(tabengine::WindowId id) override { shown_.push_back(id); }
    void destroy(tabengine::WindowId id) override { windows_.erase(id); }
    void invalidate(tabengine::WindowId) override {}
    double monotonic_seconds() const override { return platform_now; }
    void request_animation_frame(tabengine::WindowId) override {}
    void capture_pointer(tabengine::WindowId) override {}
    void release_pointer() override {}
    void minimize(tabengine::WindowId id) override { minimized_.push_back(id); }
    void toggle_maximize(tabengine::WindowId) override {}
    void hide(tabengine::WindowId id) override { hidden_.push_back(id); }
    tabengine::Size client_size(tabengine::WindowId id) const override {
        const auto& rect = windows_.at(id);
        return {rect.width, rect.height};
    }
    tabengine::Point client_origin(tabengine::WindowId id) const override {
        const auto& rect = windows_.at(id);
        return {rect.x, rect.y};
    }
    float scale(tabengine::WindowId) const override { return 1.0f; }
    void* native_handle(tabengine::WindowId) const override { return nullptr; }
    tabengine::WindowId window_at(tabengine::Point, tabengine::WindowId) const override {
        return 0;
    }
    bool supports_native_move_loop() const override { return true; }
    void set_client_origin(tabengine::WindowId id, tabengine::Point screen) override {
        windows_.at(id).x = screen.x;
        windows_.at(id).y = screen.y;
    }
    tabengine::MoveLoopResult run_native_move_loop(tabengine::WindowId) override {
        return tabengine::MoveLoopResult::Completed;
    }
    void end_native_move_loop(tabengine::WindowId) override {}
    bool begin_move_session(tabengine::WindowId) override { return true; }
    void end_move_session(tabengine::WindowId id, tabengine::MoveLoopResult result) override {
        ++move_session_ends;
        if (inject_competing_move && handler_) {
            // Real input landing mid-session, before the MoveEnded arrives:
            // exercises re-entrant dispatch from inside the drag driver.
            tabengine::Event competing{tabengine::EventType::PointerMove};
            competing.window = id;
            competing.client = {5, 5};
            handler_(competing);
        }
        // Mirrors the real backend: completion arrives as a MoveEnded event.
        tabengine::Event ended{tabengine::EventType::MoveEnded};
        ended.window = id;
        ended.move_result = result;
        if (handler_) handler_(ended);
    }
    bool set_client_bounds(tabengine::WindowId id, tabengine::Rect bounds) override {
        windows_.at(id) = bounds;
        return true;
    }
    void activate(tabengine::WindowId id) override { activated_.push_back(id); }
    int run() override { return 0; }

    std::vector<tabengine::WindowId> activated_;
    std::vector<tabengine::WindowId> minimized_;
    std::vector<tabengine::WindowId> hidden_;
    std::vector<tabengine::WindowId> shown_;
    bool inject_competing_move = false;
    int move_session_ends = 0;

private:
    std::unordered_map<tabengine::WindowId, tabengine::Rect> windows_;
    std::function<void(const tabengine::Event&)> handler_;
};

class CountingClient final : public tabengine::IClient {
public:
    tabengine::NewTab create_tab() override {
        return {next_content_++, "Tab " + std::to_string(next_content_)};
    }
    std::string window_title() override { return "Bridge Test"; }
    void tab_closed(tabengine::ContentId content) override { closed_.push_back(content); }
    void paint_body(tabengine::WindowId, tabengine::TabId, SkCanvas&, tabengine::Rect) override {}
    void body_event(const tabengine::Event& event, tabengine::Rect) override {
        ++body_events_;
        if (event.type == tabengine::EventType::PointerMove) {
            last_move_buttons = event.buttons;
            last_move_button = static_cast<int>(event.button);
        }
        if (event.type == tabengine::EventType::PointerDown ||
            event.type == tabengine::EventType::PointerUp) {
            body_click_counts.push_back(
                {event.type == tabengine::EventType::PointerDown ? "down" : "up",
                 event.click_count});
        }
    }
    // ("down"|"up", click_count) for body pointer events, in arrival order.
    std::vector<std::pair<std::string, int>> body_click_counts;
    unsigned last_move_buttons = 0;
    int last_move_button = 0;
    bool allow_close_window(tabengine::WindowId window) override {
        return veto_windows_.count(window) == 0;
    }

    tabengine::ContentId next_content_ = 1;
    std::vector<tabengine::ContentId> closed_;
    int body_events_ = 0;
    std::unordered_set<tabengine::WindowId> veto_windows_;
};

struct Harness {
    Clock clock;
    FakePlatform platform;
    FakeRenderer renderer;
    CountingClient client;
    tabengine::Shell shell{platform, renderer, client};
    AutomationRuntime runtime{{}, {"1.0", "0.1.0", "bridge-test"}, std::ref(clock)};
    ShellAutomationBridge bridge{{shell, platform, renderer, runtime}};

    // transport_request -> payload
    std::map<std::uint64_t, std::string> responses;

    Harness() {
        runtime.set_response_sink(
            [this](std::uint64_t, std::uint64_t request, std::string payload) {
                responses.emplace(request, std::move(payload));
            });
        const tabengine::WindowId window = shell.open_window({10, 20, 900, 600});
        CHECK(window != 0);
    }

    void call(std::uint64_t request, const std::string& json) {
        runtime.receive(7, request, json);
        runtime.drain(clock.now_value);
    }

    // Advance past the tab-close animation and dispatch the animation
    // frames the shell would receive from its frame timer.
    void pump_animations(tabengine::WindowId window) {
        platform.platform_now += 0.5;
        clock.now_value += 0.5;
        for (int i = 0; i < 3; ++i) {
            tabengine::Event frame{tabengine::EventType::AnimationFrame};
            frame.window = window;
            shell.on_event(frame);
        }
    }

    JsonValue result(std::uint64_t request) {
        const auto it = responses.find(request);
        CHECK(it != responses.end());
        JsonValue parsed;
        const auto parsed_result =
            automation::parse_json_strict(it->second, 65536, parsed);
        CHECK(parsed_result.status == automation::JsonParseStatus::Ok);
        const JsonValue* error = parsed.find("error");
        if (error) {
            std::fprintf(stderr, "unexpected error for %llu: %s\n",
                         static_cast<unsigned long long>(request), it->second.c_str());
            CHECK(false);
        }
        return *parsed.find("result");
    }

    JsonValue error(std::uint64_t request) {
        const auto it = responses.find(request);
        CHECK(it != responses.end());
        JsonValue parsed;
        CHECK(automation::parse_json_strict(it->second, 65536, parsed).status ==
              automation::JsonParseStatus::Ok);
        const JsonValue* error = parsed.find("error");
        if (!error) {
            std::fprintf(stderr, "expected error for %llu, got: %s\n",
                         static_cast<unsigned long long>(request), it->second.c_str());
        }
        CHECK(error != nullptr);
        return *error;
    }

    std::string window_id(const JsonValue& value) {
        return value.find("windowId")->as_string();
    }
};

void test_window_and_tab_domains() {
    Harness harness;

    // Window.create appends a second window.
    harness.call(1, R"({"jsonrpc":"2.0","id":1,"method":"Window.create"})");
    const JsonValue created = harness.result(1);
    const std::string second = harness.window_id(created);
    CHECK(second != "1");

    harness.call(2, R"({"jsonrpc":"2.0","id":2,"method":"Window.list"})");
    CHECK(harness.result(2).find("windows")->size() == 2);

    // Tab.create in window 1, then select/move/close it.
    harness.call(3,
                 R"({"jsonrpc":"2.0","id":3,"method":"Tab.create","params":{"windowId":"1"}})");
    const std::string tab = harness.result(3).find("tabId")->as_string();
    CHECK(tab != "" && tab != "0");

    harness.call(4,
                 R"({"jsonrpc":"2.0","id":4,"method":"Tab.select","params":{"windowId":"1","tabId":")" +
                     tab + R"("}})");
    CHECK(harness.result(4).is_object());

    harness.call(5,
                 R"({"jsonrpc":"2.0","id":5,"method":"Tab.move","params":{"windowId":"1","tabId":")" +
                     tab + R"(","index":0}})");
    CHECK(harness.result(5).is_object());

    harness.call(6,
                 R"({"jsonrpc":"2.0","id":6,"method":"Tab.close","params":{"windowId":"1","tabId":")" +
                     tab + R"("}})");
    CHECK(harness.result(6).find("outcome")->as_string() == "closed");
    harness.pump_animations(1);   // the close animates before tab_closed fires
    CHECK(harness.client.closed_.size() == 1);

    // Window.close honors the client's veto.
    harness.client.veto_windows_.insert(1);
    harness.call(7,
                 R"({"jsonrpc":"2.0","id":7,"method":"Window.close","params":{"windowId":"1"}})");
    CHECK(harness.result(7).find("outcome")->as_string() == "denied");
    harness.client.veto_windows_.clear();
    harness.call(8,
                 R"({"jsonrpc":"2.0","id":8,"method":"Window.close","params":{"windowId":"1"}})");
    CHECK(harness.result(8).find("outcome")->as_string() == "closed");

    // Unknown window -> TargetNotFound (1002).
    harness.call(9,
                 R"({"jsonrpc":"2.0","id":9,"method":"Tab.list","params":{"windowId":"999"}})");
    CHECK(harness.error(9).find("code")->as_integer() == 1002);
}

void test_ui_snapshot_and_node_click() {
    Harness harness;

    harness.call(10, R"({"jsonrpc":"2.0","id":10,"method":"UI.snapshot","params":{"windowId":"1"}})");
    const JsonValue snapshot = harness.result(10);
    CHECK(snapshot.find("revision")->is_integer());
    CHECK(snapshot.find("truncated")->as_boolean() == false);
    const JsonValue& nodes = *snapshot.find("nodes");
    CHECK(nodes.size() >= 4);   // window, strip, body, one tab

    bool saw_tab = false;
    bool saw_body = false;
    std::string tab_node;
    for (const JsonValue& node : nodes.items()) {
        if (node.find("role")->as_string() == "tab") {
            saw_tab = true;
            tab_node = node.find("node")->as_string();
        }
        if (node.find("role")->as_string() == "body") saw_body = true;
    }
    CHECK(saw_tab && saw_body);

    // A stableKey click on the new-tab button adds a tab.
    const std::size_t before = harness.shell.model().window(1)->tabs.size();
    harness.call(11,
                 R"({"jsonrpc":"2.0","id":11,"method":"Input.click","params":{"windowId":"1","stableKey":"newTab"}})");
    CHECK(harness.result(11).find("clicked")->as_integer() == 1);
    CHECK(harness.shell.model().window(1)->tabs.size() == before + 1);

    // Node-mode click on the first tab's close button removes a tab.
    harness.call(12, R"({"jsonrpc":"2.0","id":12,"method":"UI.snapshot","params":{"windowId":"1"}})");
    const JsonValue snapshot12 = harness.result(12);
    std::string close_node;
    for (const JsonValue& node : snapshot12.find("nodes")->items()) {
        if (node.find("role")->as_string() == "tabCloseButton") {
            close_node = node.find("node")->as_string();
        }
    }
    if (!close_node.empty()) {
        const std::size_t tabs_before = harness.shell.model().window(1)->tabs.size();
        harness.call(13,
                     R"({"jsonrpc":"2.0","id":13,"method":"Input.click","params":{"windowId":"1","node":")" +
                         close_node + R"("}})");
        CHECK(harness.result(13).find("clicked")->as_integer() == 1);
        harness.pump_animations(1);   // the close animates before the tab goes away
        CHECK(harness.shell.model().window(1)->tabs.size() == tabs_before - 1);
    }

    // Stale node refs report StaleNode (1003).
    harness.call(14,
                 R"({"jsonrpc":"2.0","id":14,"method":"Input.click","params":{"windowId":"1","node":"0"}})");
    CHECK(harness.error(14).find("code")->as_integer() == 1003);
}

void test_input_sessions_and_keys() {
    Harness harness;

    // Logical key chords reach the client body; Ctrl+T opens a tab via the
    // engine's built-in binding (verify it was NOT consumed as text).
    const std::size_t before = harness.shell.model().window(1)->tabs.size();
    harness.call(20,
                 R"({"jsonrpc":"2.0","id":20,"method":"Input.key","params":{"windowId":"1","key":"t","kind":"press","ctrl":true}})");
    CHECK(harness.result(20).is_object());
    CHECK(harness.shell.model().window(1)->tabs.size() == before + 1);

    // Plain key press reaches the body.
    harness.call(21,
                 R"({"jsonrpc":"2.0","id":21,"method":"Input.key","params":{"windowId":"1","key":"a","kind":"press"}})");
    CHECK(harness.client.body_events_ >= 1);

    // Unknown key name -> InvalidArgument (1016).
    harness.call(22,
                 R"({"jsonrpc":"2.0","id":22,"method":"Input.key","params":{"windowId":"1","key":"bogus","kind":"press"}})");
    CHECK(harness.error(22).find("code")->as_integer() == 1016);

    // Text input never triggers shortcuts.
    const std::size_t tabs_before = harness.shell.model().window(1)->tabs.size();
    harness.call(23,
                 R"({"jsonrpc":"2.0","id":23,"method":"Input.typeText","params":{"windowId":"1","text":"hi"}})");
    CHECK(harness.result(23).find("delivered")->as_integer() == 2);
    CHECK(harness.shell.model().window(1)->tabs.size() == tabs_before);

    // Session conflict: a second connection cannot press while the first
    // holds the pointer on the same window.
    harness.runtime.receive(8, 24,
                            R"({"jsonrpc":"2.0","id":24,"method":"Input.pointer","params":{"windowId":"1","kind":"down","x":10,"y":200}})");
    harness.runtime.drain(harness.clock.now_value);
    CHECK(harness.result(24).is_object());
    harness.runtime.receive(9, 25,
                            R"({"jsonrpc":"2.0","id":25,"method":"Input.pointer","params":{"windowId":"1","kind":"down","x":10,"y":200}})");
    harness.runtime.drain(harness.clock.now_value);
    CHECK(harness.error(25).find("code")->as_integer() == 1017);

    // releaseAll ends the first session's hold.
    harness.runtime.receive(8, 26,
                            R"({"jsonrpc":"2.0","id":26,"method":"Input.releaseAll"})");
    harness.runtime.drain(harness.clock.now_value);
    CHECK(harness.result(26).is_object());

    // Coordinate clicks in the body area reach the product client.
    const int body_before = harness.client.body_events_;
    harness.call(27,
                 R"({"jsonrpc":"2.0","id":27,"method":"Input.click","params":{"windowId":"1","x":50,"y":250}})");
    CHECK(harness.result(27).find("clicked")->as_integer() == 1);
    CHECK(harness.client.body_events_ >= body_before + 2);
}

void test_keyboard_release_survives_pointer_release() {
    Harness harness;

    // Hold a key, then hold and release the pointer on the same window:
    // the keyboard session must keep its own focus so releaseAll still
    // delivers the KeyUp to the real window (regression: keys used to ride
    // on pointer_window, which the pointer release cleared).
    harness.call(60,
                 R"({"jsonrpc":"2.0","id":60,"method":"Input.key","params":{"windowId":"1","key":"k","kind":"down"}})");
    CHECK(harness.result(60).is_object());
    harness.call(61,
                 R"({"jsonrpc":"2.0","id":61,"method":"Input.pointer","params":{"windowId":"1","kind":"down","x":50,"y":200}})");
    CHECK(harness.result(61).is_object());
    harness.call(62,
                 R"({"jsonrpc":"2.0","id":62,"method":"Input.pointer","params":{"windowId":"1","kind":"up","x":50,"y":200}})");
    CHECK(harness.result(62).is_object());

    // KeyUp events reach the client body (the shell forwards releases).
    const int before = harness.client.body_events_;
    harness.call(63, R"({"jsonrpc":"2.0","id":63,"method":"Input.releaseAll"})");
    CHECK(harness.result(63).is_object());
    CHECK(harness.client.body_events_ > before);

    // A plain key press now also produces a body KeyUp through the shell.
    const int press_before = harness.client.body_events_;
    harness.call(64,
                 R"({"jsonrpc":"2.0","id":64,"method":"Input.key","params":{"windowId":"1","key":"b","kind":"press"}})");
    CHECK(harness.result(64).is_object());
    CHECK(harness.client.body_events_ >= press_before + 2);   // down + up
}

void test_in_strip_drag_reorder() {
    Harness harness;

    // Two tabs: drag the active (second) tab leftwards past the first.
    harness.call(30, R"({"jsonrpc":"2.0","id":30,"method":"Tab.list","params":{"windowId":"1"}})");
    const JsonValue listing = harness.result(30);
    CHECK(listing.find("tabs")->size() == 1);

    harness.call(31,
                 R"({"jsonrpc":"2.0","id":31,"method":"Tab.create","params":{"windowId":"1"}})");
    CHECK(harness.result(31).is_object());

    // Snapshot gives tab bounds; drag the rightmost tab to the strip start.
    harness.call(32,
                 R"({"jsonrpc":"2.0","id":32,"method":"UI.snapshot","params":{"windowId":"1"}})");
    const JsonValue snapshot32 = harness.result(32);
    int right_x = -1;
    int tab_y = 0;
    int tab_width = 0;
    std::string drag_tab;
    for (const JsonValue& node : snapshot32.find("nodes")->items()) {
        if (node.find("role")->as_string() != "tab") continue;
        const JsonValue& bounds = *node.find("bounds");
        if (bounds.find("x")->as_integer() > right_x) {
            right_x = static_cast<int>(bounds.find("x")->as_integer());
            tab_y = static_cast<int>(bounds.find("y")->as_integer());
            tab_width = static_cast<int>(bounds.find("width")->as_integer());
            drag_tab = node.find("details")->find("tabId")->as_string();
        }
    }
    CHECK(right_x > 0);

    const std::string drag = R"({"jsonrpc":"2.0","id":33,"method":"Input.drag","params":)"
                             R"({"windowId":"1","from":{"x":)" +
                             std::to_string(right_x + tab_width / 2) + R"(,"y":)" +
                             std::to_string(tab_y + 10) +
                             R"(},"to":{"x":40,"y":)" + std::to_string(tab_y + 10) + R"(}}})";
    harness.call(33, drag);
    CHECK(harness.result(33).find("outcome")->as_string() == "released");

    // The dragged tab now sits first.
    harness.call(34,
                 R"({"jsonrpc":"2.0","id":34,"method":"Tab.list","params":{"windowId":"1"}})");
    const JsonValue after = harness.result(34);
    CHECK(after.find("tabs")->at(0).find("tabId")->as_string() == drag_tab);
}

void test_environment_hooks_and_events() {
    Harness harness;

    // animationSettled is satisfiable with no animations running.
    harness.call(40,
                 R"({"jsonrpc":"2.0","id":40,"method":"Wait.condition","params":{"condition":"animationSettled","windowId":"1"}})");
    harness.clock.now_value += 0.05;
    harness.runtime.drain(harness.clock.now_value);
    CHECK(harness.responses.count(40) != 0);

    // Tab operations produce events observable through Event.poll.
    harness.call(41,
                 R"({"jsonrpc":"2.0","id":41,"method":"Tab.create","params":{"windowId":"1"}})");
    harness.call(42, R"({"jsonrpc":"2.0","id":42,"method":"Event.poll","params":{}})");
    const JsonValue poll = harness.result(42);
    const JsonValue& events = *poll.find("events");
    CHECK(events.size() >= 2);   // tab.added + tab.selected
    bool saw_added = false;
    bool saw_selected = false;
    for (const JsonValue& event : events.items()) {
        const std::string name = event.find("name")->as_string();
        if (name == "tab.added") saw_added = true;
        if (name == "tab.selected") saw_selected = true;
    }
    CHECK(saw_added && saw_selected);
}

void test_system_capabilities() {
    Harness harness;
    harness.call(50, R"({"jsonrpc":"2.0","id":50,"method":"System.getInfo"})");
    const JsonValue info = harness.result(50);
    CHECK(info.find("instanceId")->is_string());
    CHECK(info.find("engineVersion")->as_string() == "0.1.0");
    const JsonValue& capabilities = *info.find("capabilities");
    CHECK(capabilities.find("renderer")->as_string() == "unknown");
    CHECK(capabilities.find("input")->find("logical")->as_boolean() == true);
}

// Byte-exact PNG of a pixel range, encoded independently of the bridge.
std::vector<std::uint8_t> expected_png(const std::vector<std::uint32_t>& bgra,
                                       int width, int height) {
    std::vector<std::uint8_t> raw(bgra.size() * 4);
    for (std::size_t i = 0; i < bgra.size(); ++i) {
        raw[i * 4] = static_cast<std::uint8_t>(bgra[i] & 0xFF);
        raw[i * 4 + 1] = static_cast<std::uint8_t>((bgra[i] >> 8) & 0xFF);
        raw[i * 4 + 2] = static_cast<std::uint8_t>((bgra[i] >> 16) & 0xFF);
        raw[i * 4 + 3] = static_cast<std::uint8_t>((bgra[i] >> 24) & 0xFF);
    }
    const SkImageInfo info = SkImageInfo::MakeN32Premul(width, height);
    const SkPixmap pixmap(info, raw.data(), static_cast<std::size_t>(width) * 4);
    const sk_sp<SkData> png = SkPngEncoder::Encode(pixmap, SkPngEncoder::Options{});
    CHECK(png != nullptr);
    const auto* begin = static_cast<const std::uint8_t*>(png->data());
    return {begin, begin + png->size()};
}

void test_capture_artifact_and_trace() {
    Harness harness;

    // Full-window capture: PNG artifact with metadata.
    harness.call(60, R"({"jsonrpc":"2.0","id":60,"method":"Capture.screenshot","params":{"windowId":"1"}})");
    const JsonValue shot = harness.result(60);
    CHECK(shot.find("backend")->as_string() == "raster");
    CHECK(shot.find("width")->as_integer() == 900);
    CHECK(shot.find("height")->as_integer() == 600);
    CHECK(shot.find("mediaType")->as_string() == "image/png");
    const std::string full_id = shot.find("artifactId")->as_string();
    CHECK(!full_id.empty());

    // Artifact.get returns matching metadata; bytes match an independent
    // encode of the fake renderer's base color.
    harness.call(61, R"({"jsonrpc":"2.0","id":61,"method":"Artifact.get","params":{"artifactId":")" +
                         full_id + R"("}})");
    const JsonValue metadata = harness.result(61);
    CHECK(metadata.find("mediaType")->as_string() == "image/png");
    CHECK(metadata.find("sha256")->as_string().size() == 64);
    automation::ArtifactStore::Entry entry;
    CHECK(harness.runtime.artifacts().get(full_id, entry));
    const std::vector<std::uint32_t> base_pixel{0xFF202080u};
    const std::vector<std::uint8_t> base_png = expected_png(base_pixel, 1, 1);
    (void)base_png;
    CHECK(entry.bytes.size() > 8);
    CHECK(entry.bytes[0] == 0x89 && entry.bytes[1] == 'P' && entry.bytes[2] == 'N' &&
          entry.bytes[3] == 'G');

    // A 1x1 region over the green sentinel pixel must equal an independent
    // encode of exactly that pixel.
    harness.call(62,
                 R"({"jsonrpc":"2.0","id":62,"method":"Capture.screenshot","params":{"windowId":"1","region":{"x":10,"y":20,"width":1,"height":1}}})");
    const JsonValue pixel_shot = harness.result(62);
    CHECK(pixel_shot.find("width")->as_integer() == 1);
    automation::ArtifactStore::Entry pixel_entry;
    CHECK(harness.runtime.artifacts().get(pixel_shot.find("artifactId")->as_string(),
                                          pixel_entry));
    const std::vector<std::uint32_t> green{0xFF00FF00u};
    CHECK(pixel_entry.bytes == expected_png(green, 1, 1));

    // Node capture uses the node's current bounds.
    harness.call(63,
                 R"({"jsonrpc":"2.0","id":63,"method":"Capture.screenshot","params":{"windowId":"1","node":")" +
                     std::to_string(
                         (static_cast<std::uint64_t>(5) << 56)) /* new-tab engine node */ +
                     R"("}})");
    CHECK(harness.result(63).find("width")->as_integer() > 0);

    // Unknown node reports StaleNode.
    harness.call(64,
                 R"({"jsonrpc":"2.0","id":64,"method":"Capture.screenshot","params":{"windowId":"1","node":"999999"}})");
    CHECK(harness.error(64).find("code")->as_integer() == 1003);

    // Over-budget region reports BudgetExceeded (1014).
    harness.call(65,
                 R"({"jsonrpc":"2.0","id":65,"method":"Capture.screenshot","params":{"windowId":"1","region":{"x":0,"y":0,"width":9000,"height":9000}}})");
    CHECK(harness.error(65).find("code")->as_integer() == 1014);

    // A renderer without capture support reports Unsupported.
    harness.renderer.capture_supported = false;
    harness.call(66, R"({"jsonrpc":"2.0","id":66,"method":"Capture.screenshot","params":{"windowId":"1"}})");
    CHECK(harness.error(66).find("code")->as_integer() == 1000);
    harness.renderer.capture_supported = true;

    // Trace records input and capture spans.
    harness.call(67, R"({"jsonrpc":"2.0","id":67,"method":"Trace.start"})");
    CHECK(harness.result(67).find("active")->as_boolean());
    harness.call(68,
                 R"({"jsonrpc":"2.0","id":68,"method":"Input.click","params":{"windowId":"1","stableKey":"newTab"}})");
    CHECK(harness.result(68).find("clicked")->as_integer() == 1);
    harness.call(69, R"({"jsonrpc":"2.0","id":69,"method":"Capture.screenshot","params":{"windowId":"1"}})");
    CHECK(harness.result(69).is_object());
    harness.call(70, R"({"jsonrpc":"2.0","id":70,"method":"Trace.status"})");
    CHECK(harness.result(70).find("spans")->as_integer() >= 2);
    harness.call(71, R"({"jsonrpc":"2.0","id":71,"method":"Trace.stop"})");
    const JsonValue trace = harness.result(71);
    bool saw_input = false;
    bool saw_capture = false;
    for (const JsonValue& span : trace.find("spans")->items()) {
        const std::string name = span.find("name")->as_string();
        if (name == "Input.click") saw_input = true;
        if (name == "Capture.screenshot") saw_capture = true;
    }
    CHECK(saw_input && saw_capture);
    CHECK(trace.find("active")->as_boolean() == false);
}

// Closing a connection releases everything its input session holds, on the
// UI thread: the next client can use the pointer instead of meeting a
// phantom SessionConflict (regression from the delivery review).
void test_input_release_on_connection_closed() {
    Harness harness;

    // Connection 30 holds the pointer, a key and an open IME composition.
    harness.runtime.receive(30, 200,
        R"({"jsonrpc":"2.0","id":1,"method":"Input.pointer","params":{"windowId":"1","kind":"down","x":40,"y":300}})");
    harness.runtime.drain(harness.clock.now_value);
    CHECK(harness.result(200).is_object());
    harness.runtime.receive(30, 201,
        R"({"jsonrpc":"2.0","id":2,"method":"Input.key","params":{"windowId":"1","key":"k","kind":"down"}})");
    harness.runtime.drain(harness.clock.now_value);
    CHECK(harness.result(201).is_object());
    harness.runtime.receive(30, 202,
        R"({"jsonrpc":"2.0","id":3,"method":"Input.ime","params":{"windowId":"1","kind":"start"}})");
    harness.runtime.drain(harness.clock.now_value);
    CHECK(harness.result(202).is_object());

    // Another connection cannot press while 30 holds the pointer.
    harness.runtime.receive(31, 203,
        R"({"jsonrpc":"2.0","id":4,"method":"Input.pointer","params":{"windowId":"1","kind":"down","x":60,"y":300}})");
    harness.runtime.drain(harness.clock.now_value);
    CHECK(harness.error(203).find("code")->as_integer() == 1017);

    // Close 30: the release events (pointer up, key up, IME cancel) run on
    // the next UI drain, not on the worker thread.
    const int before = harness.client.body_events_;
    harness.runtime.connection_closed(30);
    CHECK(harness.client.body_events_ == before);   // nothing yet
    harness.runtime.drain(harness.clock.now_value);
    CHECK(harness.client.body_events_ > before);    // released now

    // The pointer is free: connection 31 presses without releaseAll.
    harness.runtime.receive(31, 204,
        R"({"jsonrpc":"2.0","id":5,"method":"Input.pointer","params":{"windowId":"1","kind":"down","x":60,"y":300}})");
    harness.runtime.drain(harness.clock.now_value);
    CHECK(harness.result(204).is_object());
}

// A provider that switches document generations: strict clicks must follow
// its resolve verdict and the merged layout revision, and a stale ref must
// never silently click whatever object took its place (review finding: the
// merged tree ignored provider revisions and re-enumerated numeric refs).
class GenerationProvider final : public automation::ISemanticProvider {
public:
    std::uint64_t revision = 1;
    int generation = 1;
    int reject_resolves = 0;
    tabengine::Rect bounds{20, 100, 200, 60};

    void fill(automation::SemanticNode& node) const {
        node.role = "button";
        node.name = "Row";
        node.stable_key = "app.row";
        node.node = 77;
        node.bounds = bounds;
        node.clip = bounds;
        node.actions = {"click"};
    }

    bool provide_semantic(tabengine::WindowId, automation::SemanticSnapshot& out) override {
        out.revision = revision;
        out.layout_revision = revision;
        out.document_generation = static_cast<std::uint64_t>(generation);
        automation::SemanticNode node;
        fill(node);
        out.nodes.push_back(node);
        return true;
    }

    bool resolve_node(tabengine::WindowId, std::uint64_t node,
                      automation::SemanticNode& resolved) override {
        if (reject_resolves > 0 || node != 77) return false;
        fill(resolved);
        return true;
    }
};

void test_provider_generation_and_strict_clicks() {
    Harness harness;
    GenerationProvider provider;
    auto registration = harness.runtime.register_semantic_provider(&provider);
    CHECK(registration.valid());

    const auto click_json = [](const std::string& extra) {
        return R"({"jsonrpc":"2.0","id":1,"method":"Input.click","params":{"windowId":"1",)" +
               extra + R"(}})";
    };
    const std::string node_arg = R"("node":")" +
        std::to_string((static_cast<std::uint64_t>(9) << 56) | 77) + R"(")";   // slot 9, node 77

    // Snapshot pins the merged layout revision.
    harness.call(300, R"({"jsonrpc":"2.0","id":1,"method":"UI.snapshot","params":{"windowId":"1"}})");
    const JsonValue snapshot1 = harness.result(300);
    const std::int64_t pinned = snapshot1.find("layoutRevision")->as_integer();
    bool saw_provider_node = false;
    for (const JsonValue& node : snapshot1.find("nodes")->items()) {
        if (node.find("stableKey") && node.find("stableKey")->as_string() == "app.row") {
            saw_provider_node = true;
            CHECK(node.find("node")->as_string() ==
                  std::to_string((static_cast<std::uint64_t>(9) << 56) | 77));
        }
    }
    CHECK(saw_provider_node);
    CHECK(snapshot1.find("documentGeneration")->as_integer() == 1);

    // Provider rejects the ref outright (DOM rebuilt): StaleNode, and zero
    // input dispatched.
    provider.reject_resolves = 1;
    int before = harness.client.body_events_;
    harness.call(301, click_json(node_arg + R"(,"snapshotId":)" + std::to_string(pinned) +
                                 R"(,"strict":true)"));
    CHECK(harness.error(301).find("code")->as_integer() == 1003);
    CHECK(harness.client.body_events_ == before);
    provider.reject_resolves = 0;

    // Generation 2: same local ref means a different business object at a
    // different position. The provider still resolves node 77, but the
    // pinned merged revision moved: strict fails, non-strict clicks the
    // CURRENT geometry.
    provider.revision = 2;
    provider.generation = 2;
    provider.bounds = {330, 100, 200, 60};
    harness.call(302, click_json(node_arg + R"(,"snapshotId":)" + std::to_string(pinned) +
                                 R"(,"strict":true)"));
    CHECK(harness.error(302).find("code")->as_integer() == 1003);
    CHECK(harness.client.body_events_ == before);   // nothing dispatched

    harness.call(303, click_json(node_arg));
    const JsonValue clicked = harness.result(303);
    CHECK(clicked.find("x")->as_integer() == 430);   // new bounds center
    CHECK(clicked.find("y")->as_integer() == 130);
    CHECK(harness.client.body_events_ > before);

    // Strict stableKey: the pinned revision must cover provider changes.
    harness.call(304, R"({"jsonrpc":"2.0","id":1,"method":"UI.snapshot","params":{"windowId":"1"}})");
    const std::int64_t pinned2 = harness.result(304).find("layoutRevision")->as_integer();
    CHECK(pinned2 > pinned);   // provider change moved the merged revision
    provider.revision = 3;
    harness.call(305,
                 R"({"jsonrpc":"2.0","id":1,"method":"Input.click","params":{"windowId":"1","stableKey":"app.row","snapshotId":)" +
                     std::to_string(pinned2) + R"(,"strict":true}})");
    CHECK(harness.error(305).find("code")->as_integer() == 1003);
    harness.call(306,
                 R"({"jsonrpc":"2.0","id":1,"method":"Input.click","params":{"windowId":"1","stableKey":"app.row"}})");
    CHECK(harness.result(306).find("clicked")->as_integer() == 1);
}

// Provider slots are never reused: a ref minted for an unregistered
// provider never addresses the provider that took its list position.
void test_provider_slot_not_reused() {
    class BoxProvider final : public automation::ISemanticProvider {
    public:
        std::string key;
        tabengine::Rect bounds;
        bool provide_semantic(tabengine::WindowId, automation::SemanticSnapshot& out) override {
            out.revision = 1;
            out.layout_revision = 1;
            automation::SemanticNode node;
            node.role = "button";
            node.stable_key = key;
            node.node = 5;
            node.bounds = bounds;
            node.clip = bounds;
            out.nodes.push_back(node);
            return true;
        }
        bool resolve_node(tabengine::WindowId, std::uint64_t node,
                          automation::SemanticNode& resolved) override {
            if (node != 5) return false;
            resolved.role = "button";
            resolved.stable_key = key;
            resolved.node = 5;
            resolved.bounds = bounds;
            resolved.clip = bounds;
            return true;
        }
    };

    Harness harness;
    BoxProvider first;
    first.key = "first.row";
    first.bounds = {50, 200, 100, 40};
    std::int64_t before_detach = 0;
    {
        auto registration = harness.runtime.register_semantic_provider(&first);
        CHECK(registration.valid());
        harness.call(310,
                     R"({"jsonrpc":"2.0","id":1,"method":"UI.snapshot","params":{"windowId":"1"}})");
        const JsonValue snapshot = harness.result(310);
        before_detach = snapshot.find("layoutRevision")->as_integer();
        for (const JsonValue& node : snapshot.find("nodes")->items()) {
            if (node.find("stableKey") && node.find("stableKey")->as_string() == "first.row") {
                CHECK(node.find("node")->as_string() ==
                      std::to_string((static_cast<std::uint64_t>(9) << 56) | 5));
            }
        }
    }   // first provider unregisters here

    harness.call(314,
                 R"({"jsonrpc":"2.0","id":1,"method":"UI.snapshot","params":{"windowId":"1"}})");
    CHECK(harness.result(314).find("layoutRevision")->as_integer() > before_detach);

    BoxProvider second;
    second.key = "second.row";
    second.bounds = {400, 300, 100, 40};
    {
        auto registration = harness.runtime.register_semantic_provider(&second);
        CHECK(registration.valid());
        harness.call(311,
                     R"({"jsonrpc":"2.0","id":1,"method":"UI.snapshot","params":{"windowId":"1"}})");
        const JsonValue snapshot = harness.result(311);
        bool saw_second = false;
        for (const JsonValue& node : snapshot.find("nodes")->items()) {
            if (node.find("stableKey") && node.find("stableKey")->as_string() == "second.row") {
                saw_second = true;
                // New slot (10), not the recycled list index.
                CHECK(node.find("node")->as_string() ==
                      std::to_string((static_cast<std::uint64_t>(10) << 56) | 5));
            }
        }
        CHECK(saw_second);
    }

    // The old ref (slot 9) is dead: no click, no resolve, and the new
    // provider's node 5 at the same local id is NOT a substitute.
    const std::string old_ref = std::to_string((static_cast<std::uint64_t>(9) << 56) | 5);
    const int before = harness.client.body_events_;
    harness.call(312,
                 R"({"jsonrpc":"2.0","id":1,"method":"Input.click","params":{"windowId":"1","node":")" +
                     old_ref + R"("}})");
    CHECK(harness.error(312).find("code")->as_integer() == 1003);
    CHECK(harness.client.body_events_ == before);
    harness.call(313,
                 R"({"jsonrpc":"2.0","id":1,"method":"UI.resolveNode","params":{"windowId":"1","node":")" +
                     old_ref + R"("}})");
    CHECK(harness.error(313).find("code")->as_integer() == 1003);
}

// Clicks land inside the visible clip, and fully clipped nodes refuse.
void test_clip_aware_click_point() {
    class ClippedProvider final : public automation::ISemanticProvider {
    public:
        tabengine::Rect bounds{0, 400, 900, 100};
        tabengine::Rect clip{0, 400, 100, 100};
        bool fully_clipped = false;
        bool provide_semantic(tabengine::WindowId, automation::SemanticSnapshot& out) override {
            automation::SemanticNode node;
            node.role = "pane";
            node.stable_key = "app.clipped";
            node.node = 1;
            node.bounds = bounds;
            node.clip = fully_clipped ? tabengine::Rect{} : clip;
            out.nodes.push_back(node);
            return true;
        }
        bool resolve_node(tabengine::WindowId, std::uint64_t node,
                          automation::SemanticNode& resolved) override {
            if (node != 1) return false;
            resolved = out_node();
            return true;
        }
        automation::SemanticNode out_node() const {
            automation::SemanticNode node;
            node.role = "pane";
            node.stable_key = "app.clipped";
            node.node = 1;
            node.bounds = bounds;
            node.clip = fully_clipped ? tabengine::Rect{} : clip;
            return node;
        }
    };

    Harness harness;
    ClippedProvider provider;
    auto registration = harness.runtime.register_semantic_provider(&provider);
    CHECK(registration.valid());

    harness.call(320,
                 R"({"jsonrpc":"2.0","id":1,"method":"Input.click","params":{"windowId":"1","stableKey":"app.clipped"}})");
    const JsonValue clicked = harness.result(320);
    const std::int64_t x = clicked.find("x")->as_integer();
    CHECK(x >= 0 && x < 100);   // inside the visible 100px strip, not 450

    provider.fully_clipped = true;
    harness.call(321,
                 R"({"jsonrpc":"2.0","id":1,"method":"Input.click","params":{"windowId":"1","stableKey":"app.clipped"}})");
    CHECK(harness.error(321).find("code")->as_integer() == 1004);   // NotInteractable
}

// Destroying the bridge revokes its wait hooks: pending dependent waits
// fail with Unsupported instead of calling into freed memory.
void test_hooks_revoked_on_bridge_destruction() {
    Clock clock;
    FakePlatform platform;
    FakeRenderer renderer;
    CountingClient client;
    tabengine::Shell shell{platform, renderer, client};
    AutomationRuntime runtime{{}, {"1.0", "0.1.0", "detach-test"}, std::ref(clock)};

    std::map<std::uint64_t, std::string> responses;
    runtime.set_response_sink(
        [&](std::uint64_t, std::uint64_t request, std::string payload) {
            responses.emplace(request, std::move(payload));
        });
    const tabengine::WindowId window = shell.open_window({10, 20, 900, 600});
    CHECK(window != 0);

    {
        ShellAutomationBridge bridge{{shell, platform, renderer, runtime}};
        runtime.receive(7, 400,
                        R"({"jsonrpc":"2.0","id":1,"method":"Wait.condition","params":{"condition":"nodeAppears","windowId":"1","stableKey":"never.appears"}})");
        runtime.drain(clock.now_value);
        CHECK(responses.count(400) == 0);   // pending on the bridge hooks
    }   // bridge destroyed

    clock.now_value += 0.05;
    runtime.drain(clock.now_value);
    CHECK(responses.count(400) == 1);
    JsonValue parsed;
    CHECK(automation::parse_json_strict(responses[400], 65536, parsed).status ==
          automation::JsonParseStatus::Ok);
    CHECK(parsed.find("error")->find("code")->as_integer() == 1000);   // Unsupported

    // The runtime keeps serving (no hooks) without touching the dead bridge.
    runtime.receive(7, 401, R"({"jsonrpc":"2.0","id":2,"method":"System.getInfo"})");
    runtime.drain(clock.now_value);
    CHECK(responses.count(401) == 1);
}

// Real HTTP against a real bridge: raw input over a logical session holds
// across requests; without one, each request's leftovers release right
// after its response (the review's SessionConflict repro).
void test_http_input_sessions_end_to_end() {
    Clock clock;
    FakePlatform platform;
    FakeRenderer renderer;
    CountingClient client;
    tabengine::Shell shell{platform, renderer, client};
    AutomationRuntime runtime{{}, {"1.0", "0.1.0", "http-bridge"}, std::ref(clock)};
    ShellAutomationBridge bridge{{shell, platform, renderer, runtime}};
    automation::TransportConfig config;
    config.token = "0123456789abcdef0123456789abcdef";
    config.response_timeout_s = 2.0;
    automation::AutomationTransport transport{runtime, config};
    CHECK(transport.start());
    const tabengine::WindowId window = shell.open_window({10, 20, 900, 600});
    CHECK(window != 0);

    struct HttpReply {
        int status = 0;
        std::string body;
        bool rpc_error = false;
        std::int64_t error_code = 0;
    };
    const auto rpc = [&](const std::string& body, const std::string& session = {}) {
        HttpReply reply;
        std::atomic<bool> done{false};
        std::thread request_thread([&] {
            SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(transport.port());
            inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
            CHECK(connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
            std::string request = "POST /automation/v1/rpc HTTP/1.1\r\n";
            request += "Host: 127.0.0.1\r\n";
            request += "Content-Type: application/json\r\n";
            request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
            request += "Authorization: Bearer " + config.token + "\r\n";
            if (!session.empty()) request += "X-TabEngine-Session: " + session + "\r\n";
            request += "\r\n";
            request += body;
            CHECK(send(socket, request.data(), static_cast<int>(request.size()), 0) ==
                  static_cast<int>(request.size()));
            char buffer[4096];
            std::string raw;
            while (true) {
                const int got = recv(socket, buffer, sizeof(buffer), 0);
                if (got <= 0) break;
                raw.append(buffer, static_cast<std::size_t>(got));
            }
            closesocket(socket);
            const std::size_t line_end = raw.find("\r\n");
            if (line_end != std::string::npos) {
                reply.status = std::atoi(raw.substr(9, 3).c_str());
            }
            const std::size_t body_start = raw.find("\r\n\r\n");
            if (body_start != std::string::npos) reply.body = raw.substr(body_start + 4);
            done.store(true);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done.load()) {
            runtime.drain(clock.now_value);
            clock.now_value += 0.001;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CHECK(std::chrono::steady_clock::now() < deadline);
        }
        request_thread.join();
        JsonValue parsed;
        if (reply.status == 200 &&
            automation::parse_json_strict(reply.body, 65536, parsed).status ==
                automation::JsonParseStatus::Ok) {
            if (const JsonValue* error = parsed.find("error")) {
                reply.rpc_error = true;
                reply.error_code = error->find("code")->as_integer();
            }
        }
        return reply;
    };
    const auto session_begin = [&] {
        std::string id;
        std::atomic<bool> done{false};
        std::thread request_thread([&] {
            SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(transport.port());
            inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
            CHECK(connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
            const std::string request =
                "POST /automation/v1/session HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                "Authorization: Bearer " +
                config.token + "\r\n\r\n";
            CHECK(send(socket, request.data(), static_cast<int>(request.size()), 0) ==
                  static_cast<int>(request.size()));
            char buffer[1024];
            std::string raw;
            while (true) {
                const int got = recv(socket, buffer, sizeof(buffer), 0);
                if (got <= 0) break;
                raw.append(buffer, static_cast<std::size_t>(got));
            }
            closesocket(socket);
            const std::size_t body_start = raw.find("\r\n\r\n");
            JsonValue parsed;
            if (body_start != std::string::npos &&
                automation::parse_json_strict(raw.substr(body_start + 4), 65536, parsed)
                    .status == automation::JsonParseStatus::Ok) {
                if (const JsonValue* value = parsed.find("sessionId")) id = value->as_string();
            }
            done.store(true);
        });
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        request_thread.join();
        return id;
    };

    // Review repro: a raw pointer down on an ephemeral request used to
    // leave the button stuck; the next client met SessionConflict. Now the
    // ephemeral connection closes right after the response and releases.
    const HttpReply down = rpc(
        R"({"jsonrpc":"2.0","id":1,"method":"Input.pointer","params":{"windowId":"1","kind":"down","x":40,"y":300}})");
    CHECK(down.status == 200);
    CHECK(!down.rpc_error);
    const HttpReply click_after = rpc(
        R"({"jsonrpc":"2.0","id":2,"method":"Input.click","params":{"windowId":"1","x":40,"y":300}})");
    CHECK(click_after.status == 200);
    CHECK(!click_after.rpc_error);   // no SessionConflict

    // Session: down -> move -> up across three requests, one identity.
    const std::string session = session_begin();
    CHECK(session.size() >= 32);
    client.last_move_buttons = 0;
    const HttpReply s_down = rpc(
        R"({"jsonrpc":"2.0","id":3,"method":"Input.pointer","params":{"windowId":"1","kind":"down","x":40,"y":300}})",
        session);
    CHECK(s_down.status == 200 && !s_down.rpc_error);
    const HttpReply s_move = rpc(
        R"({"jsonrpc":"2.0","id":4,"method":"Input.pointer","params":{"windowId":"1","kind":"move","x":80,"y":320}})",
        session);
    CHECK(s_move.status == 200 && !s_move.rpc_error);
    CHECK(client.last_move_buttons == tabengine::kPointerButtonLeft);   // held carried
    CHECK(client.last_move_button == static_cast<int>(tabengine::PointerButton::Left));
    const HttpReply s_up = rpc(
        R"({"jsonrpc":"2.0","id":5,"method":"Input.pointer","params":{"windowId":"1","kind":"up","x":80,"y":320}})",
        session);
    CHECK(s_up.status == 200 && !s_up.rpc_error);

    // Hold a key over the session; ending the session releases it on the
    // UI thread (KeyUp reaches the body).
    const HttpReply key_down = rpc(
        R"({"jsonrpc":"2.0","id":6,"method":"Input.key","params":{"windowId":"1","key":"z","kind":"down"}})",
        session);
    CHECK(key_down.status == 200 && !key_down.rpc_error);
    const int before_release = client.body_events_;
    {
        std::atomic<bool> done{false};
        std::thread request_thread([&] {
            SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(transport.port());
            inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
            CHECK(connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
            const std::string request =
                "DELETE /automation/v1/session HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                "Authorization: Bearer " +
                config.token + "\r\nX-TabEngine-Session: " + session + "\r\n\r\n";
            CHECK(send(socket, request.data(), static_cast<int>(request.size()), 0) ==
                  static_cast<int>(request.size()));
            char buffer[1024];
            while (recv(socket, buffer, sizeof(buffer), 0) > 0) {
            }
            closesocket(socket);
            done.store(true);
        });
        while (!done.load()) {
            runtime.drain(clock.now_value);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        request_thread.join();
    }
    clock.now_value += 0.01;
    runtime.drain(clock.now_value);
    CHECK(client.body_events_ > before_release);   // KeyUp delivered

    // The session id is gone: further requests with it fail cleanly.
    const HttpReply after_end = rpc(
        R"({"jsonrpc":"2.0","id":7,"method":"System.getInfo"})", session);
    CHECK(after_end.status == 404);

    // Host-mounted route relaying input on a minted session: the legacy
    // gesture pattern (per-request HTTP, one stable identity).
    const std::string gesture_session = transport.create_session();
    transport.set_route_handler(
        [&gesture_session](const automation::AutomationTransport::RouteRequest& request,
                           const automation::AutomationTransport::RpcCall& call,
                           const automation::AutomationTransport::ArtifactReader&)
            -> automation::AutomationTransport::RouteReply {
            if (request.method != "POST" || request.path != "/gesture") {
                return automation::AutomationTransport::RouteReply{};
            }
            const std::string envelope = call(request.body, 8.0, gesture_session);
            if (envelope.empty()) {
                return automation::AutomationTransport::RouteReply{
                    503, "application/json",
                    "{\"error\":\"no response\",\"sid\":\"" + gesture_session + "\"}"};
            }
            return automation::AutomationTransport::RouteReply{200, "application/json",
                                                               envelope};
        });
    client.last_move_buttons = 0;
    // The legacy shape: plain envelopes POSTed to the host route, no
    // session header - the route pins the identity itself.
    const auto gesture = [&](const char* envelope) {
        std::string request = "POST /gesture HTTP/1.1\r\nHost: 127.0.0.1\r\n";
        request += "Authorization: Bearer " + config.token + "\r\n";
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: " + std::to_string(std::strlen(envelope)) + "\r\n\r\n";
        request += envelope;
        SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(transport.port());
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        CHECK(connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        CHECK(send(socket, request.data(), static_cast<int>(request.size()), 0) ==
              static_cast<int>(request.size()));
        char buffer[4096];
        std::string raw;
        while (true) {
            const int got = recv(socket, buffer, sizeof(buffer), 0);
            if (got <= 0) break;
            raw.append(buffer, static_cast<std::size_t>(got));
        }
        closesocket(socket);
        const std::size_t body_start = raw.find("\r\n\r\n");
        HttpReply reply;
        reply.status = std::atoi(raw.substr(9, 3).c_str());
        if (body_start != std::string::npos) reply.body = raw.substr(body_start + 4);
        JsonValue parsed;
        if (reply.status == 200 &&
            automation::parse_json_strict(reply.body, 65536, parsed).status ==
                automation::JsonParseStatus::Ok) {
            if (const JsonValue* error = parsed.find("error")) {
                reply.rpc_error = true;
                reply.error_code = error->find("code")->as_integer();
            }
        }
        return reply;
    };
    // Pump the runtime on this thread while each gesture request runs on
    // a client thread (the transport worker relays through the session).
    const auto gesture_pumped = [&](const char* envelope) {
        HttpReply reply;
        std::atomic<bool> done{false};
        std::thread client_thread([&] {
            reply = gesture(envelope);
            done.store(true);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done.load()) {
            runtime.drain(clock.now_value);
            clock.now_value += 0.001;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CHECK(std::chrono::steady_clock::now() < deadline);
        }
        client_thread.join();
        return reply;
    };
    const HttpReply g_down = gesture_pumped(
        R"({"jsonrpc":"2.0","id":1,"method":"Input.pointer","params":{"windowId":"1","kind":"down","x":40,"y":300}})");
    CHECK(g_down.status == 200 && !g_down.rpc_error);
    const HttpReply g_move = gesture_pumped(
        R"({"jsonrpc":"2.0","id":2,"method":"Input.pointer","params":{"windowId":"1","kind":"move","x":80,"y":320}})");
    CHECK(g_move.status == 200 && !g_move.rpc_error);
    CHECK(client.last_move_buttons == tabengine::kPointerButtonLeft);
    const HttpReply g_up = gesture_pumped(
        R"({"jsonrpc":"2.0","id":3,"method":"Input.pointer","params":{"windowId":"1","kind":"up","x":80,"y":320}})");
    CHECK(g_up.status == 200 && !g_up.rpc_error);

    transport.stop();
}

// Snapshot assembly must stay linear in the tab count: one chrome-target
// pass resolves every strip node (guards the O(N^2) regression).
void test_snapshot_with_many_tabs() {
    Harness harness;
    for (int i = 0; i < 39; ++i) {
        CHECK(harness.shell.new_tab(1) != 0);
    }
    CHECK(harness.shell.model().window(1)->tabs.size() == 40);

    harness.call(330,
                 R"({"jsonrpc":"2.0","id":1,"method":"UI.snapshot","params":{"windowId":"1"}})");
    const JsonValue snapshot = harness.result(330);
    CHECK(snapshot.find("truncated")->as_boolean() == false);
    std::size_t tabs = 0;
    std::size_t close_buttons = 0;
    std::unordered_set<std::string> refs;
    for (const JsonValue& node : snapshot.find("nodes")->items()) {
        const std::string role = node.find("role")->as_string();
        if (role == "tab") {
            ++tabs;
            refs.insert(node.find("node")->as_string());
        }
        if (role == "tabCloseButton") ++close_buttons;
    }
    CHECK(tabs == 40);
    CHECK(refs.size() == 40);   // every tab keeps a distinct ref
    CHECK(close_buttons >= 1);
}

// Real input can land while a stepwise tear-off move session is ending:
// the drag driver must stay consistent under re-entrant dispatch and the
// session must end exactly once (fake-level stand-in; the physical-mouse
// variant remains a manual gate).
void test_competing_input_during_stepwise_move() {
    Harness harness;

    // Find the active tab and drag it below the strip to tear it off.
    harness.call(340,
                 R"({"jsonrpc":"2.0","id":1,"method":"UI.snapshot","params":{"windowId":"1"}})");
    const JsonValue snapshot = harness.result(340);
    int tab_x = -1;
    int tab_y = 0;
    for (const JsonValue& node : snapshot.find("nodes")->items()) {
        if (node.find("role")->as_string() != "tab") continue;
        const JsonValue& bounds = *node.find("bounds");
        tab_x = static_cast<int>(bounds.find("x")->as_integer() +
                                 bounds.find("width")->as_integer() / 2);
        tab_y = static_cast<int>(bounds.find("y")->as_integer() + 10);
    }
    CHECK(tab_x > 0);

    harness.platform.inject_competing_move = true;
    const std::string drag =
        R"({"jsonrpc":"2.0","id":2,"method":"Input.drag","params":{"windowId":"1","from":{"x":)" +
        std::to_string(tab_x) + R"(,"y":)" + std::to_string(tab_y) +
        R"(},"to":{"x":)" + std::to_string(tab_x) + R"(,"y":400}}})";
    harness.call(341, drag);
    harness.platform.inject_competing_move = false;
    const JsonValue result = harness.result(341);
    const std::string outcome = result.find("outcome")->as_string();
    CHECK(outcome == "floated" || outcome == "released" || outcome == "lost");

    // One end per torn window, never a burst of re-entrant completions.
    CHECK(harness.platform.move_session_ends <= 1);

    // The tab still exists exactly once across all windows.
    std::size_t tabs_total = 0;
    for (const tabengine::WindowId id : harness.shell.model().window_ids()) {
        tabs_total += harness.shell.model().window(id)->tabs.size();
    }
    CHECK(tabs_total == 1);
}

// Synthetic double-click releases carry no click count (native: only the
// second WM_*BUTTONDBLCLK press carries 2).
void test_double_click_release_click_counts() {
    Harness harness;
    harness.client.body_click_counts.clear();
    harness.call(350,
                 R"({"jsonrpc":"2.0","id":1,"method":"Input.click","params":{"windowId":"1","x":300,"y":300,"count":2}})");
    CHECK(harness.result(350).find("clicked")->as_integer() == 2);
    const auto& counts = harness.client.body_click_counts;
    CHECK(counts.size() == 4);
    if (counts.size() == 4) {
        CHECK(counts[0].first == "down" && counts[0].second == 1);
        CHECK(counts[1].first == "up" && counts[1].second == 0);
        CHECK(counts[2].first == "down" && counts[2].second == 2);
        CHECK(counts[3].first == "up" && counts[3].second == 0);
    }
}

// Window.setPlacement routes hide/show to the platform; hiding is the
// resident-instance path (state stays alive).
void test_window_hide_show_placement() {
    Harness harness;
    harness.call(360,
                 R"({"jsonrpc":"2.0","id":1,"method":"Window.setPlacement","params":{"windowId":"1","state":"hide"}})");
    CHECK(harness.result(360).is_object());
    CHECK(harness.platform.hidden_.size() == 1);
    CHECK(harness.platform.hidden_[0] == 1);

    // The window and its automation surface stay alive while hidden.
    harness.call(361,
                 R"({"jsonrpc":"2.0","id":2,"method":"Tab.list","params":{"windowId":"1"}})");
    CHECK(harness.result(361).find("tabs")->size() == 1);

    const std::size_t shown_before = harness.platform.shown_.size();   // setup shows the window
    harness.call(362,
                 R"({"jsonrpc":"2.0","id":3,"method":"Window.setPlacement","params":{"windowId":"1","state":"show"}})");
    CHECK(harness.result(362).is_object());
    CHECK(harness.platform.shown_.size() == shown_before + 1);
    CHECK(harness.platform.shown_.back() == 1);

    // Unknown actions still fail validation.
    harness.call(363,
                 R"({"jsonrpc":"2.0","id":4,"method":"Window.setPlacement","params":{"windowId":"1","state":"teleport"}})");
    CHECK(harness.error(363).find("code")->as_integer() == -32602);

    // The capability block reports the hide support.
    harness.call(364, R"({"jsonrpc":"2.0","id":5,"method":"System.getInfo"})");
    CHECK(harness.result(364).find("capabilities")->find("windowHide")->as_boolean() == true);
}

} // namespace

int main() {
    test_window_and_tab_domains();
    test_ui_snapshot_and_node_click();
    test_input_sessions_and_keys();
    test_keyboard_release_survives_pointer_release();
    test_in_strip_drag_reorder();
    test_environment_hooks_and_events();
    test_system_capabilities();
    test_capture_artifact_and_trace();
    test_input_release_on_connection_closed();
    test_provider_generation_and_strict_clicks();
    test_provider_slot_not_reused();
    test_clip_aware_click_point();
    test_hooks_revoked_on_bridge_destruction();
    test_http_input_sessions_end_to_end();
    test_snapshot_with_many_tabs();
    test_competing_input_during_stepwise_move();
    test_double_click_release_click_counts();
    test_window_hide_show_placement();
    return 0;
}
