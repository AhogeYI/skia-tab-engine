#include "tabengine/shell.h"
#include "tabengine/text.h"
#include "chrome_metrics.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRect.h"
#include "include/core/SkRRect.h"

#include <algorithm>
#include <cmath>

namespace tabengine {
namespace {

SkRect skrect(Rect r) {
    return SkRect::MakeXYWH(static_cast<float>(r.x), static_cast<float>(r.y),
                            static_cast<float>(r.width), static_cast<float>(r.height));
}

SkPath tab_face(Rect bounds, float scale, bool active) {
    using M = detail::ChromeMetrics;
    SkPathBuilder path;
    const float width = static_cast<float>(bounds.width);
    const float height = static_cast<float>(bounds.height);
    if (width <= 0 || height <= 0) return path.detach();
    const float shoulder = std::min(static_cast<float>(M::bottom_radius) * scale, width / 2.0f);
    const float inner_width = width - 2.0f * shoulder;
    if (inner_width < 1.0f) {
        path.addRect(SkRect::MakeWH(width, height));
    } else if (!active) {
        const float radius = std::min({static_cast<float>(M::top_radius) * scale,
                                       inner_width / 2.0f, height / 2.0f});
        path.addRRect(SkRRect::MakeRectXY(
            SkRect::MakeLTRB(shoulder, 0, width - shoulder, height), radius, radius));
    } else {
        const float radius = std::min({static_cast<float>(M::top_radius) * scale,
                                       inner_width / 2.0f, height / 2.0f});
        const float extension = std::min(shoulder, height);
        path.moveTo(0, height);
        path.arcTo(SkPoint{extension, extension}, 0, SkPathBuilder::kSmall_ArcSize,
                   SkPathDirection::kCCW, SkPoint{shoulder, height - extension});
        path.lineTo(shoulder, radius);
        path.arcTo(SkPoint{radius, radius}, 0, SkPathBuilder::kSmall_ArcSize,
                   SkPathDirection::kCW, SkPoint{shoulder + radius, 0});
        path.lineTo(width - shoulder - radius, 0);
        path.arcTo(SkPoint{radius, radius}, 0, SkPathBuilder::kSmall_ArcSize,
                   SkPathDirection::kCW, SkPoint{width - shoulder, radius});
        path.lineTo(width - shoulder, height - extension);
        path.arcTo(SkPoint{extension, extension}, 0, SkPathBuilder::kSmall_ArcSize,
                   SkPathDirection::kCCW, SkPoint{width, height});
        path.close();
    }
    return path.detach();
}

Rect caption_button(const StripLayout& strip, int index, float scale) {
    using M = detail::ChromeMetrics;
    const int x = strip.caption_start + static_cast<int>(std::lround(
        (M::caption_button_width * index + M::caption_button_spacing * std::max(0, index - 1)) * scale));
    const int right = strip.caption_start + static_cast<int>(std::lround(
        (M::caption_button_width * (index + 1) + M::caption_button_spacing * index) * scale));
    return {x, 0, right - x, strip.height};
}

void text(SkCanvas& canvas, const std::string& value, float x, float y, float size, SkColor color) {
    paint_ui_text(canvas, value, x, y, size, color);
}

} // namespace

Shell::Shell(IPlatform& platform, IRenderer& renderer, IClient& client)
    : platform_(platform), renderer_(renderer), client_(client) {
    platform_.set_event_handler([this](const Event& event) { on_event(event); });
    platform_.set_caption_hit_handler(
        [this](WindowId window, Point client) { return caption_hit(window, client); });
}

Shell::~Shell() {
    platform_.set_event_handler({});
    platform_.set_caption_hit_handler({});
    for (WindowId id : model_.window_ids()) {
        busy_windows_.insert(id);
        destroy_window_contents(id);
        busy_windows_.erase(id);
    }
}

void Shell::set_theme(Theme theme) {
    theme_ = theme;
    for (WindowId id : model_.window_ids()) platform_.invalidate(id);
}

void Shell::set_chrome_options(ChromeOptions options) {
    chrome_options_ = options;
    for (WindowId id : model_.window_ids()) platform_.invalidate(id);
}

WindowId Shell::open_window(Rect bounds, bool with_initial_tab, bool visible) {
    const WindowId id = model_.create_window();
    if (!platform_.create(id, bounds, "Tabbed Window", false)) {
        (void)model_.remove_window(id);
        return 0;
    }
    if (!renderer_.attach(id, platform_.native_handle(id), platform_.client_size(id))) {
        platform_.destroy(id);
        (void)model_.remove_window(id);
        return 0;
    }
    client_.body_geometry_changed(id, body_bounds(id), platform_.scale(id));
    if (!model_.window(id)) return 0;
    if (with_initial_tab) (void)new_tab(id);
    if (!model_.window(id)) return 0;
    if (visible) platform_.show(id);
    platform_.invalidate(id);
    return id;
}

TabId Shell::new_tab(WindowId window) {
    const WindowTabs* w = model_.window(window);
    if (!w || !busy_windows_.insert(window).second) return 0;
    const TabId previous = w->active;
    NewTab created = client_.create_tab();
    const TabId id = model_.add_tab(window, created.content, std::move(created.title));
    if (id) {
        (void)model_.select_tab(window, id);
        client_.tab_attached(window, id, created.content);
        if (previous != id) client_.active_tab_changed(window, previous, id);
        platform_.invalidate(window);
    } else {
        client_.tab_closed(created.content);
    }
    busy_windows_.erase(window);
    return id;
}

bool Shell::close_tab(WindowId window, TabId tab) {
    const WindowTabs* w = model_.window(window);
    if (!w || !busy_windows_.insert(window).second) return false;
    const auto it = std::find_if(w->tabs.begin(), w->tabs.end(),
                                 [tab](const Tab& item) { return item.id == tab; });
    if (it == w->tabs.end()) {
        busy_windows_.erase(window);
        return false;
    }
    const ContentId content = it->content;
    const TabId previous = w->active;
    const bool last = w->tabs.size() == 1;
    if (!client_.allow_close_tab(window, tab, content) ||
        (last && !client_.allow_close_window(window)) ||
        !model_.close_tab(window, tab)) {
        busy_windows_.erase(window);
        return false;
    }
    const TabId current = model_.window(window)->active;
    if (previous != current) client_.active_tab_changed(window, previous, current);
    client_.tab_detached(window, tab, content);
    client_.tab_closed(content);
    if (const WindowTabs* remaining = model_.window(window)) {
        if (remaining->tabs.empty()) {
            if (drag_.window == window) {
                if (drag_.phase != DragPhase::NativeWindow) platform_.release_pointer();
                drag_ = {};
            }
            clear_hover(window);
            request_destroy(window);
        }
        else platform_.invalidate(window);
    }
    busy_windows_.erase(window);
    return true;
}

bool Shell::select_tab(WindowId window, TabId tab) {
    const WindowTabs* w = model_.window(window);
    if (!w || !busy_windows_.insert(window).second) return false;
    const TabId previous = w->active;
    if (!model_.select_tab(window, tab)) {
        busy_windows_.erase(window);
        return false;
    }
    if (previous != tab) client_.active_tab_changed(window, previous, tab);
    platform_.invalidate(window);
    busy_windows_.erase(window);
    return true;
}

bool Shell::move_tab(WindowId window, TabId tab, std::size_t index) {
    if (busy_windows_.contains(window) || !model_.move_tab(window, tab, index)) return false;
    platform_.invalidate(window);
    return true;
}

bool Shell::transfer_tab(WindowId from, WindowId to, TabId tab, std::size_t index) {
    if (from == to) return move_tab(from, tab, index);
    const WindowTabs* source = model_.window(from);
    const WindowTabs* target = model_.window(to);
    if (!source || !target || busy_windows_.contains(from) || busy_windows_.contains(to))
        return false;
    const auto it = std::find_if(source->tabs.begin(), source->tabs.end(),
                                 [tab](const Tab& item) { return item.id == tab; });
    if (it == source->tabs.end()) return false;
    const ContentId content = it->content;
    const TabId source_active = source->active;
    const TabId target_active = target->active;
    busy_windows_.insert(from);
    busy_windows_.insert(to);
    if (!model_.transfer_tab(from, to, tab, index)) {
        busy_windows_.erase(from);
        busy_windows_.erase(to);
        return false;
    }
    client_.tab_detached(from, tab, content);
    client_.tab_attached(to, tab, content);
    const TabId source_current = model_.window(from)->active;
    if (source_active != source_current)
        client_.active_tab_changed(from, source_active, source_current);
    if (target_active != tab) client_.active_tab_changed(to, target_active, tab);
    platform_.invalidate(to);
    if (model_.window(from)->tabs.empty()) {
        if (drag_.window == from) {
            if (drag_.phase != DragPhase::NativeWindow) platform_.release_pointer();
            drag_ = {};
        }
        clear_hover(from);
        request_destroy(from);
    } else {
        platform_.invalidate(from);
    }
    busy_windows_.erase(from);
    busy_windows_.erase(to);
    return true;
}

bool Shell::update_tab(WindowId window, TabId tab, std::string title,
                       bool loading, bool attention) {
    if (!model_.update_tab(window, tab, std::move(title), loading, attention)) return false;
    platform_.invalidate(window);
    return true;
}

void Shell::close_window(WindowId window) {
    const WindowTabs* w = model_.window(window);
    if (!w || !busy_windows_.insert(window).second) return;
    if (!client_.allow_close_window(window)) {
        busy_windows_.erase(window);
        return;
    }
    for (const Tab& tab : w->tabs) {
        if (!client_.allow_close_tab(window, tab.id, tab.content)) {
            busy_windows_.erase(window);
            return;
        }
    }
    destroy_window_contents(window);
    busy_windows_.erase(window);
}

void Shell::destroy_window_contents(WindowId window) {
    const WindowTabs* w = model_.window(window);
    if (!w) return;
    const TabId active = w->active;
    const std::vector<Tab> tabs = w->tabs;
    if (drag_.window == window) {
        if (drag_.phase != DragPhase::NativeWindow) platform_.release_pointer();
        drag_ = {};
    }
    clear_hover(window);
    if (active) client_.active_tab_changed(window, active, 0);
    for (const Tab& tab : tabs) {
        client_.tab_detached(window, tab.id, tab.content);
        client_.tab_closed(tab.content);
    }
    request_destroy(window);
}

void Shell::request_destroy(WindowId window) {
    if (!model_.remove_window(window)) return;
    pending_destroy_.push_back(window);
    if (dispatch_depth_ == 0) flush_destroy();
}

void Shell::flush_destroy() {
    auto pending = std::move(pending_destroy_);
    pending_destroy_.clear();
    for (WindowId id : pending) {
        renderer_.detach(id);
        platform_.destroy(id);
    }
}

void Shell::on_event(const Event& event) {
    ++dispatch_depth_;
    handle_event(event);
    --dispatch_depth_;
    if (dispatch_depth_ == 0) flush_destroy();
}

void Shell::handle_event(const Event& event) {
    if (!model_.window(event.window)) return;
    switch (event.type) {
    case EventType::Paint: paint(event.window); break;
    case EventType::Resized:
        renderer_.resize(event.window, event.size);
        client_.body_geometry_changed(event.window, body_bounds(event.window),
                                      platform_.scale(event.window));
        if (model_.window(event.window)) platform_.invalidate(event.window);
        break;
    case EventType::DpiChanged:
        renderer_.resize(event.window, platform_.client_size(event.window));
        client_.dpi_changed(event.window, platform_.scale(event.window));
        if (!model_.window(event.window)) break;
        client_.body_geometry_changed(event.window, body_bounds(event.window),
                                      platform_.scale(event.window));
        if (model_.window(event.window)) platform_.invalidate(event.window);
        break;
    case EventType::WindowActivated:
        client_.window_activation_changed(event.window, true);
        break;
    case EventType::WindowDeactivated:
        if (drag_.window == event.window && drag_.phase != DragPhase::NativeWindow) {
            cancel_drag();
        }
        clear_hover(event.window);
        client_.window_activation_changed(event.window, false);
        break;
    case EventType::PlacementChanged: {
        const Point origin = platform_.client_origin(event.window);
        const Size size = platform_.client_size(event.window);
        client_.window_placement_changed(event.window,
                                         {origin.x, origin.y, size.width, size.height},
                                         platform_.scale(event.window));
        break;
    }
    case EventType::PointerDown:
    case EventType::PointerMove:
    case EventType::PointerUp:
    case EventType::PointerLeave:
    case EventType::CaptureLost:
        handle_pointer(event);
        break;
    case EventType::Moving: handle_moving(event); break;
    case EventType::CloseRequested: close_window(event.window); break;
    case EventType::KeyDown: {
        if (event.key == 27 && drag_.window == event.window &&
            drag_.phase != DragPhase::Idle) {
            cancel_drag();
            break;
        }
        if (client_.handle_shortcut(event) || !model_.window(event.window)) break;
        if (event.ctrl && event.key == 'T') {
            (void)new_tab(event.window);
            break;
        }
        if (event.ctrl && event.key == 'W') {
            const WindowTabs* w = model_.window(event.window);
            if (w && w->active) (void)close_tab(event.window, w->active);
            break;
        }
        if (event.ctrl && event.key == 'N') {
            const Size size = platform_.client_size(event.window);
            const Point origin = platform_.client_origin(event.window);
            (void)open_window({origin.x + 40, origin.y + 40,
                               std::max(640, size.width), std::max(480, size.height)});
            break;
        }
        if (event.ctrl && event.key == 9) { // Tab
            const WindowTabs* w = model_.window(event.window);
            if (!w || w->tabs.size() < 2) break;
            const auto it = std::find_if(w->tabs.begin(), w->tabs.end(),
                [w](const Tab& tab) { return tab.id == w->active; });
            const std::size_t current = static_cast<std::size_t>(it - w->tabs.begin());
            const std::size_t count = w->tabs.size();
            const std::size_t next = event.shift ? (current + count - 1) % count
                                                 : (current + 1) % count;
            (void)select_tab(event.window, w->tabs[next].id);
            break;
        }
        const auto strip = layout(event.window);
        client_.body_event(event, {0, strip.height, event.size.width,
                                   std::max(0, event.size.height - strip.height)});
        break;
    }
    }
}

StripLayout Shell::layout(WindowId window) const {
    const auto* w = model_.window(window);
    return Layout::tab_strip(platform_.client_size(window).width, w ? w->tabs.size() : 0,
                             platform_.scale(window), chrome_options_);
}

Rect Shell::body_bounds(WindowId window) const {
    const Size size = platform_.client_size(window);
    const int top = layout(window).height;
    return {0, top, size.width, std::max(0, size.height - top)};
}

TabId Shell::tab_at(WindowId window, Point client) const {
    const auto* w = model_.window(window);
    if (!w) return 0;
    const auto strip = layout(window);
    for (std::size_t i = 0; i < strip.tabs.size(); ++i) {
        const Rect r = strip.tabs[i];
        if (w->tabs[i].id == w->active && r.contains(client) &&
            tab_face(r, platform_.scale(window), true).contains(
                static_cast<float>(client.x - r.x), static_cast<float>(client.y - r.y)))
            return w->active;
    }
    for (std::size_t i = strip.tabs.size(); i-- > 0;) {
        const Rect r = strip.tabs[i];
        if (r.contains(client) && tab_face(r, platform_.scale(window), false).contains(
                static_cast<float>(client.x - r.x), static_cast<float>(client.y - r.y)))
            return w->tabs[i].id;
    }
    return 0;
}

bool Shell::over_strip(WindowId window, Point screen) const {
    if (!model_.window(window)) return false;
    const Point origin = platform_.client_origin(window);
    const auto strip = layout(window);
    const int magnetism = static_cast<int>(std::lround(15.0f * platform_.scale(window)));
    return screen.x >= origin.x && screen.x < origin.x + platform_.client_size(window).width &&
           screen.y >= origin.y - magnetism && screen.y < origin.y + strip.height + magnetism;
}

bool Shell::caption_hit(WindowId window, Point client) const {
    if (!model_.window(window)) return false;
    const auto strip = layout(window);
    return client.y >= 0 && client.y < strip.height &&
           client.x < strip.caption_start &&
           !strip.new_tab.contains(client) && tab_at(window, client) == 0;
}

void Shell::clear_hover(WindowId window) {
    if (hover_window_ != window) return;
    if (hover_tab_ || hover_close_ || hover_new_tab_ || hover_caption_ >= 0)
        platform_.invalidate(window);
    hover_window_ = 0;
    hover_tab_ = 0;
    hover_close_ = 0;
    hover_new_tab_ = false;
    hover_caption_ = -1;
}

void Shell::update_hover(WindowId window, Point client) {
    const auto strip = layout(window);
    const float scale = platform_.scale(window);
    const int caption_start = strip.caption_start;
    TabId tab = 0;
    TabId close = 0;
    bool new_tab = false;
    int caption = -1;
    if (client.y >= 0 && client.y < strip.height) {
        if (client.x >= caption_start) {
            for (int i = 0; i < strip.caption_button_count; ++i) {
                if (caption_button(strip, i, scale).contains(client)) caption = i;
            }
        } else if (strip.new_tab.contains(client)) {
            new_tab = true;
        } else if ((tab = tab_at(window, client))) {
            const auto* w = model_.window(window);
            for (std::size_t i = 0; i < w->tabs.size(); ++i) {
                if (w->tabs[i].id != tab) continue;
                const Rect r = strip.tabs[i];
                if (client.x >= r.right() - static_cast<int>(37 * scale) &&
                    client.x < r.right() - static_cast<int>(13 * scale)) close = tab;
                break;
            }
        }
    }
    if (hover_window_ == window && hover_tab_ == tab && hover_close_ == close &&
        hover_new_tab_ == new_tab && hover_caption_ == caption) return;
    if (hover_window_ && hover_window_ != window) clear_hover(hover_window_);
    hover_window_ = window;
    hover_tab_ = tab;
    hover_close_ = close;
    hover_new_tab_ = new_tab;
    hover_caption_ = caption;
    platform_.invalidate(window);
}

void Shell::handle_pointer(const Event& event) {
    if (event.type == EventType::PointerLeave) {
        clear_hover(event.window);
        return;
    }
    const auto strip = layout(event.window);
    if (event.type == EventType::PointerMove) update_hover(event.window, event.client);
    if (event.type == EventType::PointerDown) {
        if (event.client.y >= strip.height) {
            client_.body_event(event, {0, strip.height, event.size.width,
                                       event.size.height - strip.height});
            return;
        }
        const int caption_start = strip.caption_start;
        if (event.client.x >= caption_start) {
            int button = -1;
            for (int i = 0; i < strip.caption_button_count; ++i) {
                if (caption_button(strip, i, platform_.scale(event.window)).contains(event.client))
                    button = i;
            }
            if (button < 0) return;
            if (button < strip.extra_caption_buttons) {
                client_.extra_caption_button_pressed(event.window, button);
            } else {
                const int system_button = button - strip.extra_caption_buttons;
                if (system_button == 0) platform_.minimize(event.window);
                else if (system_button == 1) platform_.toggle_maximize(event.window);
                else close_window(event.window);
            }
            return;
        }
        if (strip.new_tab.contains(event.client)) {
            (void)new_tab(event.window);
            return;
        }
        const TabId id = tab_at(event.window, event.client);
        if (!id) return;
        const WindowTabs* w = model_.window(event.window);
        const auto it = std::find_if(w->tabs.begin(), w->tabs.end(),
                                     [id](const Tab& tab) { return tab.id == id; });
        const std::size_t index = static_cast<std::size_t>(it - w->tabs.begin());
        const Rect rect = strip.tabs[index];
        if (((id == w->active && rect.width >=
              static_cast<int>(detail::ChromeMetrics::min_active_width * platform_.scale(event.window))) ||
             rect.width >= static_cast<int>(detail::ChromeMetrics::close_hide_width *
                                            platform_.scale(event.window))) &&
            event.client.x >= rect.right() - static_cast<int>(37 * platform_.scale(event.window)) &&
            event.client.x < rect.right() - static_cast<int>(13 * platform_.scale(event.window))) {
            (void)close_tab(event.window, id);
            return;
        }
        (void)select_tab(event.window, id);
        drag_ = {DragPhase::Pressed, event.window, event.window, 0, id, event.screen,
                 event.client, event.screen, index};
        platform_.capture_pointer(event.window);
        return;
    }
    if (event.type == EventType::CaptureLost) {
        if (drag_.window == event.window && drag_.phase != DragPhase::NativeWindow)
            cancel_drag();
        return;
    }
    if (event.type == EventType::PointerUp) {
        if (drag_.phase == DragPhase::Idle && event.client.y >= strip.height) {
            client_.body_event(event, {0, strip.height, event.size.width,
                                       event.size.height - strip.height});
        }
        if (drag_.phase != DragPhase::NativeWindow) {
            drag_ = {};
            platform_.release_pointer();
        }
        return;
    }
    if (drag_.phase == DragPhase::Idle || event.window != drag_.window) {
        if (event.client.y >= strip.height) {
            client_.body_event(event, {0, strip.height, event.size.width,
                                       event.size.height - strip.height});
        }
        return;
    }
    const int dx = event.screen.x - drag_.press_screen.x;
    const int dy = event.screen.y - drag_.press_screen.y;
    const int slop = static_cast<int>(std::lround(6.0f * platform_.scale(event.window)));
    if (drag_.phase == DragPhase::Pressed && dx * dx + dy * dy > slop * slop)
        drag_.phase = DragPhase::InStrip;
    if (drag_.phase != DragPhase::InStrip) return;
    drag_.current_screen = event.screen;

    const WindowTabs* w = model_.window(event.window);
    if (!w) return;
    if (w->tabs.size() == 1 || !over_strip(event.window, event.screen)) {
        start_native_drag(event.window, event.screen);
        return;
    }
    auto it = std::find_if(w->tabs.begin(), w->tabs.end(),
                           [this](const Tab& tab) { return tab.id == drag_.tab; });
    if (it == w->tabs.end()) return;
    const std::size_t from = static_cast<std::size_t>(it - w->tabs.begin());
    const std::size_t to = Layout::insertion_index(strip, event.client.x, from);
    if (from != to) (void)move_tab(event.window, drag_.tab, to);
}

void Shell::start_native_drag(WindowId window, Point screen) {
    if (!platform_.supports_native_move_loop()) {
        drag_ = {};
        platform_.release_pointer();
        return;
    }
    const WindowTabs* w = model_.window(window);
    if (!w) return;
    if (w->tabs.size() > 1) {
        const Size size = platform_.client_size(window);
        const Rect bounds{screen.x - drag_.grab_client.x, screen.y - drag_.grab_client.y,
                          size.width, size.height};
        const WindowId torn = open_window(bounds, false, false);
        if (!torn) return;
        if (!transfer_tab(window, torn, drag_.tab, 0)) {
            request_destroy(torn);
            return;
        }
        drag_.window = torn;
        drag_.phase = DragPhase::NativeWindow;
        platform_.invalidate(torn);
        platform_.show(torn);
    }
    drag_.phase = DragPhase::NativeWindow;
    platform_.set_client_origin(drag_.window,
                                {screen.x - drag_.grab_client.x,
                                 screen.y - drag_.grab_client.y});
    platform_.release_pointer();
    // The first frame must be visible before the synchronous OS move loop.
    paint(drag_.window);
    const MoveLoopResult result = platform_.run_native_move_loop(drag_.window);
    finish_native_drag(result);
}

void Shell::handle_moving(const Event& event) {
    if (drag_.phase != DragPhase::NativeWindow || event.window != drag_.window) return;
    drag_.current_screen = event.screen;
    const WindowId target = platform_.window_at(event.screen, drag_.window);
    if (!target || !over_strip(target, event.screen)) return;
    drag_.pending_target = target;
    platform_.end_native_move_loop(drag_.window);
}

void Shell::finish_native_drag(MoveLoopResult result) {
    if (drag_.phase != DragPhase::NativeWindow) return;
    const Drag completed = drag_;
    const WindowId source = completed.window;
    const WindowId target = completed.cancel_requested ? 0 : completed.pending_target;
    bool attached = false;
    if (target && model_.window(target) && model_.window(source)) {
        const auto target_layout = layout(target);
        const Point origin = platform_.client_origin(target);
        const int x = completed.current_screen.x - origin.x;
        const std::size_t index = Layout::insertion_index(target_layout, x, target_layout.tabs.size());
        attached = transfer_tab(source, target, completed.tab, index);
    }
    if (!attached && source != completed.source_window &&
        (completed.cancel_requested || result != MoveLoopResult::Completed || target != 0) &&
        model_.window(source) && model_.window(completed.source_window)) {
        (void)transfer_tab(source, completed.source_window, completed.tab,
                           completed.original_index);
    }
    drag_ = {};
}

void Shell::cancel_drag() {
    if (drag_.phase == DragPhase::Idle) return;
    if (drag_.phase == DragPhase::NativeWindow) {
        drag_.cancel_requested = true;
        drag_.pending_target = 0;
        platform_.end_native_move_loop(drag_.window);
        return;
    }
    const Drag canceled = drag_;
    drag_ = {};
    if (canceled.phase == DragPhase::InStrip && model_.window(canceled.window))
        (void)move_tab(canceled.window, canceled.tab, canceled.original_index);
    platform_.release_pointer();
}

void Shell::paint(WindowId window) {
    const auto* w = model_.window(window);
    if (!w) return;
    const Size size = platform_.client_size(window);
    if (size.width <= 0 || size.height <= 0) return;
    const Size rendered = renderer_.info(window).surface_size;
    if (rendered.width != size.width || rendered.height != size.height)
        renderer_.resize(window, size);
    SkCanvas* canvas = renderer_.canvas(window);
    if (!canvas) return;
    const float scale = platform_.scale(window);
    const StripLayout strip = layout(window);
    const int caption_start = strip.caption_start;
    canvas->clear(theme_.body);
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(theme_.strip);
    canvas->drawRect(SkRect::MakeXYWH(0, 0, static_cast<float>(size.width),
                                     static_cast<float>(strip.height)), paint);
    canvas->save();
    canvas->clipRect(SkRect::MakeLTRB(0, 0, static_cast<float>(caption_start),
                                     static_cast<float>(strip.height)));
    client_.paint_leading(window, *canvas, strip.leading_slot);

    const auto paint_tab = [&](std::size_t i) {
        const Tab& tab = w->tabs[i];
        const Rect r = strip.tabs[i];
        const bool active = tab.id == w->active;
        paint.setColor(active ? theme_.tab_active
                              : tab.id == hover_tab_ ? theme_.tab_hover : theme_.tab_inactive);
        canvas->save();
        canvas->translate(static_cast<float>(r.x), static_cast<float>(r.y));
        canvas->drawPath(tab_face(r, scale, active), paint);
        canvas->restore();
        const int icon_size = static_cast<int>(16 * scale);
        const Rect icon{r.x + static_cast<int>(20 * scale),
                        r.y + (r.height - icon_size) / 2, icon_size, icon_size};
        client_.paint_tab_icon(window, tab.id, *canvas, icon);
        canvas->save();
        const float title_x = r.x + 43 * scale;
        canvas->clipRect(SkRect::MakeLTRB(title_x, static_cast<float>(r.y),
                                          static_cast<float>(std::max(r.x, r.right() -
                                              static_cast<int>(39 * scale))),
                                          static_cast<float>(r.bottom())));
        text(*canvas, tab.title, title_x, r.y + 23 * scale, 12 * scale,
             active ? theme_.text : theme_.text_muted);
        canvas->restore();
        if ((active && r.width >= static_cast<int>(detail::ChromeMetrics::min_active_width * scale)) ||
            r.width >= static_cast<int>(detail::ChromeMetrics::close_hide_width * scale)) {
            const float cx = r.right() - 25 * scale;
            const float cy = r.y + r.height * 0.5f;
            if (hover_window_ == window && hover_close_ == tab.id) {
                paint.setColor(theme_.tab_hover);
                canvas->drawCircle(cx, cy, 11 * scale, paint);
            }
            paint_caption_symbol(*canvas, "\xEE\xA2\xBB", cx, cy, 10 * scale,
                                 active ? theme_.text_muted : theme_.text);
        }
    };
    for (std::size_t i = 0; i < w->tabs.size(); ++i) {
        if (w->tabs[i].id != w->active) paint_tab(i);
    }
    for (std::size_t i = 0; i + 1 < w->tabs.size(); ++i) {
        if (w->tabs[i].id == w->active || w->tabs[i + 1].id == w->active) continue;
        const Rect r = strip.tabs[i];
        paint.setColor(theme_.separator);
        canvas->drawRoundRect(SkRect::MakeXYWH(
                                  r.right() - (detail::ChromeMetrics::overlap +
                                               detail::ChromeMetrics::separator_width) * scale / 2,
                                  r.y + (r.height - detail::ChromeMetrics::separator_height * scale) / 2,
                                  detail::ChromeMetrics::separator_width * scale,
                                  detail::ChromeMetrics::separator_height * scale),
                              scale, scale, paint);
    }
    for (std::size_t i = 0; i < w->tabs.size(); ++i) {
        if (w->tabs[i].id == w->active) paint_tab(i);
    }
    paint.setColor(hover_window_ == window && hover_new_tab_ ?
                   theme_.new_tab_hover : theme_.new_tab);
    canvas->drawRoundRect(skrect(strip.new_tab), 14 * scale, 14 * scale, paint);
    text(*canvas, "+", strip.new_tab.x + 7 * scale, strip.new_tab.y + 21 * scale,
         20 * scale, theme_.text);
    canvas->restore();

    constexpr const char* symbols[] = {"\xEE\xA4\xA1", "\xEE\xA4\xA2", "\xEE\xA2\xBB"};
    for (int i = 0; i < strip.caption_button_count; ++i) {
        const Rect button = caption_button(strip, i, scale);
        const int inset = i ? static_cast<int>(std::lround(
            detail::ChromeMetrics::caption_button_spacing * scale)) : 0;
        const Rect face{button.x + inset, button.y, button.width - inset, button.height};
        const bool hovered = hover_window_ == window && hover_caption_ == i;
        if (hovered) {
            paint.setColor(i == strip.extra_caption_buttons + 2 ?
                           theme_.caption_close_hover : theme_.caption_hover);
            canvas->drawRect(skrect(face), paint);
        }
        if (i < strip.extra_caption_buttons) {
            client_.paint_extra_caption_button(window, i, *canvas, face, hovered);
        } else {
            paint_caption_symbol(*canvas, symbols[i - strip.extra_caption_buttons],
                                 face.x + face.width * 0.5f,
                                 strip.height * 0.5f, 12 * scale, theme_.text);
        }
    }
    const Rect body{0, strip.height, size.width, std::max(0, size.height - strip.height)};
    canvas->save();
    canvas->clipRect(skrect(body));
    client_.paint_body(window, w->active, *canvas, body);
    canvas->restore();
    renderer_.present(window, platform_.native_handle(window));
}

} // namespace tabengine
