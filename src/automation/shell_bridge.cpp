// Shell bridge core: lifecycle, observer → event conversion, environment
// hooks, capabilities, and the Window/Tab domains. Input lives in
// shell_bridge_input.cpp, UI/semantic assembly in shell_bridge_ui.cpp.

#include "shell_bridge_impl.h"

#include <algorithm>
#include <utility>

namespace tabengine::automation {
namespace {

MethodDescriptor descriptor(std::string name, std::string summary,
                            std::vector<std::string> grants, bool mutating,
                            JsonValue params, JsonValue result) {
    MethodDescriptor value;
    value.name = std::move(name);
    value.summary = std::move(summary);
    value.grants = std::move(grants);
    value.mutating = mutating;
    value.params = std::move(params);
    value.result = std::move(result);
    return value;
}

JsonValue object_schema(std::vector<std::pair<std::string, JsonValue>> properties,
                        std::vector<std::string> required = {}) {
    JsonValue schema = JsonValue::object();
    schema.set("type", "object");
    JsonValue props = JsonValue::object();
    for (auto& [name, property] : properties) props.set(std::move(name), std::move(property));
    schema.set("properties", std::move(props));
    if (!required.empty()) {
        JsonValue list = JsonValue::array();
        for (std::string& name : required) list.push_back(JsonValue(std::move(name)));
        schema.set("required", std::move(list));
    }
    return schema;
}

JsonValue string_property(std::string description) {
    JsonValue property = JsonValue::object();
    property.set("type", "string");
    property.set("description", std::move(description));
    return property;
}

JsonValue integer_property(std::string description, std::int64_t minimum = -1,
                           std::int64_t maximum = -1) {
    JsonValue property = JsonValue::object();
    property.set("type", "integer");
    property.set("description", std::move(description));
    if (minimum >= 0) property.set("minimum", minimum);
    if (maximum >= 0) property.set("maximum", maximum);
    return property;
}

JsonValue boolean_property(std::string description) {
    JsonValue property = JsonValue::object();
    property.set("type", "boolean");
    property.set("description", std::move(description));
    return property;
}

JsonValue enum_property(std::string description, std::vector<std::string> values) {
    JsonValue property = JsonValue::object();
    property.set("type", "string");
    property.set("description", std::move(description));
    JsonValue list = JsonValue::array();
    for (std::string& value : values) list.push_back(JsonValue(std::move(value)));
    property.set("enum", std::move(list));
    return property;
}

const JsonValue kObjectSchema = [] {
    JsonValue schema = JsonValue::object();
    schema.set("type", "object");
    schema.set("properties", JsonValue::object());
    return schema;
}();

// Parses a required decimal-id string parameter. On failure sends the
// error response and returns false.
bool wire_window_id(const JsonValue& params, std::string_view key, WindowId& out,
                    CompletionSink& sink) {
    const JsonValue* value = params.find(key);
    if (!value || !value->is_string() || !id_from_wire(value->as_string(), out)) {
        Error error = Error::invalid_params(std::string("params.") + std::string(key) +
                                            ": not a decimal id string");
        sink(MethodOutcome::failure(std::move(error)));
        return false;
    }
    return true;
}

JsonValue window_summary(const Shell& shell, IPlatform& platform, WindowId window) {
    JsonValue value = JsonValue::object();
    value.set("windowId", id_to_wire(window));
    const Point origin = platform.client_origin(window);
    const Size size = platform.client_size(window);
    value.set("bounds", rect_to_wire({origin.x, origin.y, size.width, size.height}));
    value.set("scale", static_cast<double>(platform.scale(window)));
    if (const WindowTabs* tabs = shell.model().window(window)) {
        value.set("activeTabId", tabs->active ? JsonValue(id_to_wire(tabs->active))
                                              : JsonValue{});
        value.set("tabCount", static_cast<std::int64_t>(tabs->tabs.size()));
    }
    return value;
}

JsonValue tab_summary(const WindowTabs& window, const Tab& tab) {
    JsonValue value = JsonValue::object();
    value.set("tabId", id_to_wire(tab.id));
    value.set("contentId", id_to_wire(tab.content));
    value.set("title", tab.title);
    value.set("active", tab.id == window.active);
    value.set("closing", tab.closing);
    value.set("loading", tab.loading);
    value.set("attention", tab.attention);
    return value;
}

} // namespace

// ---- lifecycle ----

ShellAutomationBridge::ShellAutomationBridge(Dependencies dependencies)
    : impl_(std::make_unique<Impl>(dependencies.shell, dependencies.platform,
                                   dependencies.renderer, dependencies.runtime)) {
    impl_->register_window_tab_domains();
    impl_->register_input_domain();
    impl_->register_ui_domain();
    impl_->register_capture_and_trace_domains();
    impl_->install_environment_hooks();
    impl_->publish_capabilities();
    // Connection ends (session end, idle expiry, transport stop) release
    // the input that connection holds, on the UI thread.
    impl_->connection_observer = dependencies.runtime.add_connection_observer(
        [impl = impl_.get()](std::uint64_t connection) {
            impl->release_input_session(connection);
        });
}

ShellAutomationBridge::~ShellAutomationBridge() {
    // Registrations (domains, hooks, observer) revoke themselves here, in
    // member destruction order; the runtime must simply outlive the bridge.
    impl_.reset();
}

void ShellAutomationBridge::set_renderer_backend(std::string backend) {
    impl_->renderer_backend = std::move(backend);
    impl_->publish_capabilities();
}

// ---- observer conversions ----

void ShellAutomationBridge::Impl::bump_revisions(WindowId window, bool layout_too) {
    WindowSemantic& state = semantic[window];
    ++state.revision;
    ++state.merged_revision;
    if (layout_too) {
        ++state.layout_revision;
        ++state.merged_layout_revision;
    }
}

void ShellAutomationBridge::Impl::emit_simple(std::string_view name, WindowId window, TabId tab,
                                              ContentId content, std::uint64_t generation) {
    AutomationEvent event;
    event.name = std::string(name);
    event.window = window;
    event.tab = tab;
    event.content = content;
    event.generation = generation;
    runtime.emit_event(std::move(event));
}

void ShellAutomationBridge::Impl::window_opened(WindowId window) {
    bump_revisions(window, true);
    emit_simple(event_names::kWindowAdded, window, 0, 0);
}

void ShellAutomationBridge::Impl::window_closed(WindowId window) {
    emit_simple(event_names::kWindowRemoved, window, 0, 0);
    semantic.erase(window);
    for (auto& [connection, session] : sessions) {
        (void)connection;
        if (session.pointer_window == window) {
            session.pointer_window = 0;
            session.held_buttons = 0;
        }
        // Keys and IME follow their own focus window; dropping them on a
        // closed window means releaseAll can no longer deliver their
        // events, so the session forgets them here.
        if (session.keyboard_window == window) {
            session.keyboard_window = 0;
            session.held_keys.clear();
            session.ime_open = false;
        }
    }
}

void ShellAutomationBridge::Impl::tab_added(WindowId window, TabId tab, ContentId content) {
    bump_revisions(window, true);
    emit_simple(event_names::kTabAdded, window, tab, content);
}

void ShellAutomationBridge::Impl::tab_closed(WindowId window, TabId tab, ContentId content) {
    bump_revisions(window, false);
    emit_simple(event_names::kTabRemoved, window, tab, content);
}

void ShellAutomationBridge::Impl::tab_selected(WindowId window, TabId tab) {
    bump_revisions(window, false);
    emit_simple(event_names::kTabSelected, window, tab, 0);
}

void ShellAutomationBridge::Impl::tab_moved(WindowId window, TabId tab, std::size_t) {
    bump_revisions(window, true);
    emit_simple(event_names::kTabMoved, window, tab, 0);
}

void ShellAutomationBridge::Impl::tab_transferred(WindowId from, WindowId to, TabId tab,
                                                  std::size_t) {
    bump_revisions(from, true);
    bump_revisions(to, true);
    emit_simple(event_names::kTabTransferred, from, tab, 0);
}

void ShellAutomationBridge::Impl::tab_updated(WindowId window, TabId tab) {
    bump_revisions(window, false);
    emit_simple(event_names::kTabUpdated, window, tab, 0);
}

void ShellAutomationBridge::Impl::activation_changed(WindowId window, bool active) {
    emit_simple(active ? event_names::kWindowActivated : event_names::kWindowDeactivated,
                window, 0, 0);
}

void ShellAutomationBridge::Impl::shell_event(const Event& event) {
    switch (event.type) {
    case EventType::Resized:
    case EventType::DpiChanged:
    case EventType::PlacementChanged:
        bump_revisions(event.window, true);
        if (event.type == EventType::PlacementChanged) {
            const Point origin = platform.client_origin(event.window);
            const Size size = platform.client_size(event.window);
            AutomationEvent automation_event;
            automation_event.name = std::string(event_names::kWindowBoundsChanged);
            automation_event.window = event.window;
            JsonValue details = rect_to_wire({origin.x, origin.y, size.width, size.height});
            automation_event.details = std::move(details);
            runtime.emit_event(std::move(automation_event));
        }
        break;
    default:
        break;
    }
}

void ShellAutomationBridge::Impl::animations_settled(WindowId window) {
    emit_simple(event_names::kAnimationSettled, window, 0, 0);
}

void ShellAutomationBridge::Impl::drag_started(WindowId window, TabId tab) {
    AutomationEvent event;
    event.name = std::string(event_names::kInputDispatched);
    event.window = window;
    event.tab = tab;
    JsonValue details = JsonValue::object();
    details.set("kind", "dragStarted");
    event.details = std::move(details);
    runtime.emit_event(std::move(event));
}

void ShellAutomationBridge::Impl::drag_finished(WindowId window, TabId tab) {
    AutomationEvent event;
    event.name = std::string(event_names::kInputDispatched);
    event.window = window;
    event.tab = tab;
    JsonValue details = JsonValue::object();
    details.set("kind", "dragFinished");
    event.details = std::move(details);
    runtime.emit_event(std::move(event));
}

// ---- environment hooks and capabilities ----

void ShellAutomationBridge::Impl::install_environment_hooks() {
    RuntimeEnvironmentHooks hooks;
    hooks.animation_active = [this](WindowId window) {
        return shell.has_active_animations(window);
    };
    hooks.frame_serial = [this](WindowId window) { return shell.frame_serial(window); };
    hooks.node_exists = [this](WindowId window, std::string_view stable_key) {
        return engine_node_exists(window, stable_key);
    };
    // Owned installation: destroying this bridge revokes the hooks and
    // fails dependent waits instead of calling into freed memory.
    hooks_registration = runtime.install_environment_hooks(std::move(hooks));
}

void ShellAutomationBridge::Impl::publish_capabilities() {
    JsonValue capabilities = JsonValue::object();
    capabilities.set("renderer", renderer_backend);
    JsonValue input = JsonValue::object();
    input.set("logical", true);
    input.set("native", false);
    input.set("oleDragDrop", false);
    input.set("systemIme", false);
    capabilities.set("input", std::move(input));
    capabilities.set("nativeWindowMove", platform.supports_native_move_loop());
    capabilities.set("windowActivate", true);
    capabilities.set("windowBounds", true);
    capabilities.set("windowHide", true);
    JsonValue capture = JsonValue::object();
    capture.set("supported", renderer.supports_capture());
    capture.set("raster", renderer_backend == "raster");
    capture.set("d3d12", renderer_backend == "d3d12");
    capabilities.set("capture", std::move(capture));
    runtime.set_instance_capabilities(std::move(capabilities));
}

// ---- Window/Tab domains ----

void ShellAutomationBridge::Impl::register_window_tab_domains() {
    const std::string observe{std::string(kGrantObserve)};
    const std::string mutate{std::string(kGrantWindowTab)};

    // Window domain.
    {
        std::vector<MethodDescriptor> methods;
        methods.push_back(descriptor("Window.list", "List windows with bounds and active tabs.",
                                     {observe}, false, kObjectSchema, JsonValue::object()));
        methods.push_back(descriptor(
            "Window.create",
            "Open a new window with one initial tab. Bounds are client pixels in screen space.",
            {mutate}, true,
            object_schema({
                {"bounds",
                 object_schema({
                     {"x", integer_property("Screen x")},
                     {"y", integer_property("Screen y")},
                     {"width", integer_property("Client width", 1)},
                     {"height", integer_property("Client height", 1)},
                 })},
                {"withInitialTab", boolean_property("Create the initial tab (default true)")},
            }),
            JsonValue::object()));
        methods.push_back(descriptor(
            "Window.close",
            "Close a window. Denied closures return outcome denied; side effects follow the "
            "application's veto policy.",
            {mutate}, true,
            object_schema({{"windowId", string_property("Target window (decimal id string)")}},
                          {"windowId"}),
            JsonValue::object()));
        methods.push_back(descriptor(
            "Window.activate", "Bring a window to the foreground.",
            {mutate}, true,
            object_schema({{"windowId", string_property("Target window (decimal id string)")}},
                          {"windowId"}),
            JsonValue::object()));
        methods.push_back(descriptor(
            "Window.setBounds",
            "Move and resize a window so its client area occupies the given screen rectangle.",
            {mutate}, true,
            object_schema({
                {"windowId", string_property("Target window (decimal id string)")},
                {"bounds",
                 object_schema({
                     {"x", integer_property("Screen x")},
                     {"y", integer_property("Screen y")},
                     {"width", integer_property("Client width", 1)},
                     {"height", integer_property("Client height", 1)},
                 })},
            },
                          {"windowId", "bounds"}),
            JsonValue::object()));
        methods.push_back(descriptor(
            "Window.setPlacement",
            "Minimize, toggle maximization, or hide/show a window. Hiding keeps the "
            "instance resident: surfaces, tabs and input sessions stay alive.",
            {mutate}, true,
            object_schema({
                {"windowId", string_property("Target window (decimal id string)")},
                {"state", enum_property("Placement action",
                                        {"minimize", "toggleMaximize", "hide", "show"})},
            },
                          {"windowId", "state"}),
            JsonValue::object()));

        MethodHandler handler = [this](const RequestContext& context, CompletionSink sink) {
            if (context.method == "Window.list") {
                JsonValue list = JsonValue::array();
                for (WindowId id : shell.model().window_ids()) {
                    list.push_back(window_summary(shell, platform, id));
                }
                JsonValue result = JsonValue::object();
                result.set("windows", std::move(list));
                sink(MethodOutcome::success(std::move(result)));
                return;
            }
            if (context.method == "Window.create") {
                Rect bounds{100, 100, 1100, 720};
                if (const JsonValue* value = context.params.find("bounds");
                    value && value->is_object()) {
                    Rect parsed;
                    if (!rect_from_wire(*value, parsed) || parsed.width <= 0 ||
                        parsed.height <= 0) {
                        sink(MethodOutcome::failure(
                            Error::invalid_params("params.bounds: malformed rectangle")));
                        return;
                    }
                    bounds = parsed;
                }
                bool with_initial_tab = true;
                if (const JsonValue* value = context.params.find("withInitialTab");
                    value && value->is_boolean()) {
                    with_initial_tab = value->as_boolean();
                }
                const WindowId window = shell.open_window(bounds, with_initial_tab, true);
                if (!window) {
                    sink(MethodOutcome::failure(
                        Error::make(ErrorCode::InternalError, "the shell refused the window")));
                    return;
                }
                JsonValue result = window_summary(shell, platform, window);
                if (const WindowTabs* tabs = shell.model().window(window);
                    tabs && tabs->active) {
                    result.set("tabId", id_to_wire(tabs->active));
                }
                sink(MethodOutcome::success(std::move(result)));
                return;
            }
            WindowId window = 0;
            if (!wire_window_id(context.params, "windowId", window, sink)) return;
            if (!shell.model().window(window)) {
                sink(MethodOutcome::failure(Error::target_not_found("window " +
                                                                    id_to_wire(window))));
                return;
            }
            if (context.method == "Window.close") {
                const bool present = shell.model().window(window) != nullptr;
                shell.close_window(window);
                const bool closed = present && shell.model().window(window) == nullptr;
                JsonValue result = JsonValue::object();
                result.set("outcome", closed ? "closed" : "denied");
                result.set("windowId", id_to_wire(window));
                sink(MethodOutcome::success(std::move(result)));
                return;
            }
            if (context.method == "Window.activate") {
                platform.activate(window);
                sink(MethodOutcome::success(JsonValue::object()));
                return;
            }
            if (context.method == "Window.setBounds") {
                const JsonValue* value = context.params.find("bounds");
                Rect bounds;
                if (!value || !value->is_object() || !rect_from_wire(*value, bounds) ||
                    bounds.width <= 0 || bounds.height <= 0) {
                    sink(MethodOutcome::failure(
                        Error::invalid_params("params.bounds: malformed rectangle")));
                    return;
                }
                if (!platform.set_client_bounds(window, bounds)) {
                    sink(MethodOutcome::failure(
                        Error::unsupported("this platform cannot set window bounds")));
                    return;
                }
                sink(MethodOutcome::success(JsonValue::object()));
                return;
            }
            // Window.setPlacement
            const JsonValue* state = context.params.find("state");
            const std::string action =
                state && state->is_string() ? state->as_string() : std::string{};
            if (action == "minimize") {
                platform.minimize(window);
            } else if (action == "toggleMaximize") {
                platform.toggle_maximize(window);
            } else if (action == "hide") {
                platform.hide(window);
            } else if (action == "show") {
                platform.show(window);
            } else {
                sink(MethodOutcome::failure(
                    Error::invalid_params("params.state: unknown placement action")));
                return;
            }
            sink(MethodOutcome::success(JsonValue::object()));
        };
        owned_domains.push_back(
            runtime.register_engine_methods("Window", std::move(methods), std::move(handler)));
    }

    // Tab domain.
    {
        std::vector<MethodDescriptor> methods;
        methods.push_back(descriptor(
            "Tab.list", "List tabs of one window with content ids and titles.", {observe}, false,
            object_schema({{"windowId", string_property("Target window (decimal id string)")}},
                          {"windowId"}),
            JsonValue::object()));
        methods.push_back(descriptor(
            "Tab.create", "Create a tab in a window; content identity belongs to the app.",
            {mutate}, true,
            object_schema({{"windowId", string_property("Target window (decimal id string)")}},
                          {"windowId"}),
            JsonValue::object()));
        methods.push_back(descriptor(
            "Tab.select", "Make a tab active in its window.", {mutate}, true,
            object_schema({
                {"windowId", string_property("Target window (decimal id string)")},
                {"tabId", string_property("Target tab (decimal id string)")},
            },
                          {"windowId", "tabId"}),
            JsonValue::object()));
        methods.push_back(descriptor(
            "Tab.close",
            "Close a tab. Denied closures return outcome denied; the app policy decides.",
            {mutate}, true,
            object_schema({
                {"windowId", string_property("Target window (decimal id string)")},
                {"tabId", string_property("Target tab (decimal id string)")},
            },
                          {"windowId", "tabId"}),
            JsonValue::object()));
        methods.push_back(descriptor(
            "Tab.move", "Reorder a tab within its window.", {mutate}, true,
            object_schema({
                {"windowId", string_property("Target window (decimal id string)")},
                {"tabId", string_property("Target tab (decimal id string)")},
                {"index", integer_property("Zero-based target index", 0)},
            },
                          {"windowId", "tabId", "index"}),
            JsonValue::object()));
        methods.push_back(descriptor(
            "Tab.transfer",
            "Move a tab to another window, preserving tab and content identity. This is the "
            "command path; tear-off input is a different test surface.",
            {mutate}, true,
            object_schema({
                {"fromWindowId", string_property("Source window (decimal id string)")},
                {"toWindowId", string_property("Target window (decimal id string)")},
                {"tabId", string_property("Target tab (decimal id string)")},
                {"index", integer_property("Zero-based target index", 0)},
            },
                          {"fromWindowId", "toWindowId", "tabId", "index"}),
            JsonValue::object()));

        MethodHandler handler = [this](const RequestContext& context, CompletionSink sink) {
            if (context.method == "Tab.list") {
                WindowId window = 0;
                if (!wire_window_id(context.params, "windowId", window, sink)) return;
                const WindowTabs* tabs = shell.model().window(window);
                if (!tabs) {
                    sink(MethodOutcome::failure(Error::target_not_found("window " +
                                                                        id_to_wire(window))));
                    return;
                }
                JsonValue list = JsonValue::array();
                for (const Tab& tab : tabs->tabs) list.push_back(tab_summary(*tabs, tab));
                JsonValue result = JsonValue::object();
                result.set("tabs", std::move(list));
                result.set("activeTabId", tabs->active ? JsonValue(id_to_wire(tabs->active))
                                                       : JsonValue{});
                sink(MethodOutcome::success(std::move(result)));
                return;
            }
            if (context.method == "Tab.create") {
                WindowId window = 0;
                if (!wire_window_id(context.params, "windowId", window, sink)) return;
                if (!shell.model().window(window)) {
                    sink(MethodOutcome::failure(Error::target_not_found("window " +
                                                                        id_to_wire(window))));
                    return;
                }
                const TabId tab = shell.new_tab(window);
                if (!tab) {
                    sink(MethodOutcome::failure(
                        Error::make(ErrorCode::InternalError, "the shell refused the tab")));
                    return;
                }
                const WindowTabs* tabs = shell.model().window(window);
                const auto it = std::find_if(tabs->tabs.begin(), tabs->tabs.end(),
                                             [tab](const Tab& candidate) {
                                                 return candidate.id == tab;
                                             });
                JsonValue result = JsonValue::object();
                result.set("tabId", id_to_wire(tab));
                result.set("contentId", id_to_wire(it != tabs->tabs.end() ? it->content : 0));
                sink(MethodOutcome::success(std::move(result)));
                return;
            }
            WindowId window = 0;
            if (!wire_window_id(context.params, "windowId", window, sink)) return;
            WindowId other = 0;
            TabId tab = 0;
            if (context.method == "Tab.transfer") {
                if (!wire_window_id(context.params, "fromWindowId", window, sink) ||
                    !wire_window_id(context.params, "toWindowId", other, sink) ||
                    !wire_window_id(context.params, "tabId", tab, sink)) {
                    return;
                }
            } else {
                if (!wire_window_id(context.params, "tabId", tab, sink)) return;
            }
            if (context.method == "Tab.select") {
                const bool ok = shell.select_tab(window, tab);
                if (!ok) {
                    sink(MethodOutcome::failure(Error::target_not_found(
                        "tab " + id_to_wire(tab) + " in window " + id_to_wire(window))));
                    return;
                }
                sink(MethodOutcome::success(JsonValue::object()));
                return;
            }
            if (context.method == "Tab.close") {
                const bool closed = shell.close_tab(window, tab);
                JsonValue result = JsonValue::object();
                result.set("outcome", closed ? "closed" : "denied");
                result.set("windowId", id_to_wire(window));
                result.set("tabId", id_to_wire(tab));
                sink(MethodOutcome::success(std::move(result)));
                return;
            }
            std::int64_t index = -1;
            if (const JsonValue* value = context.params.find("index");
                value && value->is_integer() && value->as_integer() >= 0) {
                index = value->as_integer();
            } else {
                sink(MethodOutcome::failure(
                    Error::invalid_params("params.index: expected a non-negative integer")));
                return;
            }
            if (context.method == "Tab.move") {
                if (!shell.move_tab(window, tab, static_cast<std::size_t>(index))) {
                    sink(MethodOutcome::failure(Error::target_not_found(
                        "tab " + id_to_wire(tab) + " in window " + id_to_wire(window))));
                    return;
                }
                sink(MethodOutcome::success(JsonValue::object()));
                return;
            }
            // Tab.transfer
            if (window == other) {
                sink(MethodOutcome::failure(
                    Error::invalid_params("params.toWindowId: use Tab.move for reordering "
                                          "within one window")));
                return;
            }
            if (!shell.transfer_tab(window, other, tab, static_cast<std::size_t>(index))) {
                sink(MethodOutcome::failure(
                    Error::target_not_found("tab " + id_to_wire(tab) + ", window " +
                                            id_to_wire(window) + " -> " + id_to_wire(other))));
                return;
            }
            JsonValue result = JsonValue::object();
            result.set("windowId", id_to_wire(other));
            result.set("tabId", id_to_wire(tab));
            sink(MethodOutcome::success(std::move(result)));
        };
        owned_domains.push_back(
            runtime.register_engine_methods("Tab", std::move(methods), std::move(handler)));
    }
}

} // namespace tabengine::automation
