#include "tabengine/shell.h"
#include "tabengine/text.h"
#include "tabengine/win32_platform.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <string>
#include <unordered_map>

namespace {

using tabengine::ContentId;
using tabengine::Rect;
using tabengine::TabId;
using tabengine::WindowId;

constexpr std::uint32_t kCanvas = 0xFF182238;
constexpr std::uint32_t kSidebar = 0xFF111827;
constexpr std::uint32_t kCard = 0xFF222E49;
constexpr std::uint32_t kText = 0xFFD9E4FF;
constexpr std::uint32_t kMuted = 0xFF93A1C4;
constexpr std::uint32_t kAccent = 0xFF6FA8FF;

enum class Page { Workspace, Documents, Downloads };

const char* page_name(Page page) {
    switch (page) {
    case Page::Workspace: return "Workspace";
    case Page::Documents: return "Documents";
    case Page::Downloads: return "Downloads";
    }
    return "Workspace";
}

std::uint32_t page_color(Page page) {
    switch (page) {
    case Page::Workspace: return 0xFF69D4E5;
    case Page::Documents: return 0xFF91AFFF;
    case Page::Downloads: return 0xFF4FC08D;
    }
    return kAccent;
}

void fill(SkCanvas& canvas, float x, float y, float width, float height,
          std::uint32_t color, float radius = 0) {
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(color);
    const SkRect rect = SkRect::MakeXYWH(x, y, width, height);
    if (radius > 0) canvas.drawRRect(SkRRect::MakeRectXY(rect, radius, radius), paint);
    else canvas.drawRect(rect, paint);
}

void label(SkCanvas& canvas, std::string_view value, float x, float baseline,
           float size = 13, std::uint32_t color = kText) {
    tabengine::paint_ui_text(canvas, value, x, baseline, size, color);
}

class Workbench final : public tabengine::IClient {
public:
    void bind(tabengine::Shell& shell, tabengine::IPlatform& platform,
              tabengine::IRenderer& renderer) {
        shell_ = &shell;
        platform_ = &platform;
        renderer_ = &renderer;
    }

    void next_page(Page page) { next_page_ = page; }

    tabengine::NewTab create_tab() override {
        const ContentId content = next_content_++;
        pages_[content] = next_page_;
        const Page created = next_page_;
        next_page_ = Page::Workspace;
        return {content, page_name(created)};
    }

    void tab_closed(ContentId content) override { pages_.erase(content); }

    void paint_leading(WindowId, SkCanvas& canvas, Rect bounds) override {
        const float cx = bounds.x + bounds.width * 0.5f;
        const float cy = bounds.y + bounds.height * 0.5f;
        fill(canvas, cx - 11, cy - 11, 22, 22, 0xFF24436A, 6);
        label(canvas, "T", cx - 4.5f, cy + 5, 14, 0xFF84DCEB);
    }

    void paint_tab_icon(WindowId window, TabId tab, SkCanvas& canvas, Rect rect) override {
        const Page page = page_for(window, tab);
        fill(canvas, static_cast<float>(rect.x), static_cast<float>(rect.y),
             static_cast<float>(rect.width), static_cast<float>(rect.height),
             page_color(page), 4);
        label(canvas, page == Page::Workspace ? "W" : page == Page::Documents ? "D" : "L",
              rect.x + 3.5f, rect.y + 12.5f, 11, 0xFF0D1526);
    }

    std::string hover_card_subtitle(WindowId, TabId, ContentId content) override {
        const auto it = pages_.find(content);
        return it == pages_.end() ? "Workbench" :
            "Workbench  /  " + std::string(page_name(it->second));
    }

    void paint_hover_card_preview(WindowId, TabId, ContentId content, SkCanvas& canvas,
                                  Rect bounds) override {
        const auto it = pages_.find(content);
        const Page page = it == pages_.end() ? Page::Workspace : it->second;
        const float x = static_cast<float>(bounds.x);
        const float y = static_cast<float>(bounds.y);
        const float w = static_cast<float>(bounds.width);
        const float h = static_cast<float>(bounds.height);
        fill(canvas, x, y, w, h, 0xFF172540);
        fill(canvas, x + 12, y + 12, std::max(0.0f, w - 24), 24, 0xFF0D1526, 8);
        fill(canvas, x + 12, y + 49, w * 0.28f, std::max(0.0f, h - 61), 0xFF223657, 5);
        fill(canvas, x + w * 0.36f, y + 49, w * 0.58f,
             std::max(0.0f, h - 61), 0xFF243B5B, 5);
        fill(canvas, x + w * 0.38f, y + 61, w * 0.18f, 5, page_color(page), 2.5f);
        label(canvas, page_name(page), x + w * 0.38f, y + 87, 12, kText);
    }

