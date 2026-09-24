#include "tabengine/layout.h"
#include "tabengine/model.h"

#include "check.h"
#include <vector>

int main() {
    tabengine::Model model;
    std::vector<tabengine::Change> changes;
    model.set_observer([&](const tabengine::Change& change) { changes.push_back(change); });
    const auto a = model.create_window();
    const auto b = model.create_window();
    const auto first = model.add_tab(a, 101, "first");
    const auto second = model.add_tab(a, 102, "second");
    CHECK(first && second && first != second);
    CHECK(model.select_tab(a, second));
    CHECK(model.transfer_tab(a, b, second, 0));
    CHECK(model.window(a)->tabs.size() == 1);
    CHECK(model.window(a)->active == first);
    CHECK(model.window(b)->tabs.front().id == second);
    CHECK(model.window(b)->tabs.front().content == 102);
    CHECK(model.window(b)->active == second);
    CHECK(!model.transfer_tab(a, 999, first, 0));
    CHECK(model.window(a)->tabs.front().id == first);
    CHECK(model.close_tab(b, second));
    CHECK(model.window(b)->active == 0);
    CHECK(changes.back().kind == tabengine::ChangeKind::TabSelected);
    CHECK(changes.back().tab == 0);
    CHECK(model.set_tab_closing(a, first));
    CHECK(model.window(a)->tabs.front().closing);
    CHECK(!model.set_tab_closing(a, first));

    const auto layout = tabengine::Layout::tab_strip(1000, 3, 1.0f);
    CHECK(layout.tabs.size() == 3);
    CHECK(layout.height == 41);
    CHECK(layout.leading_slot.width == 36);
    CHECK(layout.caption_start == 863); // 3 × 45dp + two 1dp gaps
    CHECK(layout.tabs[0].x == 36);
    CHECK(layout.tabs[0].width == 256);
    CHECK(layout.tabs[1].x == 274); // 18dp layout overlap
    CHECK(layout.tabs[2].x == 512);
    CHECK(layout.new_tab.x == layout.tabs.back().right() - 6);
    CHECK(layout.new_tab.y == 6 && layout.new_tab.width == 28);
    const auto following = tabengine::Layout::drag_visual(layout, 0, 190, 34, 1.0f);
    CHECK(following.tab.x == 156);
    CHECK(following.tab.y == layout.tabs[0].y);
    CHECK(following.new_tab.x == layout.new_tab.x);
    const auto at_end = tabengine::Layout::drag_visual(layout, 0, 2000, 34, 1.0f);
    CHECK(at_end.tab.x == 585);
    CHECK(at_end.new_tab.x == 835);
    CHECK(at_end.new_tab.right() == layout.caption_start);
    const auto at_start = tabengine::Layout::drag_visual(layout, 2, -200, 34, 1.0f);
    CHECK(at_start.tab.x == layout.leading_slot.right());
    CHECK(at_start.new_tab.x == layout.tabs[1].right() - 6);
    CHECK(tabengine::Layout::insertion_index(layout, layout.tabs[2].right(), 0) == 2);
    const auto crowded = tabengine::Layout::tab_strip(380, 12, 1.0f);
    CHECK(crowded.tabs[0].width == 32);
    CHECK(crowded.new_tab.right() <= crowded.caption_start);
    const auto crowded_drag = tabengine::Layout::drag_visual(crowded, 0, -100, 8, 1.0f);
    CHECK(crowded_drag.tab.right() <= crowded.caption_start);
    CHECK(crowded_drag.new_tab.right() <= crowded.caption_start);
    const auto closing = tabengine::Layout::tab_strip(
        1000, std::vector<bool>{false, true, false}, 1.0f);
    CHECK(closing.tabs[0].width == 256);
    CHECK(closing.tabs[1].width == 18);
    CHECK(closing.tabs[1].x == closing.tabs[0].right() - 18);
    CHECK(closing.tabs[2].x == closing.tabs[0].right() - 18);
    CHECK(closing.new_tab.x == closing.tabs[2].right() - 6);
    const auto scaled = tabengine::Layout::tab_strip(1250, 3, 1.25f);
    CHECK(scaled.leading_slot.width == 45);
    CHECK(scaled.tabs[1].x == scaled.tabs[0].right() - 23);
    CHECK(scaled.new_tab.x == scaled.tabs.back().right() - 15 + 8);
    const auto custom = tabengine::Layout::tab_strip(1000, 3, 1.0f, {64, 1});
    CHECK(custom.leading_slot.width == 64);
    CHECK(custom.extra_caption_buttons == 1);
    CHECK(custom.caption_button_count == 4);
    CHECK(custom.caption_start == 817); // Four 45dp buttons and three 1dp gaps.
    CHECK(custom.tabs.front().x == 64);
    CHECK(custom.new_tab.x == custom.tabs.back().right() - 6);
    return 0;
}
