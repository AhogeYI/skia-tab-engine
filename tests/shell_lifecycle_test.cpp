#include "tabengine/shell.h"

#include <algorithm>
#include "check.h"
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace tabengine;

class Platform final : public IPlatform {
public:
    void set_event_handler(std::function<void(const Event&)>) override {}
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
    double monotonic_seconds() const override { return now; }
    void request_animation_frame(WindowId) override { ++frame_requests; }
    void capture_pointer(WindowId) override {}
    void release_pointer() override { ++pointer_releases; }
    void minimize(WindowId id) override { minimized.push_back(id); }
    void toggle_maximize(WindowId) override {}
    Size client_size(WindowId id) const override {
        const Rect& r = windows.at(id);
        return {r.width, r.height};
    }
    Point client_origin(WindowId id) const override {
        const Rect& r = windows.at(id);
        return {r.x, r.y};
    }
    float scale(WindowId) const override { return dpi_scale; }
    void* native_handle(WindowId) const override { return nullptr; }
    WindowId window_at(Point, WindowId) const override { return 0; }
    bool supports_native_move_loop() const override { return true; }
    void set_client_origin(WindowId, Point) override {}
    MoveLoopResult run_native_move_loop(WindowId) override {
        ++native_move_loops;
        return MoveLoopResult::Completed;
    }
    void end_native_move_loop(WindowId) override {}
    int run() override { return 0; }

    std::unordered_map<WindowId, Rect> windows;
    std::vector<WindowId> minimized;
    std::function<void()> wake_handler;
    int wake_posts = 0;
    float dpi_scale = 1.0f;
    int pointer_releases = 0;
    int native_move_loops = 0;
    int frame_requests = 0;
    double now = 0.0;
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

struct Binding {
    WindowId window;
    TabId tab;
    ContentId content;
    bool operator==(const Binding&) const = default;
};

class Client final : public IClient {
public:
    NewTab create_tab() override { return {next_content++, "Tab"}; }
    bool allow_close_tab(WindowId window, TabId tab, ContentId) override {
        if (reenter && shell) {
            reenter = false;
            reentrant_tab = shell->new_tab(window);
        }
        return tab != denied_tab;
    }
    bool allow_close_window(WindowId window) override { return window != denied_window; }
    void tab_attached(WindowId w, TabId t, ContentId c) override {
        attached.push_back({w, t, c});
    }
    void tab_detached(WindowId w, TabId t, ContentId c) override {
        detached.push_back({w, t, c});
    }
    void active_tab_changed(WindowId w, TabId before, TabId after) override {
        active_changes.push_back({w, before, after});
    }
    void body_geometry_changed(WindowId w, Rect bounds, float) override {
        body_bounds[w] = bounds;
    }
    void window_activation_changed(WindowId w, bool active) override {
        window_activation.emplace_back(w, active);
    }
    void dpi_changed(WindowId w, float scale) override {
        dpi_events.emplace_back(w, scale);
    }
    void window_placement_changed(WindowId w, Rect bounds, float scale) override {
        placement_events.emplace_back(w, bounds, scale);
    }
    bool handle_shortcut(const Event& event) override {
        if (event.ctrl && event.key == 'T' && consume_new_tab_shortcut) {
            ++consumed_shortcuts;
            return true;
        }
        return false;
    }
    void tab_closed(ContentId content) override { closed.push_back(content); }
    void extra_caption_button_pressed(WindowId window, int index) override {
        extra_caption_clicks.emplace_back(window, index);
    }
    void paint_body(WindowId, TabId, SkCanvas&, Rect) override {}
    void body_event(const Event&, Rect) override {}

