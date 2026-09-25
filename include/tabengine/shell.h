#pragma once

#include "tabengine/layout.h"
#include "tabengine/model.h"
#include "tabengine/platform.h"
#include "tabengine/render.h"
#include "tabengine/theme.h"

#include <memory>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    // Used for every native window, including windows created by tab tear-off.
    [[nodiscard]] virtual std::string window_title() { return "Tabbed Window"; }
    // Returning false leaves the model and content unchanged. A host can show
    // a confirmation UI and retry the operation after the user accepts it.
    [[nodiscard]] virtual bool allow_close_tab(WindowId, TabId, ContentId) { return true; }
    [[nodiscard]] virtual bool allow_close_window(WindowId) { return true; }
    // The content instance remains application-owned through every callback.
    // A transfer detaches and reattaches the same content without closing it.
    virtual void tab_attached(WindowId, TabId, ContentId) {}
    virtual void tab_detached(WindowId, TabId, ContentId) {}
    virtual void active_tab_changed(WindowId, TabId, TabId) {}
    virtual void body_geometry_changed(WindowId, Rect, float) {}
    virtual void window_activation_changed(WindowId, bool) {}
    virtual void dpi_changed(WindowId, float) {}
    // Client-area bounds in screen pixels, emitted after an interactive move or resize.
    virtual void window_placement_changed(WindowId, Rect, float) {}
    // Return true to consume a shortcut before TabEngine's default bindings.
    virtual bool handle_shortcut(const Event&) { return false; }
    // Caret rectangle (logical, body-relative) of the editor currently in an
    // IME composition; the shell converts it to window client pixels for the
    // platform's candidate-window placement. Empty rect = no editor (the
    // system default placement applies).
    [[nodiscard]] virtual Rect ime_caret_rect(WindowId) { return {}; }
    virtual void paint_extra_caption_button(WindowId, int, SkCanvas&, Rect, bool) {}
    virtual void extra_caption_button_pressed(WindowId, int) {}
    virtual void tab_closed(ContentId content) = 0;
    virtual void paint_body(WindowId window, TabId active, SkCanvas& canvas, Rect body) = 0;
    virtual void body_event(const Event& event, Rect body) = 0;
    virtual void paint_leading(WindowId, SkCanvas&, Rect) {}
    virtual void paint_tab_icon(WindowId, TabId, SkCanvas&, Rect) {}
    virtual std::string hover_card_subtitle(WindowId, TabId, ContentId) { return {}; }
    virtual void paint_hover_card_preview(WindowId, TabId, ContentId, SkCanvas&, Rect) {}
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
    [[nodiscard]] bool move_tab(WindowId window, TabId tab, std::size_t index);
    [[nodiscard]] bool transfer_tab(WindowId from, WindowId to, TabId tab, std::size_t index);
    [[nodiscard]] bool update_tab(WindowId window, TabId tab, std::string title,
                                  bool loading = false, bool attention = false);
    void close_window(WindowId window);
    void on_event(const Event& event);
    [[nodiscard]] const Model& model() const { return model_; }
    void set_theme(Theme theme);
    void set_chrome_options(ChromeOptions options);

private:
    enum class DragPhase { Idle, Pressed, InStrip, NativeWindow };
    struct Drag {
        DragPhase phase = DragPhase::Idle;
        WindowId window = 0;
        WindowId source_window = 0;
        WindowId pending_target = 0;
        TabId tab = 0;
        Point press_screen{};
        Point grab_client{};
        Point current_screen{};
        std::size_t original_index = 0;
        int grab_tab_x = 0;
        int last_reorder_x = 0;
        bool cancel_requested = false;
    };

    struct AnimatedRect {
        Rect from{};
        Rect to{};
        double started = 0.0;
        bool running = false;
        [[nodiscard]] Rect at(double now) const;
        void retarget(Rect target, double now);
        void snap(Rect value);
        [[nodiscard]] bool active(double now) const;
    };
    struct AnimatedFloat {
        float from = 0.0f;
        float to = 0.0f;
        double started = 0.0;
        double duration = 0.120;
        bool fast_out_slow_in = false;
        bool running = false;
        [[nodiscard]] float at(double now) const;
        void retarget(float target, double now);
        [[nodiscard]] bool active(double now) const;
    };
    struct TabVisual {
        AnimatedRect bounds;
        AnimatedFloat hover;
    };
    struct WindowVisual {
        std::unordered_map<TabId, TabVisual> tabs;
        AnimatedFloat new_tab_hover;
        struct HoverCard {
            TabId pending = 0;
            TabId displayed = 0;
            double show_at = 0.0;
            bool showing = false;
            AnimatedRect bounds;
            AnimatedFloat opacity;
        } card;
    };

    void handle_event(const Event& event);
    void handle_pointer(const Event& event);
    void handle_moving(const Event& event);
    void start_native_drag(WindowId window, Point screen);
    void finish_native_drag(MoveLoopResult result);
    void cancel_drag();
    void sync_visuals(WindowId window, TabId newborn = 0, bool animate = true);
    void settle_drag(const Drag& completed);
    void advance_animations(WindowId window);
    void finish_close_tab(WindowId window, TabId tab);
    void schedule_animation(WindowId window);
    [[nodiscard]] Rect visual_tab_bounds(WindowId window, TabId tab, Rect fallback) const;
    [[nodiscard]] Rect visual_new_tab_bounds(WindowId window, const StripLayout& strip) const;
    [[nodiscard]] float tab_hover_amount(WindowId window, TabId tab) const;
    [[nodiscard]] float new_tab_hover_amount(WindowId window) const;
    void update_hover_card(WindowId window, TabId tab, Point client);
    void hide_hover_card(WindowId window);
    [[nodiscard]] Rect hover_card_bounds(WindowId window, TabId tab) const;
    void paint_hover_card(WindowId window, SkCanvas& canvas);
    void paint(WindowId window);
    void request_destroy(WindowId window);
    void flush_destroy();
    void destroy_window_contents(WindowId window);
    [[nodiscard]] Rect body_bounds(WindowId window) const;
    [[nodiscard]] StripLayout layout(WindowId window) const;
    [[nodiscard]] TabId tab_at(WindowId window, Point client) const;
    [[nodiscard]] bool over_strip(WindowId window, Point screen) const;
    [[nodiscard]] bool caption_hit(WindowId window, Point client) const;
    void update_hover(WindowId window, Point client);
    void clear_hover(WindowId window);

    IPlatform& platform_;
    IRenderer& renderer_;
    IClient& client_;
    Model model_;
    Theme theme_;
    ChromeOptions chrome_options_;
    WindowId hover_window_ = 0;
    TabId hover_tab_ = 0;
    TabId hover_close_ = 0;
    bool hover_new_tab_ = false;
    int hover_caption_ = -1;
    Drag drag_;
    std::vector<WindowId> pending_destroy_;
    std::unordered_set<WindowId> busy_windows_;
    std::unordered_map<WindowId, WindowVisual> visuals_;
    int dispatch_depth_ = 0;
};

} // namespace tabengine
