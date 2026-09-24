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

float ease_out(double elapsed, double duration) {
    const float t = static_cast<float>(std::clamp(elapsed / duration, 0.0, 1.0));
    return 1.0f - (1.0f - t) * (1.0f - t);
}

float fast_out_slow_in_tween(double elapsed, double duration) {
    const float x = static_cast<float>(std::clamp(elapsed / duration, 0.0, 1.0));
    auto bezier = [](float t, float p1, float p2) {
        const float u = 1.0f - t;
        return 3.0f * u * u * t * p1 + 3.0f * u * t * t * p2 + t * t * t;
    };
    float t = x;
    for (int i = 0; i < 8; ++i) {
        const float u = 1.0f - t;
        const float dx = 3.0f * u * u * 0.4f +
                         6.0f * u * t * (0.2f - 0.4f) +
                         3.0f * t * t * (1.0f - 0.2f);
        if (std::abs(dx) < 0.000001f) break;
        t = std::clamp(t - (bezier(t, 0.4f, 0.2f) - x) / dx, 0.0f, 1.0f);
    }
    return bezier(t, 0.0f, 1.0f);
}

int interpolate(int from, int to, float t) {
    return static_cast<int>(std::lround(from + (to - from) * t));
}

SkColor blend_color(SkColor from, SkColor to, float t) {
    const auto channel = [t](SkColor a, SkColor b, int shift) {
        return static_cast<SkColor>(interpolate((a >> shift) & 0xff,
                                                (b >> shift) & 0xff, t)) << shift;
    };
    return channel(from, to, 24) | channel(from, to, 16) |
           channel(from, to, 8) | channel(from, to, 0);
}

} // namespace

Rect Shell::AnimatedRect::at(double now) const {
    if (!running) return to;
    const float t = ease_out(now - started, detail::ChromeMetrics::bounds_duration_s);
    const int left = interpolate(from.x, to.x, t);
    const int top = interpolate(from.y, to.y, t);
    const int right = interpolate(from.right(), to.right(), t);
    const int bottom = interpolate(from.bottom(), to.bottom(), t);
    return {left, top, right - left, bottom - top};
}

void Shell::AnimatedRect::retarget(Rect target, double now) {
    if (to.x == target.x && to.y == target.y && to.width == target.width &&
        to.height == target.height) return;
    from = at(now);
    to = target;
    started = now;
    running = from.x != to.x || from.y != to.y || from.width != to.width ||
              from.height != to.height;
}

void Shell::AnimatedRect::snap(Rect value) {
    from = to = value;
    running = false;
}

bool Shell::AnimatedRect::active(double now) const {
    return running && now - started < detail::ChromeMetrics::bounds_duration_s;
}

float Shell::AnimatedFloat::at(double now) const {
    return running ? from + (to - from) *
        (fast_out_slow_in ? fast_out_slow_in_tween(now - started, duration) :
                            ease_out(now - started, duration)) : to;
}

void Shell::AnimatedFloat::retarget(float target, double now) {
    if (to == target) return;
    from = at(now);
    to = target;
    started = now;
    running = from != to;
}

