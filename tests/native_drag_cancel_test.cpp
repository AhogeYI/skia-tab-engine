#include "tabengine/shell.h"

#include "check.h"
#include <functional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
using namespace tabengine;

class Platform final : public IPlatform {
public:
    void set_event_handler(std::function<void(const Event&)> handler) override {
        events = std::move(handler);
    }
    void set_caption_hit_handler(std::function<bool(WindowId, Point)>) override {}
    void set_wake_handler(std::function<void()> handler) override { wake_handler = std::move(handler); }
    void wake() override { ++wake_posts; }
    // Fakes queue wakes; tests pump them on the "UI" thread explicitly.
    void pump_wakes() {
        while (wake_posts > 0) {
            --wake_posts;
            if (wake_handler) wake_handler();
        }
    }
    bool create(WindowId id, Rect bounds, std::string_view, bool) override {
        return windows.emplace(id, bounds).second;
    }
    void show(WindowId) override {}
    void destroy(WindowId id) override { windows.erase(id); }
    void invalidate(WindowId) override {}
    double monotonic_seconds() const override { return 0.0; }
    void request_animation_frame(WindowId) override {}
    void capture_pointer(WindowId) override {}
    void release_pointer() override {}
    void minimize(WindowId) override {}
    void toggle_maximize(WindowId) override {}
    Size client_size(WindowId id) const override {
        const Rect& r = windows.at(id);
        return {r.width, r.height};
    }
    Point client_origin(WindowId id) const override {
        const Rect& r = windows.at(id);
        return {r.x, r.y};
    }
    float scale(WindowId) const override { return 1.0f; }
    void* native_handle(WindowId) const override { return nullptr; }
    WindowId window_at(Point, WindowId) const override { return target; }
    bool supports_native_move_loop() const override { return true; }
    void set_client_origin(WindowId id, Point origin) override {
        windows.at(id).x = origin.x;
        windows.at(id).y = origin.y;
    }
    MoveLoopResult run_native_move_loop(WindowId id) override {
        moved = id;
        if (target) emit({EventType::Moving, id, {}, {550, 120}});
        if (escape) emit({EventType::KeyDown, id, {}, {}, {}, 27});
        if (during_move) during_move();
        return result;
    }
    void end_native_move_loop(WindowId) override { ++end_calls; }
    int run() override { return 0; }
    void emit(Event event) {
        event.size = client_size(event.window);
        events(event);
    }

    std::function<void(const Event&)> events;
    std::function<void()> wake_handler;
    int wake_posts = 0;
    std::unordered_map<WindowId, Rect> windows;
    MoveLoopResult result = MoveLoopResult::Canceled;
    WindowId target = 0;
    WindowId moved = 0;
    bool escape = false;
    int end_calls = 0;
    std::function<void()> during_move;
};

class Renderer final : public IRenderer {
public:
    bool attach(WindowId, void*, Size) override { return true; }
    void resize(WindowId, Size) override {}
    void detach(WindowId) override {}
    SkCanvas* canvas(WindowId) override { return nullptr; }
    void present(WindowId, void*) override {}
    RenderInfo info(WindowId) const override { return {}; }
};

class Client final : public IClient {
public:
    NewTab create_tab() override { return {next_content++, "Tab"}; }
    void tab_attached(WindowId window, TabId tab, ContentId content) override {
        attached.push_back({window, tab, content});
    }
    void tab_detached(WindowId window, TabId tab, ContentId content) override {
        detached.push_back({window, tab, content});
    }
    void tab_closed(ContentId content) override { closed.push_back(content); }
    void paint_body(WindowId, TabId, SkCanvas&, Rect) override {}
    void body_event(const Event&, Rect) override {}

    struct Binding {
        WindowId window;
        TabId tab;
        ContentId content;
    };
    ContentId next_content = 1;
    std::vector<Binding> attached;
    std::vector<Binding> detached;
    std::vector<ContentId> closed;
};

void drag_first_tab(Platform& platform, WindowId source) {
    platform.emit({EventType::PointerDown, source, {70, 20}, {170, 120}});
    platform.emit({EventType::PointerMove, source, {70, 100}, {170, 200}});
}

