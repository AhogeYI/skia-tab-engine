#include "tabengine/shell.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPixmap.h"

#include <algorithm>
#include <cassert>
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
    bool create(tabengine::WindowId id, tabengine::Rect bounds, std::string_view, bool) override {
        windows_.emplace(id, bounds);
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
    double now = 0.0;

private:
    std::unordered_map<tabengine::WindowId, tabengine::Rect> windows_;
    std::function<void(const tabengine::Event&)> handler_;
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
    void tab_closed(tabengine::ContentId id) override { closed_.push_back(id); }
    void paint_body(tabengine::WindowId, tabengine::TabId, SkCanvas&, tabengine::Rect) override {}
    void body_event(const tabengine::Event&, tabengine::Rect) override {}
    void paint_tab_icon(tabengine::WindowId, tabengine::TabId tab, SkCanvas&,
                        tabengine::Rect bounds) override { icon_x_[tab] = bounds.x; }
    const std::vector<tabengine::ContentId>& closed() const { return closed_; }
    int icon_x(tabengine::TabId tab) const { return icon_x_.at(tab); }

private:
    tabengine::ContentId next_ = 1;
    std::vector<tabengine::ContentId> closed_;
    std::unordered_map<tabengine::TabId, int> icon_x_;
};

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
    assert(shell.select_tab(window, first));
    const auto pixel = [&] {
        platform.emit({tabengine::EventType::Paint, window});
        SkPixmap pixels;
        assert(renderer->canvas(window)->peekPixels(&pixels));
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
    assert(idle != mid_hover && mid_hover != full_hover);
    platform.emit({tabengine::EventType::PointerLeave, window});
    platform.now = 0.51;
    platform.emit({tabengine::EventType::AnimationFrame, window});
    assert(pixel() == idle);

    platform.emit({tabengine::EventType::PointerDown, window, {70, 20}, {170, 120}});
    platform.emit({tabengine::EventType::PointerMove, window, {700, 20}, {800, 120}});
    assert(shell.model().window(window)->tabs[0].id == second);
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
    assert(neighbor_start > neighbor_mid && neighbor_mid > neighbor_end);
    assert(shell.model().window(window)->tabs[2].id == first);
    assert(third != first && third != second);
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
        assert(renderer->canvas(window)->peekPixels(&pixels));
        return pixels.getColor(400, 15);
    };
    assert(shell.close_tab(window, closing));
    const SkColor before_shrink = pixel();
    assert(client.closed().empty());
    platform.now = 0.35;
    platform.emit({tabengine::EventType::AnimationFrame, window});
    assert(pixel() != before_shrink);
    assert(shell.model().window(window)->tabs.size() == 2);
    platform.now = 0.46;
    platform.emit({tabengine::EventType::AnimationFrame, window});
    assert(shell.model().window(window)->tabs.size() == 1);
    assert(client.closed().size() == 1);
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
        assert(renderer->canvas(source)->peekPixels(&pixels));
        return pixels.getColor(x, y);
    };
    const SkColor opening = pixel_at(350, 15);
    platform.now = 0.25;
    platform.emit({tabengine::EventType::AnimationFrame, source});
    assert(pixel_at(350, 15) != opening);
    platform.emit({tabengine::EventType::PointerDown, source, {70, 20}, {170, 120}});
    const SkColor settled = pixel_at(290, 15);
    const int before_move = platform.invalidations();
    platform.emit({tabengine::EventType::PointerMove, source, {90, 20}, {190, 120}});
    assert(platform.invalidations() > before_move);
    assert(shell.model().window(source)->tabs.front().id == moving_tab);
    const SkColor following = pixel_at(290, 15);
    assert(following != settled);
    const int before_release = platform.invalidations();
    platform.emit({tabengine::EventType::PointerUp, source, {90, 20}, {190, 120}});
    assert(platform.invalidations() > before_release);
    platform.now = 0.50;
    platform.emit({tabengine::EventType::AnimationFrame, source});
    assert(pixel_at(290, 15) == settled);

    platform.emit({tabengine::EventType::PointerDown, source, {70, 20}, {170, 120}});
    platform.emit({tabengine::EventType::PointerMove, source, {70, 100}, {170, 200}});

    assert(platform.ended());
    assert(shell.model().window_ids().size() == 2);
    assert(shell.model().window(source)->tabs.size() == 1);
    const auto* destination = shell.model().window(target);
    assert(destination->tabs.size() == 2);
    assert(destination->active == moving_tab);
    bool preserved = false;
    for (const auto& tab : destination->tabs) {
        if (tab.id == moving_tab && tab.content == moving_content) preserved = true;
    }
    assert(preserved);
    assert(client.closed().empty());
    platform.resize_without_event(target, {640, 480});
    platform.emit({tabengine::EventType::Paint, target});
    assert(renderer->info(target).surface_size.width == 640);
    assert(renderer->info(target).surface_size.height == 480);

    const auto before_cycle = shell.model().window(target)->active;
    platform.emit({tabengine::EventType::KeyDown, target, {}, {}, {}, 9, true});
    assert(shell.model().window(target)->active != before_cycle);
    platform.emit({tabengine::EventType::KeyDown, target, {}, {}, {}, 9, true, true});
    assert(shell.model().window(target)->active == before_cycle);

    const auto plus = tabengine::Layout::tab_strip(640, destination->tabs.size(), 1.0f).new_tab;
    platform.emit({tabengine::EventType::PointerDown, target,
                   {plus.x + plus.width / 2, plus.y + plus.height / 2}, {0, 0}});
    assert(shell.model().window(target)->tabs.size() == 3);

    platform.emit({tabengine::EventType::KeyDown, target, {}, {}, {}, 'N', true});
    const auto ids = shell.model().window_ids();
    assert(ids.size() == 3);
    const auto new_window = *std::max_element(ids.begin(), ids.end());
    platform.emit({tabengine::EventType::KeyDown, new_window, {}, {}, {}, 'W', true});
    assert(shell.model().window_ids().size() == 2);
    assert(client.closed().size() == 1);
    check_close_visual();
    check_hover_and_reorder_visual();
    return 0;
}
