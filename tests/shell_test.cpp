#include "tabengine/shell.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPixmap.h"

#include <algorithm>
#include "check.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

class FakePlatform final : public tabengine::IPlatform {
public:
    void set_event_handler(std::function<void(const tabengine::Event&)> handler) override {
        handler_ = std::move(handler);
    }
    void set_caption_hit_handler(std::function<bool(tabengine::WindowId, tabengine::Point)>) override {}
    void set_wake_handler(std::function<void()> handler) override { wake_handler_ = std::move(handler); }
    void wake() override { ++wake_posts_; }
    // Fakes queue wakes instead of posting: the test pumps them on the
    // thread that would otherwise own the message loop.
    void pump_wakes() {
        while (wake_posts_ > 0) {
            --wake_posts_;
            if (wake_handler_) wake_handler_();
        }
    }
    bool create(tabengine::WindowId id, tabengine::Rect bounds, std::string_view title, bool) override {
        windows_.emplace(id, bounds);
        titles_[id] = title;
        return true;
    }
    void show(tabengine::WindowId id) override {
        if (captured_ && captured_ != id) {
            const auto old = captured_;
            captured_ = 0;
            handler_({tabengine::EventType::CaptureLost, old});
        }
    }
    void destroy(tabengine::WindowId id) override { windows_.erase(id); }
    void invalidate(tabengine::WindowId) override { ++invalidations_; }
    double monotonic_seconds() const override { return now; }
    void request_animation_frame(tabengine::WindowId) override { ++frame_requests_; }
    void capture_pointer(tabengine::WindowId id) override { captured_ = id; }
    void release_pointer() override { captured_ = 0; }
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
        return attach_target_;
    }
    bool supports_native_move_loop() const override { return true; }
    void set_client_origin(tabengine::WindowId id, tabengine::Point screen) override {
        windows_.at(id).x = screen.x;
        windows_.at(id).y = screen.y;
    }
    tabengine::MoveLoopResult run_native_move_loop(tabengine::WindowId id) override {
        handler_({tabengine::EventType::Moving, id, {}, attach_point_});
        return ended_ ? tabengine::MoveLoopResult::Canceled : tabengine::MoveLoopResult::Completed;
    }
    void end_native_move_loop(tabengine::WindowId) override { ended_ = true; }
    int run() override { return 0; }
    void emit(tabengine::Event event) {
        event.size = client_size(event.window);
        handler_(event);
    }
    void attach_to(tabengine::WindowId target, tabengine::Point point) {
        attach_target_ = target;
        attach_point_ = point;
    }
    void resize_without_event(tabengine::WindowId id, tabengine::Size size) {
        windows_.at(id).width = size.width;
        windows_.at(id).height = size.height;
    }
    bool ended() const { return ended_; }
    int invalidations() const { return invalidations_; }
    const std::string& title(tabengine::WindowId id) const { return titles_.at(id); }
    double now = 0.0;

private:
    std::unordered_map<tabengine::WindowId, tabengine::Rect> windows_;
    std::unordered_map<tabengine::WindowId, std::string> titles_;
    std::function<void(const tabengine::Event&)> handler_;
    std::function<void()> wake_handler_;
    int wake_posts_ = 0;
    tabengine::WindowId attach_target_ = 0;
    tabengine::Point attach_point_{};
    bool ended_ = false;
    int invalidations_ = 0;
    int frame_requests_ = 0;
    tabengine::WindowId captured_ = 0;
};

