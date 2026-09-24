#pragma once

#include "tabengine/layout.h"
#include "tabengine/model.h"
#include "tabengine/platform.h"
#include "tabengine/render.h"

#include <memory>
#include <string>
#include <vector>

namespace tabengine {

struct NewTab {
    ContentId content = 0;
    std::string title = "New Tab";
};

// The embedding application owns content, navigation, menus, persistence, and
// the entire body below the tab strip. It can draw into the same SkCanvas.
class IClient {
public:
    virtual ~IClient() = default;
    [[nodiscard]] virtual NewTab create_tab() = 0;
    virtual void tab_closed(ContentId content) = 0;
    virtual void paint_body(WindowId window, TabId active, SkCanvas& canvas, Rect body) = 0;
    virtual void body_event(const Event& event, Rect body) = 0;
};

class Shell {
public:
    Shell(IPlatform& platform, IRenderer& renderer, IClient& client);
    ~Shell();
    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;

    [[nodiscard]] WindowId open_window(Rect bounds = {100, 100, 1100, 720},
                                       bool with_initial_tab = true, bool visible = true);
    [[nodiscard]] TabId new_tab(WindowId window);
    [[nodiscard]] bool close_tab(WindowId window, TabId tab);
    [[nodiscard]] bool select_tab(WindowId window, TabId tab);
    [[nodiscard]] bool update_tab(WindowId window, TabId tab, std::string title,
                                  bool loading = false, bool attention = false);
    void close_window(WindowId window);
    void on_event(const Event& event);
    [[nodiscard]] const Model& model() const { return model_; }

private:
    enum class DragPhase { Idle, Pressed, InStrip, NativeWindow };
    struct Drag {
        DragPhase phase = DragPhase::Idle;
        WindowId window = 0;
        WindowId pending_target = 0;
        TabId tab = 0;
        Point press_screen{};
        Point grab_client{};
        Point current_screen{};
    };

    void handle_event(const Event& event);
    void handle_pointer(const Event& event);
    void handle_moving(const Event& event);
    void start_native_drag(WindowId window, Point screen);
    void finish_native_drag();
    void paint(WindowId window);
    void request_destroy(WindowId window);
    void flush_destroy();
    [[nodiscard]] StripLayout layout(WindowId window) const;
    [[nodiscard]] TabId tab_at(WindowId window, Point client) const;
    [[nodiscard]] bool over_strip(WindowId window, Point screen) const;
    [[nodiscard]] bool caption_hit(WindowId window, Point client) const;

    IPlatform& platform_;
    IRenderer& renderer_;
    IClient& client_;
    Model model_;
    Drag drag_;
    std::vector<WindowId> pending_destroy_;
    int dispatch_depth_ = 0;
};

} // namespace tabengine
