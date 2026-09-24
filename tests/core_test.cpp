#include "tabengine/layout.h"
#include "tabengine/model.h"

#include <cassert>
#include <vector>

int main() {
    tabengine::Model model;
    std::vector<tabengine::Change> changes;
    model.set_observer([&](const tabengine::Change& change) { changes.push_back(change); });
    const auto a = model.create_window();
    const auto b = model.create_window();
    const auto first = model.add_tab(a, 101, "first");
    const auto second = model.add_tab(a, 102, "second");
    assert(first && second && first != second);
    assert(model.select_tab(a, second));
    assert(model.transfer_tab(a, b, second, 0));
    assert(model.window(a)->tabs.size() == 1);
    assert(model.window(a)->active == first);
    assert(model.window(b)->tabs.front().id == second);
    assert(model.window(b)->tabs.front().content == 102);
    assert(model.window(b)->active == second);
    assert(!model.transfer_tab(a, 999, first, 0));
    assert(model.window(a)->tabs.front().id == first);
    assert(model.close_tab(b, second));
    assert(model.window(b)->active == 0);
    assert(changes.back().kind == tabengine::ChangeKind::TabSelected);
    assert(changes.back().tab == 0);

    const auto layout = tabengine::Layout::tab_strip(1000, 3, 1.0f);
    assert(layout.tabs.size() == 3);
    assert(layout.tabs[1].x < layout.tabs[0].right());
    assert(tabengine::Layout::insertion_index(layout, layout.tabs[2].right(), 0) == 2);
    return 0;
}
