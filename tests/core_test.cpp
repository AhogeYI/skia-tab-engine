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
    assert(layout.height == 41);
    assert(layout.leading_slot.width == 36);
    assert(layout.caption_start == 863); // 3 × 45dp + two 1dp gaps
    assert(layout.tabs[0].x == 36);
    assert(layout.tabs[0].width == 256);
    assert(layout.tabs[1].x == 274); // 18dp layout overlap
    assert(layout.tabs[2].x == 512);
    assert(layout.new_tab.x == layout.tabs.back().right() - 6);
    assert(layout.new_tab.y == 6 && layout.new_tab.width == 28);
    assert(tabengine::Layout::insertion_index(layout, layout.tabs[2].right(), 0) == 2);
    const auto crowded = tabengine::Layout::tab_strip(380, 12, 1.0f);
    assert(crowded.tabs[0].width == 32);
    assert(crowded.new_tab.right() <= crowded.caption_start);
    const auto scaled = tabengine::Layout::tab_strip(1250, 3, 1.25f);
    assert(scaled.leading_slot.width == 45);
    assert(scaled.tabs[1].x == scaled.tabs[0].right() - 23);
    assert(scaled.new_tab.x == scaled.tabs.back().right() - 15 + 8);
    return 0;
}