class Client final : public tabengine::IClient {
public:
    tabengine::NewTab create_tab() override {
        const auto id = next_++;
        return {id, "Tab " + std::to_string(id)};
    }
    std::string window_title() override { return "Test Product"; }
    void tab_closed(tabengine::ContentId id) override { closed_.push_back(id); }
    void paint_body(tabengine::WindowId, tabengine::TabId, SkCanvas&, tabengine::Rect) override {}
    void body_event(const tabengine::Event& event, tabengine::Rect) override {
        last_body_ctrl = event.ctrl;
        last_body_shift = event.shift;
        last_body_button = event.button;
        ++body_events_;
    }
    void paint_tab_icon(tabengine::WindowId, tabengine::TabId tab, SkCanvas&,
                        tabengine::Rect bounds) override {
        icon_x_[tab] = bounds.x;
        paint_order_.push_back(tab);
    }
    std::string hover_card_subtitle(tabengine::WindowId, tabengine::TabId,
                                    tabengine::ContentId) override { return "Preview"; }
    void paint_hover_card_preview(tabengine::WindowId, tabengine::TabId,
                                  tabengine::ContentId, SkCanvas&, tabengine::Rect) override {
        ++preview_paints_;
    }
    const std::vector<tabengine::ContentId>& closed() const { return closed_; }
    int icon_x(tabengine::TabId tab) const { return icon_x_.at(tab); }
    int preview_paints() const { return preview_paints_; }
    void clear_paint_order() { paint_order_.clear(); }
    const std::vector<tabengine::TabId>& paint_order() const { return paint_order_; }
    bool last_body_ctrl = false;
    bool last_body_shift = false;
    tabengine::PointerButton last_body_button = tabengine::PointerButton::Left;
    int body_events() const { return body_events_; }

private:
    tabengine::ContentId next_ = 1;
    std::vector<tabengine::ContentId> closed_;
    std::unordered_map<tabengine::TabId, int> icon_x_;
    std::vector<tabengine::TabId> paint_order_;
    int preview_paints_ = 0;
    int body_events_ = 0;
};

void check_body_release_and_capture_loss_reach_client() {
    FakePlatform platform;
    auto renderer = tabengine::make_skia_raster_renderer();
    Client client;
    tabengine::Shell shell(platform, *renderer, client);
    const auto window = shell.open_window({100, 100, 900, 600});
    const int before = client.body_events();
    platform.emit({tabengine::EventType::PointerDown, window, {300, 200}});
    CHECK(client.body_events() == before + 1);
    // A body editor can own pointer capture and release above the strip.
    platform.emit({tabengine::EventType::PointerUp, window, {300, 10}});
    CHECK(client.body_events() == before + 2);
    platform.emit({tabengine::EventType::CaptureLost, window});
    CHECK(client.body_events() == before + 3);
}

