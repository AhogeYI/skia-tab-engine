#include "tabengine/shell.h"

#include "check.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <array>
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
    WindowId window_at(Point, WindowId) const override { return 0; }
    bool supports_native_move_loop() const override { return false; }
    void set_client_origin(WindowId, Point) override {}
    MoveLoopResult run_native_move_loop(WindowId) override { return MoveLoopResult::Unsupported; }
    void end_native_move_loop(WindowId) override {}
    int run() override { return 0; }
    void emit(Event event) {
        // A destroyed native window stops delivering messages entirely.
        if (!windows.contains(event.window)) return;
        event.size = client_size(event.window);
        events(event);
    }

    std::function<void(const Event&)> events;
    std::function<void()> wake_handler;
    int wake_posts = 0;
    std::unordered_map<WindowId, Rect> windows;
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
    NewTab create_tab() override { return {++next_content, "Tab"}; }
    bool handle_shortcut(const Event& event) override {
        (void)event;
        ++shortcuts;
        return false;
    }
    void tab_closed(ContentId) override {}
    void paint_body(WindowId, TabId, SkCanvas&, Rect) override {}
    void body_event(const Event& event, Rect body) override {
        if (event.type == EventType::TextInput) {
            received.push_back(event.code_point);
            body_height = body.height;
        }
        if (event.type == EventType::KeyDown && event.alt && event.key == VK_LEFT_KEY) {
            ++alt_left;
        }
        if (event.type == EventType::PointerWheel) {
            wheels.push_back({event.wheel, event.client.x, event.client.y});
        }
    }

    static constexpr int VK_LEFT_KEY = 0x25;
    ContentId next_content = 0;
    int shortcuts = 0;
    int alt_left = 0;
    int body_height = 0;
    std::vector<char32_t> received;
    std::vector<std::array<int, 3>> wheels;
};

void check_composer_pairs_astral() {
    SurrogateComposer composer;
    CHECK(!composer.feed(0xD83D)); // high surrogate of U+1F600
    CHECK(composer.feed(0xDE00));
    CHECK(composer.code_point == 0x1F600);
}

void check_composer_bmp_direct() {
    SurrogateComposer composer;
    // U+9C7C, a CJK ideograph outside the ASCII range.
    CHECK(composer.feed(0x9C7C));
    CHECK(composer.code_point == 0x9C7C);
}

void check_composer_lone_low_is_dropped() {
    SurrogateComposer composer;
    CHECK(!composer.feed(0xDE00));
    CHECK(composer.feed('b'));
    CHECK(composer.code_point == 'b');
}

void check_composer_dangling_high_dropped() {
    SurrogateComposer composer;
    CHECK(!composer.feed(0xD83D));
    CHECK(composer.feed('a'));
    CHECK(composer.code_point == 'a');
    // The pair completes only while the high surrogate is current.
    CHECK(!composer.feed(0xD83D));
    CHECK(!composer.feed(0xD83D));
    CHECK(composer.feed(0xDE00));
    CHECK(composer.code_point == 0x1F600);
}

void check_shell_forwards_text_to_body() {
    Platform platform;
    Renderer renderer;
    Client client;
    Shell shell(platform, renderer, client);
    const WindowId window = shell.open_window({100, 100, 900, 600}, true, false);
    const std::size_t tabs_before = shell.model().window(window)->tabs.size();

    platform.emit({EventType::TextInput, window, {}, {}, {}, 0, false, false, false, 'x'});
    platform.emit({EventType::TextInput, window, {}, {}, {}, 0, false, false, false, 0x1F600});

    CHECK((client.received == std::vector<char32_t>{'x', 0x1F600}));
    // Text is not a shortcut and never triggers the engine's key bindings.
    CHECK(client.shortcuts == 0);
    CHECK(shell.model().window(window)->tabs.size() == tabs_before);
    // The body rect passed to the client matches a KeyDown's body rect.
    platform.emit({EventType::KeyDown, window, {}, {}, {}, 0x25, false, false, true});
    CHECK(client.alt_left == 1);
    CHECK(client.body_height > 0);
}

void check_shell_drops_text_for_unknown_window() {
    Platform platform;
    Renderer renderer;
    Client client;
    Shell shell(platform, renderer, client);
    const WindowId window = shell.open_window({100, 100, 900, 600});
    shell.close_window(window);
    platform.emit({EventType::TextInput, window, {}, {}, {}, 0, false, false, false, 'x'});
    CHECK(client.received.empty());
}

// Wheels always belong to the body, with the client point carried along.
void check_shell_forwards_wheel_to_body() {
    Platform platform;
    Renderer renderer;
    Client client;
    Shell shell(platform, renderer, client);
    const WindowId window = shell.open_window({100, 100, 900, 600});

    Event up;
    up.type = EventType::PointerWheel;
    up.window = window;
    up.client = {130, 220};
    up.wheel = 120;
    platform.emit(up);
    Event down;
    down.type = EventType::PointerWheel;
    down.window = window;
    down.client = {140, 240};
    down.wheel = -240;
    platform.emit(down);

    CHECK(client.wheels.size() == 2);
    CHECK(client.wheels[0][0] == 120 && client.wheels[0][1] == 130);
    CHECK(client.wheels[1][0] == -240 && client.wheels[1][2] == 240);
    CHECK(client.shortcuts == 0);
}

} // namespace

int main() {
    check_composer_pairs_astral();
    check_composer_bmp_direct();
    check_composer_lone_low_is_dropped();
    check_composer_dangling_high_dropped();
    check_shell_forwards_text_to_body();
    check_shell_drops_text_for_unknown_window();
    check_shell_forwards_wheel_to_body();
    return 0;
}