bool Shell::AnimatedFloat::active(double now) const {
    return running && now - started < duration;
}

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
    for (WindowId id : model_.window_ids()) {
        sync_visuals(id, 0, false);
        platform_.invalidate(id);
    }
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
    hide_hover_card(window);
    sync_visuals(window);
    const TabId previous = w->active;
    NewTab created = client_.create_tab();
    const TabId id = model_.add_tab(window, created.content, std::move(created.title));
    if (id) {
        (void)model_.select_tab(window, id);
        client_.tab_attached(window, id, created.content);
        if (previous != id) client_.active_tab_changed(window, previous, id);
        sync_visuals(window, id);
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
    if (it == w->tabs.end() || it->closing) {
        busy_windows_.erase(window);
        return false;
    }
    const ContentId content = it->content;
    const TabId previous = w->active;
    const bool last = std::count_if(w->tabs.begin(), w->tabs.end(),
        [](const Tab& item) { return !item.closing; }) == 1;
    if (!client_.allow_close_tab(window, tab, content) ||
        (last && !client_.allow_close_window(window))) {
        busy_windows_.erase(window);
        return false;
    }
    if (last) {
        destroy_window_contents(window);
        busy_windows_.erase(window);
        return true;
    }
    if (hover_window_ == window && hover_tab_ == tab) clear_hover(window);
    const auto visual = visuals_.find(window);
    if (visual != visuals_.end() &&
        (visual->second.card.pending == tab || visual->second.card.displayed == tab))
        hide_hover_card(window);
    sync_visuals(window);
    if (drag_.window == window && drag_.tab == tab) {
        drag_ = {};
        platform_.release_pointer();
    }
    const std::size_t index = static_cast<std::size_t>(it - w->tabs.begin());
    TabId next = 0;
    for (std::size_t i = index + 1; i < w->tabs.size(); ++i) {
        if (!w->tabs[i].closing) { next = w->tabs[i].id; break; }
    }
    if (!next) {
        for (std::size_t i = index; i-- > 0;) {
            if (!w->tabs[i].closing) { next = w->tabs[i].id; break; }
        }
    }
    (void)model_.set_tab_closing(window, tab);
    if (previous == tab && next) (void)model_.select_tab(window, next);
    if (previous == tab && next) client_.active_tab_changed(window, previous, next);
    sync_visuals(window);
    platform_.invalidate(window);
    busy_windows_.erase(window);
    return true;
}