void check_non_left_pointer_routing() {
    FakePlatform platform;
    auto renderer = tabengine::make_skia_raster_renderer();
    Client client;
    tabengine::Shell shell(platform, *renderer, client);
    const auto window = shell.open_window({100, 100, 900, 600});
    (void)shell.new_tab(window);
    const auto* tabs = shell.model().window(window);
    const std::size_t tabs_before = tabs->tabs.size();
    const auto first = tabs->tabs.front().id;
    const auto second = tabs->tabs[1].id;
    (void)shell.select_tab(window, second);
    tabengine::Rect first_bounds{};
    for (const auto& target : shell.chrome_targets(window)) {
        if (target.kind == tabengine::Shell::ChromeTarget::Kind::Tab &&
            target.tab == first) {
            first_bounds = target.bounds;
        }
    }
    CHECK(first_bounds.width > 0);
    const int tab_x = first_bounds.x + first_bounds.width / 2;
    const int tab_y = first_bounds.y + first_bounds.height / 2;

    // A right press in the body reaches the client with its button.
    int before = client.body_events();
    tabengine::Event body_right{tabengine::EventType::PointerDown, window, {300, 300}};
    body_right.button = tabengine::PointerButton::Right;
    platform.emit(body_right);
    CHECK(client.body_events() == before + 1);
    CHECK(client.last_body_button == tabengine::PointerButton::Right);
    // So does its release while nothing is being dragged.
    tabengine::Event body_right_up{tabengine::EventType::PointerUp, window, {300, 300}};
    body_right_up.button = tabengine::PointerButton::Right;
    platform.emit(body_right_up);
    CHECK(client.body_events() == before + 2);
    // A middle press in the body rides the same plumbing.
    tabengine::Event body_middle{tabengine::EventType::PointerDown, window, {300, 300}};
    body_middle.button = tabengine::PointerButton::Middle;
    platform.emit(body_middle);
    CHECK(client.body_events() == before + 3);
    CHECK(client.last_body_button == tabengine::PointerButton::Middle);

    // A right press on the inactive tab selects it but arms no drag: a move
    // far beyond the slop reorders nothing, and no tab tears off.
    before = client.body_events();
    tabengine::Event tab_right{tabengine::EventType::PointerDown, window,
                               {tab_x, tab_y}};
    tab_right.button = tabengine::PointerButton::Right;
    platform.emit(tab_right);
    CHECK(client.body_events() == before); // strip press, not a body event
    CHECK(shell.model().window(window)->active == first);
    platform.emit({tabengine::EventType::PointerMove, window,
                   {tab_x + 120, tab_y}, {tab_x + 220, tab_y}});
    CHECK(shell.model().window(window)->tabs.size() == tabs_before);
    CHECK(shell.model().window(window)->tabs.front().id == first);
    CHECK(shell.model().window_ids().size() == 1);

    // Right-pressing chrome does nothing: no caption button, no new tab.
    const auto windows_before = shell.model().window_ids().size();
    tabengine::Event caption_right{tabengine::EventType::PointerDown, window,
                                   {880, 12}};
    caption_right.button = tabengine::PointerButton::Right;
    platform.emit(caption_right);
    const auto strip = tabengine::Layout::tab_strip(900, tabs_before, 1.0f);
    tabengine::Event plus_right{
        tabengine::EventType::PointerDown, window,
        {strip.new_tab.x + strip.new_tab.width / 2,
         strip.new_tab.y + strip.new_tab.height / 2}};
    plus_right.button = tabengine::PointerButton::Right;
    platform.emit(plus_right);
    CHECK(shell.model().window_ids().size() == windows_before);
    CHECK(shell.model().window(window)->tabs.size() == tabs_before);
    CHECK(client.body_events() == before);

    // A right release during an armed left drag does not settle it: the
    // drag visual keeps following moves until the left release.
    (void)shell.select_tab(window, second);
    platform.emit({tabengine::EventType::PointerDown, window, {tab_x, tab_y},
                   {tab_x + 100, tab_y + 100}});
    platform.emit({tabengine::EventType::PointerMove, window,
                   {tab_x + 60, tab_y}, {tab_x + 160, tab_y + 100}});
    tabengine::Event stray_up{tabengine::EventType::PointerUp, window,
                              {tab_x + 60, tab_y}};
    stray_up.button = tabengine::PointerButton::Right;
    platform.emit(stray_up);
    const int invalidated = platform.invalidations();
    platform.emit({tabengine::EventType::PointerMove, window,
                   {tab_x + 90, tab_y}, {tab_x + 190, tab_y + 100}});
    CHECK(platform.invalidations() > invalidated);
    platform.emit({tabengine::EventType::PointerUp, window,
                   {tab_x + 90, tab_y}, {tab_x + 190, tab_y + 100}});
    CHECK(shell.model().window(window)->tabs.size() == tabs_before);
}

void check_shift_modified_chords_reach_the_client() {
    FakePlatform platform;
    auto renderer = tabengine::make_skia_raster_renderer();
    Client client;
    tabengine::Shell shell(platform, *renderer, client);
    const auto window = shell.open_window({100, 100, 900, 600});
    const std::size_t windows_before = shell.model().window_ids().size();
    const std::size_t tabs_before = shell.model().window(window)->tabs.size();

    // Ctrl+Shift+N and Ctrl+Shift+T are product chords (new folder, reopen
    // closed tab): the engine's unmodified defaults must not claim them, and
    // they must reach the client's body with their modifiers intact.
    int before = client.body_events();
    platform.emit({tabengine::EventType::KeyDown, window, {}, {}, {}, 'N', true, true});
    CHECK(client.body_events() == before + 1);
    CHECK(client.last_body_ctrl && client.last_body_shift);
    CHECK(shell.model().window_ids().size() == windows_before); // no new window

    before = client.body_events();
    platform.emit({tabengine::EventType::KeyDown, window, {}, {}, {}, 'T', true, true});
    CHECK(client.body_events() == before + 1);
    CHECK(shell.model().window(window)->tabs.size() == tabs_before); // no new tab

    // The plain engine defaults still work.
    platform.emit({tabengine::EventType::KeyDown, window, {}, {}, {}, 'T', true});
    CHECK(shell.model().window(window)->tabs.size() == tabs_before + 1);
    platform.emit({tabengine::EventType::KeyDown, window, {}, {}, {}, 'N', true});
    CHECK(shell.model().window_ids().size() == windows_before + 1);
}