void check_rollback(MoveLoopResult result, bool escape = false) {
    Platform platform;
    Renderer renderer;
    Client client;
    Shell shell(platform, renderer, client);
    const WindowId source = shell.open_window({100, 100, 900, 600});
    const Tab first = shell.model().window(source)->tabs.front();
    const TabId second = shell.new_tab(source);
    platform.result = result;
    platform.escape = escape;
    drag_first_tab(platform, source);

    const WindowTabs* restored = shell.model().window(source);
    CHECK(restored && restored->tabs.size() == 2);
    CHECK(restored->tabs[0].id == first.id);
    CHECK(restored->tabs[0].content == first.content);
    CHECK(restored->tabs[1].id == second);
    CHECK(restored->active == first.id);
    CHECK(shell.model().window_ids().size() == 1);
    CHECK(platform.windows.size() == 1);
    CHECK(platform.moved != source);
    CHECK(client.detached.size() == 2);
    CHECK(client.attached.size() == 4);
    CHECK(client.closed.empty());
}

void check_completed_tear_off() {
    Platform platform;
    platform.result = MoveLoopResult::Completed;
    Renderer renderer;
    Client client;
    Shell shell(platform, renderer, client);
    const WindowId source = shell.open_window({100, 100, 900, 600});
    const Tab first = shell.model().window(source)->tabs.front();
    (void)shell.new_tab(source);
    drag_first_tab(platform, source);

    CHECK(shell.model().window_ids().size() == 2);
    CHECK(shell.model().window(source)->tabs.size() == 1);
    const WindowTabs* torn = shell.model().window(platform.moved);
    CHECK(torn && torn->tabs.size() == 1);
    CHECK(torn->tabs[0].id == first.id && torn->tabs[0].content == first.content);
    CHECK(client.closed.empty());
}

void check_target_attach(bool escape) {
    Platform platform;
    Renderer renderer;
    Client client;
    Shell shell(platform, renderer, client);
    const WindowId source = shell.open_window({100, 100, 900, 600});
    const Tab first = shell.model().window(source)->tabs.front();
    (void)shell.new_tab(source);
    const WindowId target = shell.open_window({500, 100, 900, 600});
    platform.target = target;
    platform.escape = escape;
    drag_first_tab(platform, source);

    CHECK(platform.end_calls >= 1);
    CHECK(shell.model().window_ids().size() == 2);
    const WindowTabs* destination = shell.model().window(escape ? source : target);
    CHECK(destination->active == first.id);
    CHECK(destination->tabs.size() == 2);
    CHECK(client.closed.empty());
}

void check_single_tab_cancel() {
    Platform platform;
    Renderer renderer;
    Client client;
    Shell shell(platform, renderer, client);
    const WindowId source = shell.open_window({100, 100, 900, 600});
    const Tab first = shell.model().window(source)->tabs.front();
    drag_first_tab(platform, source);

    CHECK(platform.moved == source);
    CHECK(shell.model().window_ids().size() == 1);
    CHECK(shell.model().window(source)->tabs[0].id == first.id);
    CHECK(client.detached.empty());
    CHECK(client.closed.empty());
}

void check_source_closed_during_cancel() {
    Platform platform;
    Renderer renderer;
    Client client;
    Shell shell(platform, renderer, client);
    const WindowId source = shell.open_window({100, 100, 900, 600});
    const Tab first = shell.model().window(source)->tabs.front();
    (void)shell.new_tab(source);
    platform.during_move = [&] { shell.close_window(source); };
    drag_first_tab(platform, source);

    CHECK(!shell.model().window(source));
    CHECK(shell.model().window_ids().size() == 1);
    const WindowTabs* torn = shell.model().window(platform.moved);
    CHECK(torn && torn->tabs.size() == 1);
    CHECK(torn->tabs[0].id == first.id && torn->tabs[0].content == first.content);
    CHECK(client.closed.size() == 1);
    CHECK(client.closed[0] != first.content);
}
} // namespace

int main() {
    check_rollback(MoveLoopResult::Canceled);
    check_rollback(MoveLoopResult::Unsupported);
    check_rollback(MoveLoopResult::Canceled, true);
    check_rollback(MoveLoopResult::Completed, true);
    check_completed_tear_off();
    check_target_attach(false);
    check_target_attach(true);
    check_single_tab_cancel();
    check_source_closed_during_cancel();
}