bool Shell::select_tab(WindowId window, TabId tab) {
    const WindowTabs* w = model_.window(window);
    if (!w || !busy_windows_.insert(window).second) return false;
    const auto candidate = std::find_if(w->tabs.begin(), w->tabs.end(),
        [tab](const Tab& item) { return item.id == tab; });
    if (candidate == w->tabs.end() || candidate->closing) {
        busy_windows_.erase(window);
        return false;
    }
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
    const WindowTabs* w = model_.window(window);
    if (!w || busy_windows_.contains(window)) return false;
    const auto candidate = std::find_if(w->tabs.begin(), w->tabs.end(),
        [tab](const Tab& item) { return item.id == tab; });
    if (candidate == w->tabs.end() || candidate->closing) return false;
    sync_visuals(window);
    if (!model_.move_tab(window, tab, index)) return false;
    sync_visuals(window);
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
    if (it->closing) return false;
    sync_visuals(from);
    sync_visuals(to);
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
    sync_visuals(from);
    sync_visuals(to, tab);
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
        if (!tab.closing && !client_.allow_close_tab(window, tab.id, tab.content)) {
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
    visuals_.erase(window);
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
        sync_visuals(event.window, 0, false);
        if (auto visual = visuals_.find(event.window);
            visual != visuals_.end() && visual->second.card.displayed)
            visual->second.card.bounds.snap(
                hover_card_bounds(event.window, visual->second.card.displayed));
        client_.body_geometry_changed(event.window, body_bounds(event.window),
                                      platform_.scale(event.window));
        if (model_.window(event.window)) platform_.invalidate(event.window);
        break;
    case EventType::DpiChanged:
        renderer_.resize(event.window, platform_.client_size(event.window));
        sync_visuals(event.window, 0, false);
        if (auto visual = visuals_.find(event.window);
            visual != visuals_.end() && visual->second.card.displayed)
            visual->second.card.bounds.snap(
                hover_card_bounds(event.window, visual->second.card.displayed));
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
    case EventType::AnimationFrame: advance_animations(event.window); break;
    case EventType::CloseRequested: close_window(event.window); break;
    case EventType::KeyDown: {
        if (event.key == 27 && drag_.window == event.window &&
            drag_.phase != DragPhase::Idle) {
            cancel_drag();
            break;
        }
        if (event.key == 27) {
            const auto visual = visuals_.find(event.window);
            if (visual != visuals_.end() &&
                (visual->second.card.pending || visual->second.card.showing)) {
                hide_hover_card(event.window);
                break;
            }
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
    if (!w) return Layout::tab_strip(platform_.client_size(window).width, 0,
                                     platform_.scale(window), chrome_options_);
    std::vector<bool> closing;
    closing.reserve(w->tabs.size());
    for (const Tab& tab : w->tabs) closing.push_back(tab.closing);
    return Layout::tab_strip(platform_.client_size(window).width, closing,
                             platform_.scale(window), chrome_options_);
}

Rect Shell::body_bounds(WindowId window) const {
    const Size size = platform_.client_size(window);
    const int top = layout(window).height;
    return {0, top, size.width, std::max(0, size.height - top)};
}

void Shell::sync_visuals(WindowId window, TabId newborn, bool animate) {
    const WindowTabs* w = model_.window(window);
    if (!w) return;
    const double now = platform_.monotonic_seconds();
    const StripLayout strip = layout(window);
    auto& visual = visuals_[window];
    for (auto it = visual.tabs.begin(); it != visual.tabs.end();) {
        const bool present = std::any_of(w->tabs.begin(), w->tabs.end(),
            [id = it->first](const Tab& tab) { return tab.id == id; });
        if (!present) it = visual.tabs.erase(it);
        else ++it;
    }
    for (std::size_t i = 0; i < w->tabs.size(); ++i) {
        const Tab& tab = w->tabs[i];
        const Rect target = strip.tabs[i];
        auto [it, inserted] = visual.tabs.try_emplace(tab.id);
        AnimatedRect& bounds = it->second.bounds;
        if (inserted) {
            Rect start = target;
            if (animate && tab.id == newborn && w->tabs.size() > 1) {
                const int overlap = static_cast<int>(std::lround(
                    detail::ChromeMetrics::overlap * platform_.scale(window)));
                start.width = overlap;
                if (i > 0) {
                    const TabId previous = w->tabs[i - 1].id;
                    start.x = visual_tab_bounds(window, previous, strip.tabs[i - 1]).right() -
                              overlap;
                } else if (i + 1 < w->tabs.size()) {
                    const TabId next = w->tabs[i + 1].id;
                    start.x = visual_tab_bounds(window, next, strip.tabs[i + 1]).x;
                }
            }
            bounds.snap(start);
            if (hover_window_ == window && hover_tab_ == tab.id)
                it->second.hover.retarget(1.0f, now);
        }
        if (!animate || (drag_.phase == DragPhase::InStrip &&
                         drag_.window == window && drag_.tab == tab.id)) {
            bounds.snap(target);
        } else {
            bounds.retarget(target, now);
        }
    }
    schedule_animation(window);
}

Rect Shell::visual_tab_bounds(WindowId window, TabId tab, Rect fallback) const {
    const auto visual = visuals_.find(window);
    if (visual == visuals_.end()) return fallback;
    const auto it = visual->second.tabs.find(tab);
    if (it == visual->second.tabs.end()) return fallback;
    return it->second.bounds.at(platform_.monotonic_seconds());
}

Rect Shell::visual_new_tab_bounds(WindowId window, const StripLayout& strip) const {
    const WindowTabs* w = model_.window(window);
    if (!w || w->tabs.empty()) return strip.new_tab;
    int right = 0;
    for (std::size_t i = 0; i < w->tabs.size(); ++i) {
        Rect bounds = visual_tab_bounds(window, w->tabs[i].id, strip.tabs[i]);
        if (drag_.phase == DragPhase::InStrip && drag_.window == window &&
            drag_.tab == w->tabs[i].id) {
            const Point origin = platform_.client_origin(window);
            bounds = Layout::drag_visual(strip, i,
                drag_.current_screen.x - origin.x, drag_.grab_tab_x,
                platform_.scale(window)).tab;
        }
        right = std::max(right, bounds.right());
    }
    const float scale = platform_.scale(window);
    const int radius = static_cast<int>(std::lround(detail::ChromeMetrics::bottom_radius * scale));
    const int padding = static_cast<int>(std::lround(detail::ChromeMetrics::strip_padding * scale));
    Rect button = strip.new_tab;
    button.x = std::clamp(right - radius + padding, 0,
                          std::max(0, strip.caption_start - button.width));
    return button;
}

float Shell::tab_hover_amount(WindowId window, TabId tab) const {
    const auto visual = visuals_.find(window);
    if (visual == visuals_.end()) return 0.0f;
    const auto it = visual->second.tabs.find(tab);
    return it == visual->second.tabs.end() ? 0.0f :
        it->second.hover.at(platform_.monotonic_seconds());
}

float Shell::new_tab_hover_amount(WindowId window) const {
    const auto visual = visuals_.find(window);
    return visual == visuals_.end() ? 0.0f :
        visual->second.new_tab_hover.at(platform_.monotonic_seconds());
}

Rect Shell::hover_card_bounds(WindowId window, TabId tab) const {
    const WindowTabs* w = model_.window(window);
    if (!w) return {};
    const auto it = std::find_if(w->tabs.begin(), w->tabs.end(),
        [tab](const Tab& item) { return item.id == tab && !item.closing; });
    if (it == w->tabs.end()) return {};
    const StripLayout strip = layout(window);
    const std::size_t index = static_cast<std::size_t>(it - w->tabs.begin());
    const Rect anchor = visual_tab_bounds(window, tab, strip.tabs[index]);
    const float scale = platform_.scale(window);
    const int width = std::min(platform_.client_size(window).width,
        static_cast<int>(std::lround(detail::ChromeMetrics::hover_card_width * scale)));
    const bool preview = w->active != tab;
    const int height = static_cast<int>(std::lround(
        (detail::ChromeMetrics::hover_card_footer_height +
         (preview ? detail::ChromeMetrics::hover_card_preview_height : 0)) * scale));
    const int x = std::clamp(anchor.x + anchor.width / 2 - width / 2, 0,
                             std::max(0, platform_.client_size(window).width - width));
    return {x, anchor.bottom(), width, height};
}

void Shell::hide_hover_card(WindowId window) {
    auto visual = visuals_.find(window);
    if (visual == visuals_.end()) return;
    auto& card = visual->second.card;
    card.pending = 0;
    if (!card.showing) return;
    card.showing = false;
    card.opacity.duration = detail::ChromeMetrics::hover_card_fade_out_s;
    card.opacity.fast_out_slow_in = true;
    card.opacity.retarget(0.0f, platform_.monotonic_seconds());
    schedule_animation(window);
    platform_.invalidate(window);
}

void Shell::update_hover_card(WindowId window, TabId tab, Point client) {
    auto& card = visuals_[window].card;
    const double now = platform_.monotonic_seconds();
    if (!tab) {
        if (card.showing && card.bounds.at(now).contains(client)) return;
        hide_hover_card(window);
        return;
    }
    if (card.showing) {
        if (card.displayed != tab) {
            card.displayed = tab;
            card.bounds.retarget(hover_card_bounds(window, tab), now);
            schedule_animation(window);
            platform_.invalidate(window);
        }
        return;
    }
    if (card.pending == tab) return;
    card.pending = tab;
    card.show_at = now + detail::ChromeMetrics::hover_card_delay_s;
    schedule_animation(window);
}

void Shell::paint_hover_card(WindowId window, SkCanvas& canvas) {
    const auto visual = visuals_.find(window);
    const WindowTabs* w = model_.window(window);
    if (visual == visuals_.end() || !w) return;
    const auto& card = visual->second.card;
    const double now = platform_.monotonic_seconds();
    const float opacity = card.opacity.at(now);
    if (!card.displayed || opacity <= 0.01f) return;
    const auto it = std::find_if(w->tabs.begin(), w->tabs.end(),
        [id = card.displayed](const Tab& tab) { return tab.id == id && !tab.closing; });
    if (it == w->tabs.end()) return;
    const Tab tab = *it;
    const Rect bounds = card.bounds.at(now);
    if (bounds.width <= 0 || bounds.height <= 0) return;
    const float scale = platform_.scale(window);
    const bool preview = w->active != card.displayed;
    const int preview_height = preview ? static_cast<int>(std::lround(
        detail::ChromeMetrics::hover_card_preview_height * scale)) : 0;
    canvas.save();
    canvas.saveLayerAlphaf(nullptr, opacity);
    SkPaint paint;
    paint.setAntiAlias(true);
    const SkRRect panel = SkRRect::MakeRectXY(skrect(bounds), 8 * scale, 8 * scale);
    paint.setColor(theme_.hover_card);
    canvas.drawRRect(panel, paint);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(std::max(1.0f, scale));
    paint.setColor(theme_.hover_card_border);
    canvas.drawRRect(panel, paint);
    paint.setStyle(SkPaint::kFill_Style);
    canvas.clipRRect(panel, true);
    if (preview) {
        const Rect preview_bounds{bounds.x, bounds.y, bounds.width, preview_height};
        paint.setColor(theme_.strip);
        canvas.drawRect(skrect(preview_bounds), paint);
        canvas.save();
        canvas.clipRect(skrect(preview_bounds));
        client_.paint_hover_card_preview(window, tab.id, tab.content, canvas,
                                         preview_bounds);
        canvas.restore();
        paint.setColor(theme_.hover_card_border);
        canvas.drawRect(SkRect::MakeXYWH(static_cast<float>(bounds.x),
                         static_cast<float>(bounds.y + preview_height - 1),
                         static_cast<float>(bounds.width), 1.0f), paint);
    }
    const float left = bounds.x + 12 * scale;
    const float footer = static_cast<float>(bounds.y + preview_height);
    canvas.clipRect(SkRect::MakeLTRB(left, footer,
        static_cast<float>(bounds.right() - 12 * scale),
        static_cast<float>(bounds.bottom())));
    text(canvas, tab.title, left, footer + 20 * scale, 12 * scale, theme_.text);
    const std::string subtitle = client_.hover_card_subtitle(window, tab.id, tab.content);
    if (!subtitle.empty())
        text(canvas, subtitle, left, footer + 39 * scale, 11 * scale, theme_.text_muted);
    canvas.restore();
    canvas.restore();
}

void Shell::schedule_animation(WindowId window) {
    const auto visual = visuals_.find(window);
    const WindowTabs* w = model_.window(window);
    if (visual == visuals_.end() || !w) return;
    if (std::any_of(w->tabs.begin(), w->tabs.end(),
                    [](const Tab& tab) { return tab.closing; })) {
        platform_.request_animation_frame(window);
        return;
    }
    const auto& card = visual->second.card;
    if (card.pending || card.bounds.active(platform_.monotonic_seconds()) ||
        card.opacity.active(platform_.monotonic_seconds())) {
        platform_.request_animation_frame(window);
        return;
    }
    const double now = platform_.monotonic_seconds();
    if (visual->second.new_tab_hover.active(now)) {
        platform_.request_animation_frame(window);
        return;
    }
    for (const auto& [_, tab] : visual->second.tabs) {
        if (tab.bounds.active(now) || tab.hover.active(now)) {
            platform_.request_animation_frame(window);
            return;
        }
    }
}

void Shell::finish_close_tab(WindowId window, TabId tab) {
    const WindowTabs* w = model_.window(window);
    if (!w || !busy_windows_.insert(window).second) return;
    const auto it = std::find_if(w->tabs.begin(), w->tabs.end(),
        [tab](const Tab& item) { return item.id == tab && item.closing; });
    if (it == w->tabs.end()) {
        busy_windows_.erase(window);
        return;
    }
    const ContentId content = it->content;
    (void)model_.close_tab(window, tab);
    client_.tab_detached(window, tab, content);
    client_.tab_closed(content);
    if (auto visual = visuals_.find(window); visual != visuals_.end())
        visual->second.tabs.erase(tab);
    sync_visuals(window);
    platform_.invalidate(window);
    busy_windows_.erase(window);
}

void Shell::advance_animations(WindowId window) {
    const WindowTabs* w = model_.window(window);
    if (!w) return;
    const double now = platform_.monotonic_seconds();
    std::vector<TabId> finished;
    const auto visual = visuals_.find(window);
    if (visual != visuals_.end()) {
        for (const Tab& tab : w->tabs) {
            if (!tab.closing) continue;
            const auto it = visual->second.tabs.find(tab.id);
            if (it != visual->second.tabs.end() && !it->second.bounds.active(now))
                finished.push_back(tab.id);
        }
    }
    for (TabId tab : finished) finish_close_tab(window, tab);
    if (!model_.window(window)) return;
    auto& card = visuals_[window].card;
    if (card.pending && now >= card.show_at) {
        const TabId tab = card.pending;
        card.pending = 0;
        const Rect target = hover_card_bounds(window, tab);
        if (target.width > 0) {
            if (!card.displayed || card.opacity.at(now) <= 0.0f)
                card.bounds.snap(target);
            else
                card.bounds.retarget(target, now);
            card.displayed = tab;
            card.showing = true;
            card.opacity.duration = detail::ChromeMetrics::hover_card_fade_in_s;
            card.opacity.fast_out_slow_in = true;
            card.opacity.retarget(1.0f, now);
        }
    }
    if (card.showing && card.displayed)
        card.bounds.retarget(hover_card_bounds(window, card.displayed), now);
    if (!card.showing && !card.opacity.active(now) && card.opacity.at(now) <= 0.0f)
        card.displayed = 0;
    platform_.invalidate(window);
    schedule_animation(window);
}

void Shell::settle_drag(const Drag& completed) {
    if (completed.phase != DragPhase::InStrip || !model_.window(completed.window)) return;
    const StripLayout strip = layout(completed.window);
    const WindowTabs* w = model_.window(completed.window);
    for (std::size_t i = 0; i < w->tabs.size(); ++i) {
        if (w->tabs[i].id != completed.tab) continue;
        const Point origin = platform_.client_origin(completed.window);
        const Rect from = Layout::drag_visual(strip, i,
            completed.current_screen.x - origin.x, completed.grab_tab_x,
            platform_.scale(completed.window)).tab;
        auto& bounds = visuals_[completed.window].tabs[completed.tab].bounds;
        bounds.snap(from);
        bounds.retarget(strip.tabs[i], platform_.monotonic_seconds());
        schedule_animation(completed.window);
        platform_.invalidate(completed.window);
        return;
    }
}

TabId Shell::tab_at(WindowId window, Point client) const {
    const auto* w = model_.window(window);
    if (!w) return 0;
    const auto strip = layout(window);
    for (std::size_t i = 0; i < strip.tabs.size(); ++i) {
        if (w->tabs[i].closing) continue;
        const Rect r = visual_tab_bounds(window, w->tabs[i].id, strip.tabs[i]);
        if (w->tabs[i].id == w->active && r.contains(client) &&
            tab_face(r, platform_.scale(window), true).contains(
                static_cast<float>(client.x - r.x), static_cast<float>(client.y - r.y)))
            return w->active;
    }
    for (std::size_t i = strip.tabs.size(); i-- > 0;) {
        if (w->tabs[i].closing) continue;
        const Rect r = visual_tab_bounds(window, w->tabs[i].id, strip.tabs[i]);
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
           !visual_new_tab_bounds(window, strip).contains(client) &&
           tab_at(window, client) == 0;
}

void Shell::clear_hover(WindowId window) {
    if (hover_window_ != window) return;
    hide_hover_card(window);
    const double now = platform_.monotonic_seconds();
    auto& visual = visuals_[window];
    if (auto it = visual.tabs.find(hover_tab_); it != visual.tabs.end())
        it->second.hover.retarget(0.0f, now);
    visual.new_tab_hover.retarget(0.0f, now);
    schedule_animation(window);
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
        } else if (visual_new_tab_bounds(window, strip).contains(client)) {
            new_tab = true;
        } else if ((tab = tab_at(window, client))) {
            const auto* w = model_.window(window);
            for (std::size_t i = 0; i < w->tabs.size(); ++i) {
                if (w->tabs[i].id != tab) continue;
                const Rect r = visual_tab_bounds(window, tab, strip.tabs[i]);
                if (client.x >= r.right() - static_cast<int>(37 * scale) &&
                    client.x < r.right() - static_cast<int>(13 * scale)) close = tab;
                break;
            }
        }
    }
    if (hover_window_ == window && hover_tab_ == tab && hover_close_ == close &&
        hover_new_tab_ == new_tab && hover_caption_ == caption) {
        update_hover_card(window, tab, client);
        return;
    }
    const TabId old_tab = hover_window_ == window ? hover_tab_ : 0;
    const bool old_new_tab = hover_window_ == window && hover_new_tab_;
    if (hover_window_ && hover_window_ != window) clear_hover(hover_window_);
    const double now = platform_.monotonic_seconds();
    auto& visual = visuals_[window];
    if (old_tab != tab) {
        if (auto it = visual.tabs.find(old_tab); it != visual.tabs.end())
            it->second.hover.retarget(0.0f, now);
        if (auto it = visual.tabs.find(tab); it != visual.tabs.end())
            it->second.hover.retarget(1.0f, now);
    }
    if (old_new_tab != new_tab)
        visual.new_tab_hover.retarget(new_tab ? 1.0f : 0.0f, now);
    hover_window_ = window;
    hover_tab_ = tab;
    hover_close_ = close;
    hover_new_tab_ = new_tab;
    hover_caption_ = caption;
    update_hover_card(window, tab, client);
    schedule_animation(window);
    platform_.invalidate(window);
}

void Shell::handle_pointer(const Event& event) {
    if (event.type == EventType::PointerLeave) {
        clear_hover(event.window);
        return;
    }
    const auto strip = layout(event.window);
    if (event.type == EventType::PointerMove && drag_.phase == DragPhase::Idle)
        update_hover(event.window, event.client);
    if (event.type == EventType::PointerDown) {
        const auto visual = visuals_.find(event.window);
        const bool card_hit = visual != visuals_.end() && visual->second.card.showing &&
            visual->second.card.bounds.at(platform_.monotonic_seconds()).contains(event.client);
        hide_hover_card(event.window);
        if (card_hit) return;
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
        if (visual_new_tab_bounds(event.window, strip).contains(event.client)) {
            (void)new_tab(event.window);
            return;
        }
        const TabId id = tab_at(event.window, event.client);
        if (!id) return;
        const WindowTabs* w = model_.window(event.window);
        const auto it = std::find_if(w->tabs.begin(), w->tabs.end(),
                                     [id](const Tab& tab) { return tab.id == id; });
        const std::size_t index = static_cast<std::size_t>(it - w->tabs.begin());
        const Rect rect = visual_tab_bounds(event.window, id, strip.tabs[index]);
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
                 event.client, event.screen, index, event.client.x - rect.x};
        platform_.capture_pointer(event.window);
        return;
    }
    if (event.type == EventType::CaptureLost) {
        if (drag_.window == event.window && drag_.phase != DragPhase::NativeWindow)
            cancel_drag();
        return;
    }
    if (event.type == EventType::PointerUp) {
        const bool was_dragging = drag_.phase == DragPhase::InStrip;
        const Drag completed = drag_;
        if (drag_.phase == DragPhase::Idle && event.client.y >= strip.height) {
            client_.body_event(event, {0, strip.height, event.size.width,
                                       event.size.height - strip.height});
        }
        if (drag_.phase != DragPhase::NativeWindow) {
            drag_ = {};
            platform_.release_pointer();
            if (was_dragging) settle_drag(completed);
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
    if (drag_.phase == DragPhase::Pressed && dx * dx + dy * dy > slop * slop) {
        drag_.phase = DragPhase::InStrip;
        clear_hover(event.window);
    }
    if (drag_.phase != DragPhase::InStrip) return;
    drag_.current_screen = event.screen;

    const WindowTabs* w = model_.window(event.window);
    if (!w) return;
    if (w->tabs.size() == 1 || !over_strip(event.window, event.screen)) {
        start_native_drag(event.window, event.screen);
        return;
    }
    platform_.invalidate(event.window);
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
    if (canceled.phase == DragPhase::InStrip && model_.window(canceled.window)) {
        (void)move_tab(canceled.window, canceled.tab, canceled.original_index);
        settle_drag(canceled);
    }
    platform_.release_pointer();
}

void Shell::paint(WindowId window) {
    const auto* w = model_.window(window);
    if (!w) return;
    const Size size = platform_.client_size(window);
    if (size.width <= 0 || size.height <= 0) return;
    const Size rendered = renderer_.info(window).surface_size;
    if (rendered.width != size.width || rendered.height != size.height) {
        renderer_.resize(window, size);
        sync_visuals(window, 0, false);
    }
    SkCanvas* canvas = renderer_.canvas(window);
    if (!canvas) return;
    struct PaintGuard {
        std::unordered_set<WindowId>& busy;
        WindowId window;
        bool owns;
        ~PaintGuard() { if (owns) busy.erase(window); }
    } guard{busy_windows_, window, busy_windows_.insert(window).second};
    const float scale = platform_.scale(window);
    const StripLayout strip = layout(window);
    const bool dragging = drag_.phase == DragPhase::InStrip && drag_.window == window;
    std::size_t dragged_index = strip.tabs.size();
    if (dragging) {
        for (std::size_t i = 0; i < w->tabs.size(); ++i) {
            if (w->tabs[i].id == drag_.tab) {
                dragged_index = i;
                break;
            }
        }
    }
    const Point origin = dragging ? platform_.client_origin(window) : Point{};
    const DragVisual drag_visual = dragging
        ? Layout::drag_visual(strip, dragged_index,
                              drag_.current_screen.x - origin.x, drag_.grab_tab_x, scale)
        : DragVisual{};
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
        const Rect r = dragging && i == dragged_index ? drag_visual.tab :
            visual_tab_bounds(window, tab.id, strip.tabs[i]);
        const bool active = tab.id == w->active && !tab.closing;
        paint.setColor(active ? theme_.tab_active :
            blend_color(theme_.tab_inactive, theme_.tab_hover,
                        tab_hover_amount(window, tab.id)));
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
        if (w->tabs[i].id != w->active && !w->tabs[i].closing &&
            i != dragged_index) paint_tab(i);
    }
    for (std::size_t i = 0; i + 1 < w->tabs.size(); ++i) {
        if (w->tabs[i].id == w->active || w->tabs[i + 1].id == w->active ||
            w->tabs[i].closing || w->tabs[i + 1].closing ||
            i == dragged_index || i + 1 == dragged_index) continue;
        const Rect r = visual_tab_bounds(window, w->tabs[i].id, strip.tabs[i]);
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
        if (w->tabs[i].id == w->active && i != dragged_index) paint_tab(i);
    }
    for (std::size_t i = 0; i < w->tabs.size(); ++i) {
        if (w->tabs[i].closing) paint_tab(i);
    }
    if (dragged_index < w->tabs.size()) paint_tab(dragged_index);
    const Rect new_tab = visual_new_tab_bounds(window, strip);
    paint.setColor(blend_color(theme_.new_tab, theme_.new_tab_hover,
                               new_tab_hover_amount(window)));
    canvas->drawRoundRect(skrect(new_tab), 14 * scale, 14 * scale, paint);
    text(*canvas, "+", new_tab.x + 7 * scale, new_tab.y + 21 * scale,
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
    paint_hover_card(window, *canvas);
    renderer_.present(window, platform_.native_handle(window));
}

} // namespace tabengine