    Shell* shell = nullptr;
    ContentId next_content = 1;
    TabId denied_tab = 0;
    WindowId denied_window = 0;
    bool reenter = false;
    TabId reentrant_tab = 999;
    std::vector<Binding> attached;
    std::vector<Binding> detached;
    std::vector<ContentId> closed;
    std::vector<std::tuple<WindowId, TabId, TabId>> active_changes;
    std::unordered_map<WindowId, Rect> body_bounds;
    std::vector<std::pair<WindowId, int>> extra_caption_clicks;
    std::vector<std::pair<WindowId, bool>> window_activation;
    std::vector<std::pair<WindowId, float>> dpi_events;
    std::vector<std::tuple<WindowId, Rect, float>> placement_events;
    bool consume_new_tab_shortcut = false;
    int consumed_shortcuts = 0;
};

void check_animated_lifecycle() {
    Platform platform;
    Renderer renderer;
    Client client;
    Shell shell(platform, renderer, client);
    const WindowId window = shell.open_window({0, 0, 900, 600});
    const TabId first = shell.model().window(window)->active;
    const TabId second = shell.new_tab(window);
    const ContentId second_content = shell.model().window(window)->tabs[1].content;
    CHECK(platform.frame_requests > 0);
    platform.now = 0.10;
    shell.on_event({EventType::AnimationFrame, window});
    const TabId third = shell.new_tab(window); // Retarget while the second tab is expanding.
    platform.now = 0.35;
    shell.on_event({EventType::AnimationFrame, window});
    client.denied_tab = second;
    CHECK(!shell.close_tab(window, second));
    CHECK(!shell.model().window(window)->tabs[1].closing);
    client.denied_tab = 0;
    CHECK(shell.close_tab(window, second));
    CHECK(shell.model().window(window)->tabs[1].closing);
    CHECK(shell.model().window(window)->active == third);
    CHECK(!shell.select_tab(window, second));
    CHECK(!shell.move_tab(window, second, 0));
    CHECK(client.closed.empty() && client.detached.empty());
    platform.now = 0.50;
    shell.on_event({EventType::AnimationFrame, window});
    CHECK(shell.model().window(window)->tabs.size() == 3);
    CHECK(client.closed.empty());
    platform.now = 0.56;
    shell.on_event({EventType::AnimationFrame, window});
    CHECK(shell.model().window(window)->tabs.size() == 2);
    CHECK(shell.model().window(window)->tabs[0].id == first);
    CHECK(shell.model().window(window)->tabs[1].id == third);
    CHECK(client.closed == std::vector<ContentId>({second_content}));
    shell.on_event({EventType::AnimationFrame, window});
    CHECK(client.closed.size() == 1);

    const WindowId short_lived = shell.open_window({0, 0, 700, 500});
    const ContentId first_content = shell.model().window(short_lived)->tabs[0].content;
    const TabId other = shell.new_tab(short_lived);
    const ContentId other_content = shell.model().window(short_lived)->tabs[1].content;
    platform.now = 0.80;
    shell.on_event({EventType::AnimationFrame, short_lived});
    const TabId closing = shell.model().window(short_lived)->tabs[0].id;
    CHECK(shell.close_tab(short_lived, closing));
    CHECK(shell.close_tab(short_lived, other)); // Last live tab closes the window.
    CHECK(!shell.model().window(short_lived));
    CHECK(!platform.windows.contains(short_lived));
    CHECK(std::count(client.closed.begin(), client.closed.end(), first_content) == 1);
    CHECK(std::count(client.closed.begin(), client.closed.end(), other_content) == 1);
}

} // namespace

