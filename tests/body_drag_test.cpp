#include "tabengine/shell.h"

#include "include/core/SkCanvas.h"

#include "check.h"
#include <memory>
#include <string>
#include <vector>

namespace {

// Minimal recording client: the scenarios only care whether body_event ran
// and with which event, so the drag-routing contract can be asserted without
// any product knowledge.
class Client final : public tabengine::IClient {
public:
    tabengine::NewTab create_tab() override {
        return {next_++, "Tab"};
    }
    void tab_closed(tabengine::ContentId) override {}
    void paint_body(tabengine::WindowId, tabengine::TabId, SkCanvas&,
                    tabengine::Rect) override {}
    void body_event(const tabengine::Event& event, tabengine::Rect body) override {
        events_.push_back({event.type, event.client.x, event.client.y, body.y});
    }
    const std::vector<std::string>& titles() const { return titles_; }
    struct Received {
        tabengine::EventType type = tabengine::EventType::PointerMove;
        int x = 0;
        int y = 0;
        int body_y = 0;
    };
    const std::vector<Received>& events() const { return events_; }
    void clear() { events_.clear(); }

private:
    tabengine::ContentId next_ = 1;
    std::vector<Received> events_;
    std::vector<std::string> titles_;
};

class FakePlatform final : public tabengine::IPlatform {
public:
    void set_event_handler(std::function<void(const tabengine::Event&)> handler) override {
        handler_ = std::move(handler);
    }
    void set_caption_hit_handler(std::function<bool(tabengine::WindowId, tabengine::Point)>) override {}
    void set_wake_handler(std::function<void()>) override {}
    void wake() override {}
    void set_ime_caret_provider(std::function<tabengine::Rect(tabengine::WindowId)>) override {}
    bool create(tabengine::WindowId id, tabengine::Rect bounds, std::string_view, bool) override {
        windows_.emplace(id, bounds);
        return true;
    }
    void show(tabengine::WindowId) override {}
    void destroy(tabengine::WindowId id) override { windows_.erase(id); }
    void invalidate(tabengine::WindowId) override {}
    double monotonic_seconds() const override { return 0.0; }
    void request_animation_frame(tabengine::WindowId) override {}
    void capture_pointer(tabengine::WindowId) override {}
    void release_pointer() override {}
    void minimize(tabengine::WindowId) override {}
    void toggle_maximize(tabengine::WindowId) override {}
    tabengine::Size client_size(tabengine::WindowId id) const override {
        const auto& r = windows_.at(id);
        return {r.width, r.height};
    }
    tabengine::Point client_origin(tabengine::WindowId id) const override {
        const auto& r = windows_.at(id);
        return {r.x, r.y};
    }
    float scale(tabengine::WindowId) const override { return 1.0f; }
    void* native_handle(tabengine::WindowId) const override { return nullptr; }
    tabengine::WindowId window_at(tabengine::Point, tabengine::WindowId) const override {
        return 0;
    }
    bool supports_native_move_loop() const override { return false; }
    void set_client_origin(tabengine::WindowId, tabengine::Point) override {}
    tabengine::MoveLoopResult run_native_move_loop(tabengine::WindowId) override {
        return tabengine::MoveLoopResult::Unsupported;
    }
    void end_native_move_loop(tabengine::WindowId) override {}
    int run() override { return 0; }
    void emit(tabengine::Event event) {
        event.size = client_size(event.window);
        handler_(event);
    }

private:
    std::unordered_map<tabengine::WindowId, tabengine::Rect> windows_;
    std::function<void(const tabengine::Event&)> handler_;
};

void check_strip_moves_forward_during_body_drag() {
    FakePlatform platform;
    auto renderer = tabengine::make_skia_raster_renderer();
    Client client;
    tabengine::Shell shell(platform, *renderer, client);
    const auto window = shell.open_window({100, 100, 900, 600});
    (void)shell.new_tab(window);

    // Without a body drag, a move over the strip never reaches the body.
    client.clear();
    platform.emit({tabengine::EventType::PointerMove, window, {120, 10}});
    CHECK(client.events().empty());

    // The client declared a body drag: strip moves now forward with the body
    // rect the body expects (origin below the strip).
    shell.set_body_drag(window, true);
    platform.emit({tabengine::EventType::PointerMove, window, {120, 10}});
    CHECK(client.events().size() == 1);
    CHECK(client.events()[0].type == tabengine::EventType::PointerMove);
    CHECK(client.events()[0].y == 10);
    CHECK(client.events()[0].body_y > 0);

    // Moves inside the body keep flowing through the same forwarding.
    platform.emit({tabengine::EventType::PointerMove, window, {120, 500}});
    CHECK(client.events().size() == 2);
    CHECK(client.events()[1].y == 500);

    // Clearing the flag restores the default routing.
    shell.set_body_drag(window, false);
    platform.emit({tabengine::EventType::PointerMove, window, {120, 10}});
    CHECK(client.events().size() == 2);
}

void check_body_drag_does_not_arm_tab_drag() {
    FakePlatform platform;
    auto renderer = tabengine::make_skia_raster_renderer();
    Client client;
    tabengine::Shell shell(platform, *renderer, client);
    const auto window = shell.open_window({100, 100, 900, 600});
    const auto second = shell.new_tab(window);

    tabengine::Rect second_bounds{};
    for (const auto& target : shell.chrome_targets(window)) {
        if (target.kind == tabengine::Shell::ChromeTarget::Kind::Tab &&
            target.tab == second) {
            second_bounds = target.bounds;
        }
    }
    CHECK(second_bounds.width > 0);
    const int strip_y = second_bounds.y + second_bounds.height / 2;

    // A body drag is running (flag set, button already held by the client).
    shell.set_body_drag(window, true);
    client.clear();
    // A down over a tab would normally select it and arm a tab drag; while a
    // body drag owns the gesture the strip must not react. The engine cannot
    // receive a fresh left down mid-gesture in practice (capture), so the
    // contract check is the move/release pair: selecting another tab by move
    // and releasing over it must not reorder or tear off tabs.
    platform.emit({tabengine::EventType::PointerMove, window,
                   {second_bounds.x + second_bounds.width / 2, strip_y}});
    platform.emit({tabengine::EventType::PointerUp, window,
                   {second_bounds.x + second_bounds.width / 2, strip_y}});
    CHECK(client.events().size() == 2);
    CHECK(client.events()[0].type == tabengine::EventType::PointerMove);
    CHECK(client.events()[1].type == tabengine::EventType::PointerUp);
    // No tab was closed or torn off by the release over the strip: the model
    // still holds both tabs in order.
    const auto* tabs = shell.model().window(window);
    CHECK(tabs->tabs.size() == 2);
    shell.set_body_drag(window, false);

    // After the drag ends, strip presses work again (tab selection).
    platform.emit({tabengine::EventType::PointerDown, window,
                   {second_bounds.x + second_bounds.width / 2, strip_y}});
    CHECK(shell.model().window(window)->active == second);
}

void check_body_drag_is_per_window() {
    FakePlatform platform;
    auto renderer = tabengine::make_skia_raster_renderer();
    Client client;
    tabengine::Shell shell(platform, *renderer, client);
    const auto first = shell.open_window({100, 100, 900, 600});
    const auto second = shell.open_window({120, 120, 900, 600});

    shell.set_body_drag(first, true);
    client.clear();
    platform.emit({tabengine::EventType::PointerMove, second, {120, 10}});
    CHECK(client.events().empty()); // only the declaring window forwards
    platform.emit({tabengine::EventType::PointerMove, first, {120, 10}});
    CHECK(client.events().size() == 1);

    // Closing the dragging window drops its flag with it (no stale routing
    // for a recycled window id is observable here, but the set must empty).
    shell.close_window(first);
    client.clear();
    platform.emit({tabengine::EventType::PointerMove, second, {120, 10}});
    CHECK(client.events().empty());
}

} // namespace

int main() {
    check_strip_moves_forward_during_body_drag();
    check_body_drag_does_not_arm_tab_drag();
    check_body_drag_is_per_window();
    return 0;
}
