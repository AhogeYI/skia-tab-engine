#pragma once

#include "tabengine/types.h"

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tabengine {

// ContentId belongs to the embedding application. The engine never creates,
// navigates, serializes, or destroys application content.
struct Tab {
    TabId id = 0;
    ContentId content = 0;
    std::string title;
    bool pinned = false;
    bool loading = false;
    bool attention = false;
};

struct WindowTabs {
    WindowId id = 0;
    std::vector<Tab> tabs;
    TabId active = 0;
};

enum class ChangeKind { WindowAdded, WindowRemoved, TabAdded, TabRemoved, TabMoved, TabSelected, TabUpdated };

struct Change {
    ChangeKind kind;
    WindowId window = 0;
    WindowId other_window = 0;
    TabId tab = 0;
};

class Model {
public:
    using Observer = std::function<void(const Change&)>;

    void set_observer(Observer observer) { observer_ = std::move(observer); }
    [[nodiscard]] WindowId create_window();
    [[nodiscard]] bool remove_window(WindowId id);
    [[nodiscard]] TabId add_tab(WindowId window, ContentId content, std::string title,
                                std::size_t index = static_cast<std::size_t>(-1));
    [[nodiscard]] bool close_tab(WindowId window, TabId tab);
    [[nodiscard]] bool select_tab(WindowId window, TabId tab);
    [[nodiscard]] bool move_tab(WindowId window, TabId tab, std::size_t index);
    [[nodiscard]] bool transfer_tab(WindowId from, WindowId to, TabId tab, std::size_t index);
    [[nodiscard]] bool update_tab(WindowId window, TabId tab, std::string title, bool loading,
                                  bool attention);
    [[nodiscard]] const WindowTabs* window(WindowId id) const;
    [[nodiscard]] WindowTabs* window(WindowId id);
    [[nodiscard]] std::vector<WindowId> window_ids() const;

private:
    void notify(Change change) const;
    std::unordered_map<WindowId, WindowTabs> windows_;
    WindowId next_window_ = 1;
    TabId next_tab_ = 1;
    Observer observer_;
};

} // namespace tabengine

