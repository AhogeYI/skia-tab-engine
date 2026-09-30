// Internal shared state for the shell bridge translation units. Not part
// of any installed SDK surface.

#pragma once

#include "tabengine/automation/shell_bridge.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tabengine::automation {

// Shared snapshot budget for the bridge translation units.
inline constexpr std::size_t kMaxSnapshotNodes = 4096;

struct ShellAutomationBridge::Impl : ShellObserver {
    Impl(Shell& shell_value, IPlatform& platform_value, IRenderer& renderer_value,
         AutomationRuntime& runtime_value)
        : shell(shell_value), platform(platform_value), renderer(renderer_value),
          runtime(runtime_value) {
        shell.add_observer(this);
    }

    ~Impl() override { shell.remove_observer(this); }

    Shell& shell;
    IPlatform& platform;
    IRenderer& renderer;
    AutomationRuntime& runtime;
    std::string renderer_backend = "unknown";
    std::vector<MethodRegistration> owned_domains;

    // ---- semantic bookkeeping (UI thread) ----
    struct WindowSemantic {
        std::uint64_t revision = 1;
        std::uint64_t layout_revision = 1;
    };
    std::unordered_map<WindowId, WindowSemantic> semantic;

    // ---- input sessions (UI thread; keyed by transport connection) ----
    struct InputSession {
        WindowId pointer_window = 0;   // window where buttons are held (0 = none)
        unsigned held_buttons = 0;
        Point last_client{};
        bool has_last_client = false;
        std::vector<int> held_keys;    // VK codes currently held by this session
        bool ime_open = false;
    };
    std::unordered_map<std::uint64_t, InputSession> sessions;

    // ---- helpers shared across the bridge translation units ----

    [[nodiscard]] Point client_to_screen(WindowId window, Point client) const {
        const Point origin = platform.client_origin(window);
        return {origin.x + client.x, origin.y + client.y};
    }

    [[nodiscard]] static std::uint64_t engine_node_ref(unsigned kind, std::uint64_t local) {
        return (static_cast<std::uint64_t>(kind) << 56) | (local & 0x00FFFFFFFFFFFFFFull);
    }

    // Engine node kinds encoded in the top byte of a node ref.
    static constexpr unsigned kNodeWindow = 1;
    static constexpr unsigned kNodeStrip = 2;
    static constexpr unsigned kNodeTab = 3;
    static constexpr unsigned kNodeTabClose = 4;
    static constexpr unsigned kNodeNewTab = 5;
    static constexpr unsigned kNodeCaption = 6;
    static constexpr unsigned kNodeCaptionArea = 7;
    static constexpr unsigned kNodeBody = 8;
    // Application provider subtrees use slots above the engine kinds.
    static constexpr unsigned kNodeProviderBase = 9;

    // ---- ShellObserver overrides: shell activity → automation events ----
    void window_opened(WindowId window) override;
    void window_closed(WindowId window) override;
    void tab_added(WindowId window, TabId tab, ContentId content) override;
    void tab_closed(WindowId window, TabId tab, ContentId content) override;
    void tab_selected(WindowId window, TabId tab) override;
    void tab_moved(WindowId window, TabId tab, std::size_t index) override;
    void tab_transferred(WindowId from, WindowId to, TabId tab, std::size_t index) override;
    void tab_updated(WindowId window, TabId tab) override;
    void activation_changed(WindowId window, bool active) override;
    void shell_event(const Event& event) override;
    void frame_presented(WindowId window, std::uint64_t serial) override;
    void animations_settled(WindowId window) override;
    void drag_started(WindowId window, TabId tab) override;
    void drag_finished(WindowId window, TabId tab) override;

    void bump_revisions(WindowId window, bool layout_too);
    void emit_simple(std::string_view name, WindowId window, TabId tab, ContentId content,
                     std::uint64_t generation = 0);

    // ---- trace (UI thread) ----
    struct TraceSpan {
        std::string name;
        std::string kind;
        double start = 0.0;
        double end = 0.0;
        std::string request_id;
        WindowId window = 0;
        JsonValue details;
    };
    bool trace_active = false;
    std::size_t trace_max_spans = 512;
    std::size_t trace_dropped = 0;
    std::vector<TraceSpan> trace_spans;
    void record_span(std::string name, std::string kind, double start, double end,
                     std::string request_id = {}, WindowId window = 0,
                     JsonValue details = JsonValue{});

    // Records one span for the enclosing scope when a trace is active.
    struct SpanScope {
        Impl* impl;
        std::string name;
        std::string kind;
        double start;
        std::string request_id;
        WindowId window;
        SpanScope(Impl* owner, std::string span_name, std::string span_kind,
                  std::string request, WindowId target)
            : impl(owner), name(std::move(span_name)), kind(std::move(span_kind)),
              start(owner->runtime.now()), request_id(std::move(request)), window(target) {}
        ~SpanScope() {
            if (impl->trace_active) {
                impl->record_span(name, kind, start, impl->runtime.now(), request_id, window);
            }
        }
        SpanScope(const SpanScope&) = delete;
        SpanScope& operator=(const SpanScope&) = delete;
    };

    // ---- domain registration (defined per translation unit) ----
    void register_window_tab_domains();
    void register_input_domain();
    void register_ui_domain();
    void register_capture_and_trace_domains();
    void install_environment_hooks();
    void publish_capabilities();

    // UI/semantic helpers (shell_bridge_ui.cpp)
    [[nodiscard]] bool resolve_engine_node(WindowId window, unsigned kind, std::uint64_t local,
                                           SemanticNode& out) const;
    [[nodiscard]] bool engine_node_exists(WindowId window, std::string_view stable_key) const;
    [[nodiscard]] SemanticSnapshot build_snapshot(WindowId window, std::size_t max_nodes,
                                                  bool& truncated);
};

} // namespace tabengine::automation