void check_hover_paint_order() {
    FakePlatform platform;
    auto renderer = tabengine::make_skia_raster_renderer();
    Client client;
    tabengine::Shell shell(platform, *renderer, client);
    const auto window = shell.open_window({100, 100, 900, 600});
    CHECK(platform.title(window) == "Test Product");
    const auto first = shell.model().window(window)->tabs.front().id;
    const auto second = shell.new_tab(window);
    const auto active = shell.new_tab(window);
    platform.now = 0.25;
    platform.emit({tabengine::EventType::AnimationFrame, window});
    platform.emit({tabengine::EventType::PointerMove, window, {70, 20}, {170, 120}});
    client.clear_paint_order();
    platform.emit({tabengine::EventType::Paint, window});
    CHECK((client.paint_order() == std::vector<tabengine::TabId>{second, first, active}));
}

void check_hover_card_visual() {
    FakePlatform platform;
    auto renderer = tabengine::make_skia_raster_renderer();
    Client client;
    tabengine::Shell shell(platform, *renderer, client);
    const auto window = shell.open_window({100, 100, 900, 600});
    const auto first = shell.model().window(window)->tabs.front().id;
    (void)shell.new_tab(window);
    platform.now = 0.25;
    platform.emit({tabengine::EventType::AnimationFrame, window});
    CHECK(shell.select_tab(window, first));
    const auto pixel = [&] {
        platform.emit({tabengine::EventType::Paint, window});
        SkPixmap pixels;
        CHECK(renderer->canvas(window)->peekPixels(&pixels));
        return pixels.getColor(250, 60);
    };
    const SkColor body = pixel();
    platform.emit({tabengine::EventType::PointerMove, window, {70, 20}, {170, 120}});
    platform.now = 0.54;
    platform.emit({tabengine::EventType::AnimationFrame, window});
    CHECK(pixel() == body);
    platform.now = 0.55;
    platform.emit({tabengine::EventType::AnimationFrame, window});
    platform.now = 0.65;
    platform.emit({tabengine::EventType::AnimationFrame, window});
    CHECK(pixel() != body);
    platform.now = 0.76;
    platform.emit({tabengine::EventType::AnimationFrame, window});
    platform.emit({tabengine::EventType::PointerMove, window, {320, 20}, {420, 120}});
    platform.emit({tabengine::EventType::Paint, window});
    CHECK(client.preview_paints() > 0);
    platform.emit({tabengine::EventType::PointerMove, window, {250, 60}, {350, 160}});
    platform.emit({tabengine::EventType::PointerLeave, window});
    platform.now = 0.92;
    platform.emit({tabengine::EventType::AnimationFrame, window});
    CHECK(pixel() == body);
}

