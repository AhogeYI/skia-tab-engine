#include "tabengine/shell.h"
#include "tabengine/win32_platform.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
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
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setColor(SkColorSetRGB(43, 52, 67));
        SkFont font;
        font.setSize(24);
        const std::string title = "Host content for tab " + std::to_string(active);
        canvas.drawSimpleText(title.data(), title.size(), SkTextEncoding::kUTF8,
                              32, static_cast<float>(body.y + 65), font, paint);
        font.setSize(14);
        const std::string hint = "Ctrl+T: new tab | drag a tab to reorder or tear off | close with x";
        canvas.drawSimpleText(hint.data(), hint.size(), SkTextEncoding::kUTF8,
                              32, static_cast<float>(body.y + 96), font, paint);
    }

    void body_event(const tabengine::Event&, tabengine::Rect) override {}

private:
    std::uint64_t next_ = 1;
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto platform = tabengine::make_win32_platform();
    auto renderer = tabengine::make_skia_raster_renderer();
    DemoClient client;
    tabengine::Shell shell(*platform, *renderer, client);
    const tabengine::WindowId window = shell.open_window();
    if (!window) return 1;
    (void)shell.new_tab(window);
    (void)shell.new_tab(window);
    return platform->run();
}

