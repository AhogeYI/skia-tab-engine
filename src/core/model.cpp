#include "tabengine/model.h"

#include <algorithm>
#include <utility>

namespace tabengine {
namespace {

auto find_tab(WindowTabs& window, TabId id) {
    return std::find_if(window.tabs.begin(), window.tabs.end(),
                        [id](const Tab& tab) { return tab.id == id; });
}

} // namespace

WindowId Model::create_window() {
    const WindowId id = next_window_++;
    windows_.emplace(id, WindowTabs{id});
    notify({ChangeKind::WindowAdded, id});
    return id;
}

bool Model::remove_window(WindowId id) {
    if (windows_.erase(id) == 0) return false;
    notify({ChangeKind::WindowRemoved, id});
    return true;
}

TabId Model::add_tab(WindowId id, ContentId content, std::string title, std::size_t index) {
    auto* w = window(id);
    if (!w) return 0;
    const TabId tab = next_tab_++;
    index = std::min(index, w->tabs.size());
    w->tabs.insert(w->tabs.begin() + static_cast<std::ptrdiff_t>(index),
                   Tab{tab, content, std::move(title)});
    if (w->active == 0) w->active = tab;
    notify({ChangeKind::TabAdded, id, 0, tab});
    return tab;
}

bool Model::close_tab(WindowId id, TabId tab) {
    auto* w = window(id);
    if (!w) return false;
    auto it = find_tab(*w, tab);
    if (it == w->tabs.end()) return false;
    const std::size_t index = static_cast<std::size_t>(it - w->tabs.begin());
    w->tabs.erase(it);
    const bool active_removed = w->active == tab;
    if (w->active == tab) {
        w->active = w->tabs.empty() ? 0 : w->tabs[std::min(index, w->tabs.size() - 1)].id;
    }
    notify({ChangeKind::TabRemoved, id, 0, tab});
    if (active_removed) notify({ChangeKind::TabSelected, id, 0, w->active});
    return true;
}

bool Model::select_tab(WindowId id, TabId tab) {
    auto* w = window(id);
    if (!w || find_tab(*w, tab) == w->tabs.end()) return false;
    if (w->active == tab) return true;
    w->active = tab;
    notify({ChangeKind::TabSelected, id, 0, tab});
    return true;
}

bool Model::move_tab(WindowId id, TabId tab, std::size_t index) {
    auto* w = window(id);
    if (!w) return false;
    auto it = find_tab(*w, tab);
    if (it == w->tabs.end()) return false;
    Tab moved = std::move(*it);
    w->tabs.erase(it);
    index = std::min(index, w->tabs.size());
    w->tabs.insert(w->tabs.begin() + static_cast<std::ptrdiff_t>(index), std::move(moved));
    notify({ChangeKind::TabMoved, id, id, tab});
    return true;
}

bool Model::transfer_tab(WindowId from, WindowId to, TabId tab, std::size_t index) {
    if (from == to) return move_tab(from, tab, index);
    auto* source = window(from);
    auto* target = window(to);
    if (!source || !target) return false;
    auto it = find_tab(*source, tab);
    if (it == source->tabs.end()) return false;
    const std::size_t old_index = static_cast<std::size_t>(it - source->tabs.begin());
    Tab moved = std::move(*it);
    source->tabs.erase(it);
    if (source->active == tab) {
        source->active = source->tabs.empty()
                             ? 0 : source->tabs[std::min(old_index, source->tabs.size() - 1)].id;
    }
    index = std::min(index, target->tabs.size());
    target->tabs.insert(target->tabs.begin() + static_cast<std::ptrdiff_t>(index), std::move(moved));
    target->active = tab;
    notify({ChangeKind::TabMoved, from, to, tab});
    return true;
}

bool Model::update_tab(WindowId id, TabId tab, std::string title, bool loading, bool attention) {
    auto* w = window(id);
    if (!w) return false;
    auto it = find_tab(*w, tab);
    if (it == w->tabs.end()) return false;
    it->title = std::move(title);
    it->loading = loading;
    it->attention = attention;
    notify({ChangeKind::TabUpdated, id, 0, tab});
    return true;
}

bool Model::set_tab_closing(WindowId id, TabId tab) {
    auto* w = window(id);
    if (!w) return false;
    auto it = find_tab(*w, tab);
    if (it == w->tabs.end() || it->closing) return false;
    it->closing = true;
    notify({ChangeKind::TabUpdated, id, 0, tab});
    return true;
}

const WindowTabs* Model::window(WindowId id) const {
    auto it = windows_.find(id);
    return it == windows_.end() ? nullptr : &it->second;
}

WindowTabs* Model::window(WindowId id) {
    auto it = windows_.find(id);
    return it == windows_.end() ? nullptr : &it->second;
}

std::vector<WindowId> Model::window_ids() const {
    std::vector<WindowId> ids;
    ids.reserve(windows_.size());
    for (const auto& [id, _] : windows_) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    return ids;
}

void Model::notify(Change change) const {
    if (observer_) observer_(change);
}

} // namespace tabengine