void check_hover_and_reorder_visual() {
    FakePlatform platform;
    auto renderer = tabengine::make_skia_raster_renderer();
    Client client;
    tabengine::Shell shell(platform, *renderer, client);
    const auto window = shell.open_window({100, 100, 900, 600});
    const auto first = shell.model().window(window)->tabs[0].id;
    const auto second = shell.new_tab(window);
    const auto third = shell.new_tab(window);
    platform.now = 0.25;
    platform.emit({tabengine::EventType::AnimationFrame, window});
    CHECK(shell.select_tab(window, first));
    const auto pixel = [&] {
        platform.emit({tabengine::EventType::Paint, window});
        SkPixmap pixels;
        CHECK(renderer->canvas(window)->peekPixels(&pixels));
        return pixels.getColor(400, 15);
    };
    const SkColor idle = pixel();
    platform.emit({tabengine::EventType::PointerMove, window, {400, 15}, {500, 115}});
    platform.now = 0.31;
    platform.emit({tabengine::EventType::AnimationFrame, window});
    const SkColor mid_hover = pixel();
    platform.now = 0.38;
    platform.emit({tabengine::EventType::AnimationFrame, window});
    const SkColor full_hover = pixel();
    CHECK(idle != mid_hover && mid_hover != full_hover);
    platform.emit({tabengine::EventType::PointerLeave, window});
    platform.now = 0.51;
    platform.emit({tabengine::EventType::AnimationFrame, window});
    CHECK(pixel() == idle);

    platform.emit({tabengine::EventType::PointerDown, window, {70, 20}, {170, 120}});
    platform.emit({tabengine::EventType::PointerMove, window, {700, 20}, {800, 120}});
    CHECK(shell.model().window(window)->tabs[0].id == second);
    platform.emit({tabengine::EventType::Paint, window});
    const int neighbor_start = client.icon_x(second);
    platform.now = 0.61;
    platform.emit({tabengine::EventType::AnimationFrame, window});
    platform.emit({tabengine::EventType::Paint, window});
    const int neighbor_mid = client.icon_x(second);
    platform.now = 0.72;
    platform.emit({tabengine::EventType::AnimationFrame, window});
    platform.emit({tabengine::EventType::Paint, window});
    const int neighbor_end = client.icon_x(second);
    CHECK(neighbor_start > neighbor_mid && neighbor_mid > neighbor_end);
    CHECK(shell.model().window(window)->tabs[2].id == first);
    CHECK(third != first && third != second);
}

void check_close_visual() {
    FakePlatform platform;
    auto renderer = tabengine::make_skia_raster_renderer();
    Client client;
    tabengine::Shell shell(platform, *renderer, client);
    const auto window = shell.open_window({100, 100, 900, 600});
    const auto closing = shell.new_tab(window);
    platform.now = 0.25;
    platform.emit({tabengine::EventType::AnimationFrame, window});
    const auto pixel = [&] {
        platform.emit({tabengine::EventType::Paint, window});
        SkPixmap pixels;
        CHECK(renderer->canvas(window)->peekPixels(&pixels));
        return pixels.getColor(400, 15);
    };
    CHECK(shell.close_tab(window, closing));
    const SkColor before_shrink = pixel();
    CHECK(client.closed().empty());
    platform.now = 0.35;
    platform.emit({tabengine::EventType::AnimationFrame, window});
    CHECK(pixel() != before_shrink);
    CHECK(shell.model().window(window)->tabs.size() == 2);
    platform.now = 0.46;
    platform.emit({tabengine::EventType::AnimationFrame, window});
    CHECK(shell.model().window(window)->tabs.size() == 1);
    CHECK(client.closed().size() == 1);
}

} // namespace

