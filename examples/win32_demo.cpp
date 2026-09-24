#include "tabengine/shell.h"
#include "tabengine/text.h"
#include "tabengine/win32_platform.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

namespace {

class DemoClient final : public tabengine::IClient {
public:
    tabengine::NewTab create_tab() override {
        const auto id = next_++;
        return {id, "Document " + std::to_string(id)};
    }

    void tab_closed(tabengine::ContentId) override {}

    void paint_body(tabengine::WindowId, tabengine::TabId active, SkCanvas& canvas,
                    tabengine::Rect body) override {
        const std::string title = "Host content for tab " + std::to_string(active);
        tabengine::paint_ui_text(canvas, title, 32, static_cast<float>(body.y + 65),
                                 24, SkColorSetRGB(43, 52, 67));
        const std::string hint = "Ctrl+T: new tab | drag a tab to reorder or tear off | close with x";
        tabengine::paint_ui_text(canvas, hint, 32, static_cast<float>(body.y + 96),
                                 14, SkColorSetRGB(43, 52, 67));
    }

    void body_event(const tabengine::Event&, tabengine::Rect) override {}

private:
    std::uint64_t next_ = 1;
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto platform = tabengine::make_win32_platform();
    auto renderer = tabengine::make_skia_windows_renderer();
    DemoClient client;
    tabengine::Shell shell(*platform, *renderer, client);
    const tabengine::WindowId window = shell.open_window();
    if (!window) return 1;
    (void)shell.new_tab(window);
    (void)shell.new_tab(window);
    return platform->run();
}