int main() {
    Platform platform;
    Renderer renderer;
    Client client;
    {
        Shell shell(platform, renderer, client);
        client.shell = &shell;
        shell.set_chrome_options({36, 1});
        const WindowId source = shell.open_window({0, 0, 900, 600});
        const TabId first = shell.model().window(source)->active;
        const ContentId first_content = shell.model().window(source)->tabs.front().content;
        const TabId moved = shell.new_tab(source);
        const ContentId moved_content = shell.model().window(source)->tabs.back().content;
        const WindowId target = shell.open_window({100, 100, 800, 500});
        const TabId target_tab = shell.model().window(target)->active;
        const ContentId target_content = shell.model().window(target)->tabs.front().content;
        CHECK(client.attached == std::vector<Binding>({
            {source, first, first_content}, {source, moved, moved_content},
            {target, target_tab, target_content}}));
        CHECK(client.body_bounds[source].y == 41);
        CHECK(client.body_bounds[source].height == 559);
        CHECK(client.active_changes.size() == 3);
        CHECK(client.active_changes[0] == std::make_tuple(source, TabId{0}, first));
        CHECK(client.active_changes[1] == std::make_tuple(source, first, moved));
        CHECK(client.active_changes[2] == std::make_tuple(target, TabId{0}, target_tab));
        const auto chrome = Layout::tab_strip(900, 2, 1.0f, {36, 1});
        shell.on_event({EventType::PointerDown, source,
                        {chrome.caption_start + 20, 20}});
        CHECK((client.extra_caption_clicks ==
                std::vector<std::pair<WindowId, int>>({{source, 0}})));
        shell.on_event({EventType::PointerDown, source,
                        {chrome.caption_start + 67, 20}});
        CHECK(platform.minimized == std::vector<WindowId>({source}));
        CHECK(shell.model().window(source)->tabs.size() == 2);

        platform.now = 0.25;
        shell.on_event({EventType::AnimationFrame, source});

        const auto reorder_first = [&] {
            shell.on_event({EventType::PointerDown, source, {70, 20}, {70, 20}});
            shell.on_event({EventType::PointerMove, source, {80, 20}, {80, 20}});
            CHECK(shell.model().window(source)->tabs[0].id == first);
            shell.on_event({EventType::PointerMove, source, {450, 20}, {450, 20}});
            CHECK(shell.model().window(source)->tabs[0].id == moved);
            CHECK(shell.model().window(source)->tabs[1].id == first);
        };
        reorder_first();
        shell.on_event({EventType::KeyDown, source, {}, {}, {}, 27}); // Escape
        CHECK(shell.model().window(source)->tabs[0].id == first);
        CHECK(shell.model().window(source)->tabs[1].id == moved);
        platform.now = 0.50;
        shell.on_event({EventType::AnimationFrame, source});
        reorder_first();
        shell.on_event({EventType::CaptureLost, source});
        CHECK(shell.model().window(source)->tabs[0].id == first);
        CHECK(shell.model().window(source)->tabs[1].id == moved);
        platform.now = 0.75;
        shell.on_event({EventType::AnimationFrame, source});

        shell.on_event({EventType::WindowActivated, source});
        shell.on_event({EventType::PointerDown, source, {70, 20}, {70, 20}});
        shell.on_event({EventType::WindowDeactivated, source});
        shell.on_event({EventType::PointerMove, source, {70, 100}, {70, 100}});
        CHECK((client.window_activation == std::vector<std::pair<WindowId, bool>>(
            {{source, true}, {source, false}})));
        CHECK(platform.pointer_releases == 3);
        CHECK(platform.native_move_loops == 0);
        CHECK(shell.model().window_ids().size() == 2);
        client.consume_new_tab_shortcut = true;
        shell.on_event({EventType::KeyDown, source, {}, {}, {}, 'T', true});
        CHECK(client.consumed_shortcuts == 1);
        CHECK(shell.model().window(source)->tabs.size() == 2);
        client.consume_new_tab_shortcut = false;

        platform.windows[source].width = 640;
        platform.windows[source].height = 480;
        shell.on_event({EventType::Resized, source, {}, {}, {640, 480}});
        CHECK(client.body_bounds[source].width == 640);
        CHECK(client.body_bounds[source].height == 439);
        platform.dpi_scale = 1.5f;
        shell.on_event({EventType::DpiChanged, source});
        CHECK((client.dpi_events ==
                std::vector<std::pair<WindowId, float>>({{source, 1.5f}})));
        CHECK(client.body_bounds[source].y == 62);
        CHECK(client.body_bounds[source].height == 418);
        platform.windows[source].x = 123;
        platform.windows[source].y = 234;
        shell.on_event({EventType::PlacementChanged, source});
        const auto& [placed_window, placed_bounds, placed_scale] = client.placement_events.back();
        CHECK(placed_window == source && placed_bounds.x == 123 && placed_bounds.y == 234);
        CHECK(placed_bounds.width == 640 && placed_bounds.height == 480);
        CHECK(placed_scale == 1.5f);

        client.denied_tab = moved;
        CHECK(!shell.close_tab(source, moved));
        CHECK(shell.model().window(source)->tabs.size() == 2);
        CHECK(client.closed.empty() && client.detached.empty());
        client.denied_tab = 0;
        CHECK(shell.select_tab(source, first));
        CHECK(shell.transfer_tab(source, target, moved, 1));
        CHECK((client.detached.back() == Binding{source, moved, moved_content}));
        CHECK((client.attached.back() == Binding{target, moved, moved_content}));
        CHECK(shell.model().window(target)->active == moved);
        CHECK(client.closed.empty());
        CHECK(client.active_changes.back() == std::make_tuple(target, target_tab, moved));

        client.denied_tab = moved;
        shell.close_window(target);
        CHECK(shell.model().window(target)->tabs.size() == 2);
        CHECK(client.detached.size() == 1 && client.closed.empty());
        client.denied_tab = 0;
        shell.close_window(target);
        CHECK(!shell.model().window(target) && !platform.windows.contains(target));
        CHECK(client.closed == std::vector<ContentId>({target_content, moved_content}));
        CHECK(client.active_changes.back() == std::make_tuple(target, moved, TabId{0}));

        client.denied_window = source;
        CHECK(!shell.close_tab(source, first));
        CHECK(shell.model().window(source)->tabs.size() == 1);
        client.denied_window = 0;
        client.reenter = true;
        CHECK(shell.close_tab(source, first));
        CHECK(client.reentrant_tab == 0);
        CHECK(!shell.model().window(source));
        CHECK(client.closed == std::vector<ContentId>({target_content, moved_content,
                                                        first_content}));
        CHECK(client.active_changes.back() == std::make_tuple(source, first, TabId{0}));

        const WindowId single = shell.open_window({0, 0, 700, 500});
        const TabId single_tab = shell.model().window(single)->active;
        const ContentId single_content = shell.model().window(single)->tabs.front().content;
        const WindowId empty = shell.open_window({0, 0, 700, 500}, false);
        CHECK(shell.transfer_tab(single, empty, single_tab, 0));
        CHECK(!shell.model().window(single) && !platform.windows.contains(single));
        CHECK(shell.model().window(empty)->tabs.front().content == single_content);
        CHECK(client.closed.size() == 3);
    }
    CHECK(client.closed.size() == 4);
    CHECK(client.detached.size() == client.attached.size());
    CHECK(platform.windows.empty());
    check_animated_lifecycle();
}