int main() {
    FakePlatform platform;
    auto renderer = tabengine::make_skia_raster_renderer();
    Client client;
    tabengine::Shell shell(platform, *renderer, client);
    const auto source = shell.open_window({100, 100, 900, 600});
    const auto moving_tab = shell.model().window(source)->tabs.front().id;
    const auto moving_content = shell.model().window(source)->tabs.front().content;
    (void)shell.new_tab(source);
    const auto target = shell.open_window({500, 100, 900, 600});
    platform.attach_to(target, {550, 120});
    const auto pixel_at = [&](int x, int y) {
        platform.emit({tabengine::EventType::Paint, source});
        SkPixmap pixels;
        CHECK(renderer->canvas(source)->peekPixels(&pixels));
        return pixels.getColor(x, y);
    };
    // Pointer presses carry their modifier keys to the body verbatim - hosts
    // implement Ctrl/Shift-click selection on them.
    platform.emit({tabengine::EventType::PointerDown, source, {70, 300}, {170, 400}, {},
                   0, true, true, false});
    CHECK(client.body_events() > 0 && client.last_body_ctrl && client.last_body_shift);
    platform.emit({tabengine::EventType::PointerDown, source, {70, 300}, {170, 400}});
    CHECK(!client.last_body_ctrl && !client.last_body_shift);
    const SkColor opening = pixel_at(350, 15);
    platform.now = 0.25;
    platform.emit({tabengine::EventType::AnimationFrame, source});
    CHECK(pixel_at(350, 15) != opening);
    platform.emit({tabengine::EventType::PointerDown, source, {70, 20}, {170, 120}});
    const SkColor settled = pixel_at(290, 15);
    const int before_move = platform.invalidations();
    platform.emit({tabengine::EventType::PointerMove, source, {90, 20}, {190, 120}});
    CHECK(platform.invalidations() > before_move);
    CHECK(shell.model().window(source)->tabs.front().id == moving_tab);
    const SkColor following = pixel_at(290, 15);
    CHECK(following != settled);
    const int before_release = platform.invalidations();
    platform.emit({tabengine::EventType::PointerUp, source, {90, 20}, {190, 120}});
    CHECK(platform.invalidations() > before_release);
    platform.now = 0.50;
    platform.emit({tabengine::EventType::AnimationFrame, source});
    CHECK(pixel_at(290, 15) == settled);

    platform.emit({tabengine::EventType::PointerDown, source, {70, 20}, {170, 120}});
    platform.emit({tabengine::EventType::PointerMove, source, {70, 100}, {170, 200}});

    CHECK(platform.ended());
    CHECK(shell.model().window_ids().size() == 2);
    CHECK(shell.model().window(source)->tabs.size() == 1);
    const auto* destination = shell.model().window(target);
    CHECK(destination->tabs.size() == 2);
    CHECK(destination->active == moving_tab);
    bool preserved = false;
    for (const auto& tab : destination->tabs) {
        if (tab.id == moving_tab && tab.content == moving_content) preserved = true;
    }
    CHECK(preserved);
    CHECK(client.closed().empty());
    platform.resize_without_event(target, {640, 480});
    platform.emit({tabengine::EventType::Paint, target});
    CHECK(renderer->info(target).surface_size.width == 640);
    CHECK(renderer->info(target).surface_size.height == 480);

    const auto before_cycle = shell.model().window(target)->active;
    platform.emit({tabengine::EventType::KeyDown, target, {}, {}, {}, 9, true});
    CHECK(shell.model().window(target)->active != before_cycle);
    platform.emit({tabengine::EventType::KeyDown, target, {}, {}, {}, 9, true, true});
    CHECK(shell.model().window(target)->active == before_cycle);

    const auto plus = tabengine::Layout::tab_strip(640, destination->tabs.size(), 1.0f).new_tab;
    platform.emit({tabengine::EventType::PointerDown, target,
                   {plus.x + plus.width / 2, plus.y + plus.height / 2}, {0, 0}});
    CHECK(shell.model().window(target)->tabs.size() == 3);

    platform.emit({tabengine::EventType::KeyDown, target, {}, {}, {}, 'N', true});
    const auto ids = shell.model().window_ids();
    CHECK(ids.size() == 3);
    const auto new_window = *std::max_element(ids.begin(), ids.end());
    platform.emit({tabengine::EventType::KeyDown, new_window, {}, {}, {}, 'W', true});
    CHECK(shell.model().window_ids().size() == 2);
    CHECK(client.closed().size() == 1);
    check_close_visual();
    check_hover_and_reorder_visual();
    check_hover_card_visual();
    check_hover_paint_order();
    check_shift_modified_chords_reach_the_client();
    check_body_release_and_capture_loss_reach_client();
    check_non_left_pointer_routing();
    return 0;
}
