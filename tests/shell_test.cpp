#include "tabengine/shell.h"

#include "include/core/SkCanvas.h"

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
    void invalidate(tabengine::WindowId) override {}
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
    void run_native_move_loop(tabengine::WindowId id) override {
        handler_({tabengine::EventType::Moving, id, {}, attach_point_});
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
    bool ended() const { return ended_; }

private:
    std::unordered_map<tabengine::WindowId, tabengine::Rect> windows_;
    std::function<void(const tabengine::Event&)> handler_;
    tabengine::WindowId attach_target_ = 0;
    tabengine::Point attach_point_{};
    bool ended_ = false;
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
    const std::vector<tabengine::ContentId>& closed() const { return closed_; }

private:
    tabengine::ContentId next_ = 1;
    std::vector<tabengine::ContentId> closed_;
};

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

    platform.emit({tabengine::EventType::PointerDown, source, {30, 20}, {130, 120}});
    platform.emit({tabengine::EventType::PointerMove, source, {40, 100}, {140, 200}});

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
    return 0;
}