    void paint_body(WindowId window, TabId active, SkCanvas& canvas, Rect body) override {
        const Page page = page_for(window, active);
        const float scale = platform_->scale(window);
        const float width = body.width / scale;
        const float height = body.height / scale;
        canvas.save();
        canvas.translate(static_cast<float>(body.x), static_cast<float>(body.y));
        canvas.scale(scale, scale);
        fill(canvas, 0, 0, width, height, kCanvas);
        paint_toolbar(canvas, page, width);
        paint_sidebar(canvas, page, height);
        paint_content(canvas, window, page, width, height);
        canvas.restore();
    }

    void body_event(const tabengine::Event& event, Rect body) override {
        if (!shell_ || event.type != tabengine::EventType::PointerDown) return;
        const float scale = platform_->scale(event.window);
        const float x = (event.client.x - body.x) / scale;
        const float y = (event.client.y - body.y) / scale;
        if (x < 220 && y >= 119 && y < 227) {
            const int row = static_cast<int>((y - 119) / 36);
            if (row >= 0 && row < 3) navigate(event.window, static_cast<Page>(row));
        }
        if (y > 113 && y < 151) {
            const float width = body.width / scale;
            if (x > width - 270 && x < width - 150) {
                (void)shell_->open_window({180, 140, 1000, 700});
            } else if (x > width - 142 && x < width - 32) {
                next_page(Page::Workspace);
                (void)shell_->new_tab(event.window);
            }
        }
    }

private:
    Page page_for(WindowId window, TabId tab) const {
        if (!shell_) return Page::Workspace;
        const auto* tabs = shell_->model().window(window);
        if (!tabs) return Page::Workspace;
        for (const auto& item : tabs->tabs) {
            if (item.id != tab) continue;
            if (auto it = pages_.find(item.content); it != pages_.end()) return it->second;
        }
        return Page::Workspace;
    }

    void navigate(WindowId window, Page page) {
        const auto* tabs = shell_->model().window(window);
        if (!tabs) return;
        for (const auto& item : tabs->tabs) {
            if (item.id != tabs->active) continue;
            pages_[item.content] = page;
            (void)shell_->update_tab(window, item.id, page_name(page));
            return;
        }
    }

    void paint_toolbar(SkCanvas& canvas, Page page, float width) {
        fill(canvas, 0, 0, width, 42, kCanvas);
        fill(canvas, 0, 41, width, 1, 0xFF263553);
        label(canvas, "<", 19, 27, 19, kMuted);
        label(canvas, ">", 51, 27, 19, kMuted);
        label(canvas, "C", 83, 27, 15, kMuted);
        fill(canvas, 113, 5, std::max(100.0f, width - 322), 32, 0xFF0D1526, 16);
        label(canvas, "Workbench  /  " + std::string(page_name(page)), 130, 26, 13);
        label(canvas, "HOST UI", width - 145, 26, 11, kMuted);
        fill(canvas, width - 46, 11, 20, 20, 0xFF49678A, 10);
        label(canvas, "T", width - 40.5f, 26, 12, 0xFFFFFFFF);
    }

    void paint_sidebar(SkCanvas& canvas, Page page, float height) {
        fill(canvas, 0, 42, 220, height - 42, kSidebar);
        label(canvas, "TABENGINE", 20, 86, 12, kAccent);
        label(canvas, "EXPLORER", 20, 111, 10, kMuted);
        constexpr std::array<const char*, 3> names{"Workspace", "Documents", "Downloads"};
        constexpr std::array<const char*, 3> marks{"W", "D", "L"};
        for (int i = 0; i < 3; ++i) {
            const float y = 119 + i * 36.0f;
            const bool selected = static_cast<int>(page) == i;
            if (selected) {
                fill(canvas, 8, y, 204, 32, 0xFF233657, 6);
                fill(canvas, 8, y + 5, 3, 22, kAccent, 1.5f);
            }
            fill(canvas, 23, y + 7, 18, 18, page_color(static_cast<Page>(i)), 4);
            label(canvas, marks[i], 28, y + 20, 10, kSidebar);
            label(canvas, names[i], 52, y + 22, 13, selected ? kText : kMuted);
        }
        label(canvas, "LIBRARY", 20, 284, 10, kMuted);
        label(canvas, "Tab model", 28, 319, 12, kMuted);
        label(canvas, "Window host", 28, 352, 12, kMuted);
        label(canvas, "Skia renderer", 28, 385, 12, kMuted);
        if (height > 500) {
            fill(canvas, 16, height - 100, 188, 76, 0xFF1D2D49, 8);
            label(canvas, "F5 WORKBENCH", 30, height - 72, 10, kAccent);
            label(canvas, "Ctrl+N  /  drag to detach", 30, height - 47, 11);
        }
    }

