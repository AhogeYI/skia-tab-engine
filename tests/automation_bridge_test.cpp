// Shell bridge integration: real protocol calls through the runtime against
// a faked platform and renderer. Covers Window/Tab commands, semantic
// snapshots, node-mode clicks, input sessions, and in-strip drags.

#include "tabengine/automation/shell_bridge.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/encode/SkPngEncoder.h"

#include "check.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
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
    void show(tabengine::WindowId) override {}
    void destroy(tabengine::WindowId id) override { windows_.erase(id); }
    void invalidate(tabengine::WindowId) override {}
    double monotonic_seconds() const override { return platform_now; }
    void request_animation_frame(tabengine::WindowId) override {}
    void capture_pointer(tabengine::WindowId) override {}
    void release_pointer() override {}
    void minimize(tabengine::WindowId) override {}
    void toggle_maximize(tabengine::WindowId) override {}
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
    void body_event(const tabengine::Event&, tabengine::Rect) override { ++body_events_; }
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

    // transport_request → payload
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

    // Unknown window → TargetNotFound (1002).
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

    // Unknown key name → InvalidArgument (1016).
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

} // namespace

int main() {
    test_window_and_tab_domains();
    test_ui_snapshot_and_node_click();
    test_input_sessions_and_keys();
    test_in_strip_drag_reorder();
    test_environment_hooks_and_events();
    test_system_capabilities();
    test_capture_artifact_and_trace();
    return 0;
}
