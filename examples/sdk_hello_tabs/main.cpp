#include "tabengine/shell.h"
#include "tabengine/text.h"
#include "tabengine/win32_platform.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"

#include <windows.h>

#include <cstdint>
#include <cwchar>
#include <string>
#include <unordered_map>

namespace {

using tabengine::ContentId;
using tabengine::Rect;
using tabengine::TabId;
using tabengine::WindowId;

Rect new_page_button(Rect body, float scale) {
    return {body.x + static_cast<int>(24 * scale),
            body.y + static_cast<int>(100 * scale),
            static_cast<int>(150 * scale), static_cast<int>(40 * scale)};
}

class HelloClient final : public tabengine::IClient {
public:
    void bind(tabengine::Shell& shell, tabengine::IPlatform& platform) {
        shell_ = &shell;
        platform_ = &platform;
    }

    [[nodiscard]] tabengine::NewTab create_tab() override {
        const ContentId id = next_content_++;
        const std::string title = "Page " + std::to_string(id);
        pages_.emplace(id, title);
        return {id, title};
    }

    [[nodiscard]] std::string window_title() override { return "TabEngine Hello Tabs"; }

    void tab_closed(ContentId content) override { pages_.erase(content); }

    void paint_leading(WindowId, SkCanvas& canvas, Rect bounds) override {
        tabengine::paint_ui_text(canvas, "H", static_cast<float>(bounds.x + 12),
                                 static_cast<float>(bounds.y + bounds.height - 12),
                                 16.0f, 0xFFFFFFFF);
    }

    void paint_body(WindowId window, TabId active, SkCanvas& canvas, Rect body) override {
        SkPaint paint;
        paint.setColor(0xFFF5F7FB);
        canvas.drawRect(SkRect::MakeXYWH(static_cast<float>(body.x),
                                         static_cast<float>(body.y),
                                         static_cast<float>(body.width),
                                         static_cast<float>(body.height)), paint);

        const float scale = platform_->scale(window);
        const std::string title = title_for(window, active);
        tabengine::paint_ui_text(canvas, title,
                                 static_cast<float>(body.x) + 24 * scale,
                                 static_cast<float>(body.y) + 54 * scale,
                                 25 * scale, 0xFF162238);
        tabengine::paint_ui_text(canvas, "The application owns this content.",
                                 static_cast<float>(body.x) + 24 * scale,
                                 static_cast<float>(body.y) + 79 * scale,
                                 13 * scale, 0xFF4A5970);

        const Rect button = new_page_button(body, scale);
        paint.setColor(0xFF2364B5);
        canvas.drawRoundRect(SkRect::MakeXYWH(static_cast<float>(button.x),
                                              static_cast<float>(button.y),
                                              static_cast<float>(button.width),
                                              static_cast<float>(button.height)),
                             8 * scale, 8 * scale, paint);
        tabengine::paint_ui_text(canvas, "New page", button.x + 24 * scale,
                                 button.y + 26 * scale, 14 * scale, 0xFFFFFFFF);
    }

    void body_event(const tabengine::Event& event, Rect body) override {
        if (event.type == tabengine::EventType::PointerDown &&
            event.button == tabengine::PointerButton::Left &&
            new_page_button(body, platform_->scale(event.window)).contains(event.client)) {
            (void)shell_->new_tab(event.window);
        }
    }

    [[nodiscard]] bool empty() const { return pages_.empty(); }

private:
    std::string title_for(WindowId window, TabId active) const {
        const auto* tabs = shell_->model().window(window);
        if (!tabs) return "No page";
        for (const auto& tab : tabs->tabs) {
            if (tab.id != active) continue;
            const auto it = pages_.find(tab.content);
            return it == pages_.end() ? "Unknown page" : it->second;
        }
        return "No page";
    }

    tabengine::Shell* shell_ = nullptr;
    tabengine::IPlatform* platform_ = nullptr;
    ContentId next_content_ = 1;
    std::unordered_map<ContentId, std::string> pages_;
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR command_line, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool smoke = command_line && std::wcsstr(command_line, L"--smoke");
    const bool raster = smoke || (command_line && std::wcsstr(command_line, L"--raster"));
    auto platform = tabengine::make_win32_platform();
    auto renderer = raster ? tabengine::make_skia_raster_renderer()
                           : tabengine::make_skia_windows_renderer();
    HelloClient client;
    tabengine::Shell shell(*platform, *renderer, client);
    client.bind(shell, *platform);

    const WindowId window = shell.open_window({100, 100, 850, 550}, true, false);
    if (!window) return 1;
    if (smoke) {
        if (!shell.new_tab(window)) return 2;
        shell.close_window(window);
        return client.empty() ? 0 : 3;
    }
    platform->show(window);
    return platform->run();
}