    void paint_content(SkCanvas& canvas, WindowId window, Page page, float width,
                       float height) {
        const float x = 246;
        const float usable = std::max(360.0f, width - x - 28);
        label(canvas, "WINDOWS / SKIA", x, 88, 11, kAccent);
        label(canvas, page_name(page), x, 128, 28);
        label(canvas, "A native tabbed shell with application-owned content.", x, 154, 13, kMuted);
        fill(canvas, width - 270, 113, 120, 34, 0xFF273854, 17);
        label(canvas, "+  NEW WINDOW", width - 255, 135, 11);
        fill(canvas, width - 142, 113, 110, 34, 0xFF2B4167, 17);
        label(canvas, "+  NEW TAB", width - 125, 135, 11);
        fill(canvas, x, 178, usable, 1, 0xFF2C3B59);
        label(canvas, "QUICK ACCESS", x, 211, 11, kMuted);

        constexpr std::array<const char*, 6> names{
            "Projects", "Documents", "Downloads", "Design assets", "Examples", "Exports"};
        constexpr std::array<const char*, 6> descriptions{
            "12 recent items", "Reference files", "Transfer queue",
            "Icons and colors", "Integration hosts", "Ready to share"};
        constexpr std::array<std::uint32_t, 6> colors{
            0xFF5AAAE8, 0xFF91AFFF, 0xFF4FC08D,
            0xFFA78BFA, 0xFFE5B567, 0xFF57D7E8};
        const float gap = 12;
        const float card_width = (usable - gap) / 2;
        for (int i = 0; i < 6; ++i) {
            const float cx = x + (i % 2) * (card_width + gap);
            const float cy = 226 + (i / 2) * 84.0f;
            fill(canvas, cx, cy, card_width, 72, kCard, 10);
            fill(canvas, cx + 16, cy + 17, 38, 38, colors[i], 8);
            label(canvas, std::string_view(names[i]).substr(0, 1), cx + 28, cy + 43,
                  19, 0xFF142037);
            label(canvas, names[i], cx + 68, cy + 32, 14);
            label(canvas, descriptions[i], cx + 68, cy + 52, 11, kMuted);
        }
        if (height > 580) {
            label(canvas, "ENGINE STATUS", x, 523, 11, kMuted);
            fill(canvas, x, 541, usable, 72, 0xFF1D2B45, 10);
            fill(canvas, x + 18, 563, 8, 8, 0xFF4FC08D, 4);
            const tabengine::RenderInfo info = renderer_->info(window);
            const std::string backend = info.backend == tabengine::RenderBackend::D3D12 ?
                                        "D3D12" : "Raster";
            label(canvas, "Skia " + backend, x + 36, 570, 13);
            label(canvas, "Native Win32 frame  |  Surface " +
                  std::to_string(info.surface_size.width) + " x " +
                  std::to_string(info.surface_size.height), x + 18, 594, 11, kMuted);
        }
    }

    tabengine::Shell* shell_ = nullptr;
    tabengine::IPlatform* platform_ = nullptr;
    tabengine::IRenderer* renderer_ = nullptr;
    std::unordered_map<ContentId, Page> pages_;
    ContentId next_content_ = 1;
    Page next_page_ = Page::Workspace;
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR command_line, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto platform = tabengine::make_win32_platform();
    const bool force_raster = command_line && std::wcsstr(command_line, L"--raster");
    auto renderer = force_raster ? tabengine::make_skia_raster_renderer()
                                 : tabengine::make_skia_windows_renderer();
    Workbench client;
    tabengine::Shell shell(*platform, *renderer, client);
    client.bind(shell, *platform, *renderer);
    const WindowId window = shell.open_window({100, 100, 1280, 800}, true, false);
    if (!window) return 1;
    client.next_page(Page::Documents);
    (void)shell.new_tab(window);
    client.next_page(Page::Downloads);
    (void)shell.new_tab(window);
    platform->show(window);
    return platform->run();
}
