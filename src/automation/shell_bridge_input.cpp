// Shell bridge Input domain: logical input synthesis through the same
// Shell dispatch path the platform uses, per-connection input sessions
// with conflict detection, and the stepwise tear-off drag driver.

#include "shell_bridge_impl.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace tabengine::automation {
namespace {

// Mirrors the engine's private drag_start_slop metric; the first synthetic
// move must exceed it so the Shell's drag state machine arms reliably.
constexpr int kDragSlopPx = 4;

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

MethodDescriptor descriptor(std::string name, std::string summary, bool mutating,
                            JsonValue params) {
    MethodDescriptor value;
    value.name = std::move(name);
    value.summary = std::move(summary);
    value.grants = {std::string(kGrantInput)};
    value.mutating = mutating;
    value.params = std::move(params);
    value.result = [] {
        JsonValue schema = JsonValue::object();
        schema.set("type", "object");
        schema.set("properties", JsonValue::object());
        return schema;
    }();
    return value;
}

const JsonValue kPointSchema = object_schema({
    {"x", integer_property("Client x in pixels")},
    {"y", integer_property("Client y in pixels")},
});

bool wire_window_id(const JsonValue& params, std::string_view key, WindowId& out,
                    CompletionSink& sink) {
    const JsonValue* value = params.find(key);
    if (!value || !value->is_string() || !id_from_wire(value->as_string(), out)) {
        sink(MethodOutcome::failure(Error::invalid_params(std::string("params.") +
                                                          std::string(key) +
                                                          ": not a decimal id string")));
        return false;
    }
    return true;
}

bool wire_point(const JsonValue& params, std::string_view key, Point& out, CompletionSink& sink) {
    const JsonValue* value = params.find(key);
    const auto read_int = [&value](std::string_view name, int& target) {
        if (!value) return false;
        const JsonValue* member = value->find(name);
        if (!member || !member->is_integer() ||
            member->as_integer() < std::numeric_limits<int>::min() ||
            member->as_integer() > std::numeric_limits<int>::max()) {
            return false;
        }
        target = static_cast<int>(member->as_integer());
        return true;
    };
    int x = 0;
    int y = 0;
    if (!value || !value->is_object() || !read_int("x", x) || !read_int("y", y)) {
        sink(MethodOutcome::failure(
            Error::invalid_params(std::string("params.") + std::string(key) +
                                  ": expected {x, y} integers")));
        return false;
    }
    out = {x, y};
    return true;
}

bool wire_xy(const JsonValue& params, Point& out, CompletionSink& sink) {
    const JsonValue* x = params.find("x");
    const JsonValue* y = params.find("y");
    if (!x || !x->is_integer() || !y || !y->is_integer()) {
        sink(MethodOutcome::failure(
            Error::invalid_params("params.x/params.y: expected integers")));
        return false;
    }
    out = {static_cast<int>(x->as_integer()), static_cast<int>(y->as_integer())};
    return true;
}

bool wire_button(const JsonValue& params, PointerButton& out, CompletionSink& sink) {
    out = PointerButton::Left;
    const JsonValue* value = params.find("button");
    if (!value || !value->is_string()) return true;   // default left
    const std::string& name = value->as_string();
    if (name == "left") { out = PointerButton::Left; return true; }
    if (name == "right") { out = PointerButton::Right; return true; }
    if (name == "middle") { out = PointerButton::Middle; return true; }
    sink(MethodOutcome::failure(
        Error::invalid_params("params.button: expected left, right or middle")));
    return false;
}

// Logical key name -> Windows virtual-key code. The protocol speaks logical
// names; Windows VK values stay inside the bridge.
bool logical_key_to_vk(std::string_view name, int& vk) {
    static const std::pair<std::string_view, int> kNamed[] = {
        {"backspace", VK_BACK},     {"tab", VK_TAB},         {"enter", VK_RETURN},
        {"return", VK_RETURN},      {"pause", VK_PAUSE},     {"capslock", VK_CAPITAL},
        {"escape", VK_ESCAPE},      {"esc", VK_ESCAPE},      {"space", VK_SPACE},
        {"pageup", VK_PRIOR},       {"pagedown", VK_NEXT},   {"end", VK_END},
        {"home", VK_HOME},          {"arrowleft", VK_LEFT},  {"arrowup", VK_UP},
        {"arrowright", VK_RIGHT},   {"arrowdown", VK_DOWN},  {"left", VK_LEFT},
        {"up", VK_UP},              {"right", VK_RIGHT},     {"down", VK_DOWN},
        {"insert", VK_INSERT},      {"delete", VK_DELETE},   {"del", VK_DELETE},
        {"f1", VK_F1},   {"f2", VK_F2},   {"f3", VK_F3},   {"f4", VK_F4},
        {"f5", VK_F5},   {"f6", VK_F6},   {"f7", VK_F7},   {"f8", VK_F8},
        {"f9", VK_F9},   {"f10", VK_F10}, {"f11", VK_F11}, {"f12", VK_F12},
        {"multiply", VK_MULTIPLY},  {"add", VK_ADD},         {"separator", VK_SEPARATOR},
        {"subtract", VK_SUBTRACT},  {"decimal", VK_DECIMAL}, {"divide", VK_DIVIDE},
        {";", VK_OEM_1},            {"=", VK_OEM_PLUS},      {",", VK_OEM_COMMA},
        {"-", VK_OEM_MINUS},        {".", VK_OEM_PERIOD},    {"/", VK_OEM_2},
        {"`", VK_OEM_3},            {"[", VK_OEM_4},         {"\\", VK_OEM_5},
        {"]", VK_OEM_6},            {"'", VK_OEM_7},
    };
    for (const auto& [key, code] : kNamed) {
        if (name == key) {
            vk = code;
            return true;
        }
    }
    if (name.size() == 1) {
        const char c = name[0];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            vk = static_cast<int>(static_cast<unsigned char>(
                c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c));
            return true;
        }
    }
    return false;
}

unsigned button_bit(PointerButton button) {
    switch (button) {
    case PointerButton::Left: return kPointerButtonLeft;
    case PointerButton::Right: return kPointerButtonRight;
    case PointerButton::Middle: return kPointerButtonMiddle;
    default: return 0;
    }
}

// Strict UTF-8 decode for Input.typeText; rejects surrogates and overlongs.
bool decode_utf8(std::string_view text, std::vector<char32_t>& out) {
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        std::uint32_t code_point = 0;
        if (lead < 0x80) {
            out.push_back(static_cast<char32_t>(lead));
            ++i;
            continue;
        }
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            code_point = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            code_point = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            code_point = lead & 0x07u;
        } else {
            return false;
        }
        if (i + length > text.size()) return false;
        for (std::size_t k = 1; k < length; ++k) {
            const unsigned char continuation = static_cast<unsigned char>(text[i + k]);
            if ((continuation & 0xC0) != 0x80) return false;
            code_point = (code_point << 6) | (continuation & 0x3Fu);
        }
        if ((length == 2 && code_point < 0x80) || (length == 3 && code_point < 0x800) ||
            (length == 4 && code_point < 0x10000))
            return false;
        if (code_point >= 0xD800 && code_point <= 0xDFFF) return false;
        if (code_point > 0x10FFFF) return false;
        out.push_back(static_cast<char32_t>(code_point));
        i += length;
    }
    return true;
}

} // namespace

void ShellAutomationBridge::Impl::release_session_input(InputSession& session) {
    if (session.pointer_window && session.held_buttons != 0 &&
        shell.model().window(session.pointer_window)) {
        // Every held button gets its own up at the last position; a window
        // that died meanwhile simply forgets the hold.
        for (const unsigned bit :
             {kPointerButtonLeft, kPointerButtonRight, kPointerButtonMiddle}) {
            if (!(session.held_buttons & bit)) continue;
            Event up;
            up.type = EventType::PointerUp;
            up.window = session.pointer_window;
            up.size = platform.client_size(session.pointer_window);
            up.client = session.last_client;
            up.screen = client_to_screen(session.pointer_window, session.last_client);
            up.button = bit == kPointerButtonLeft ? PointerButton::Left
                        : bit == kPointerButtonRight ? PointerButton::Right
                                                     : PointerButton::Middle;
            shell.on_event(up);
        }
    }
    if (session.keyboard_window && shell.model().window(session.keyboard_window)) {
        for (const int vk : session.held_keys) {
            Event up;
            up.type = EventType::KeyUp;
            up.key = vk;
            up.window = session.keyboard_window;
            up.size = platform.client_size(session.keyboard_window);
            shell.on_event(up);
        }
        if (session.ime_open) {
            Event cancel;
            cancel.type = EventType::ImeCancel;
            cancel.window = session.keyboard_window;
            cancel.size = platform.client_size(session.keyboard_window);
            shell.on_event(cancel);
        }
    }
    session = InputSession{};
}

void ShellAutomationBridge::Impl::release_input_session(std::uint64_t connection) {
    const auto it = sessions.find(connection);
    if (it == sessions.end()) return;
    InputSession session = std::move(it->second);
    sessions.erase(it);
    release_session_input(session);
}

void ShellAutomationBridge::Impl::register_input_domain() {
    const std::string input{std::string(kGrantInput)};

    std::vector<MethodDescriptor> methods;
    methods.push_back(descriptor("Input.click", "Synthesize a full click at a client position, or at a semantic node resolved through its authoritative owner (node ref or stableKey). Node clicks land inside the visible clip. With snapshotId and strict the click fails when the merged layout revision (shell plus providers) changed since the snapshot - for both node and stableKey mode. count=2 emits a double click whose second press carries clickCount 2 (releases carry none, matching native).", true,
        object_schema({
            {"windowId", string_property("Target window (decimal id string)")},
            {"x", integer_property("Client x in pixels")},
            {"y", integer_property("Client y in pixels")},
            {"node", string_property("Node ref from UI.snapshot (decimal string)")},
            {"stableKey", string_property("Business identity instead of x/y or node")},
            {"snapshotId", integer_property("Snapshot layoutRevision for strict clicks", 0)},
            {"strict", boolean_property("Fail when the merged layoutRevision moved (default false)")},
            {"button", enum_property("Pointer button", {"left", "right", "middle"})},
            {"count", integer_property("Click count (1 or 2)", 1, 2)},
        },
                      {"windowId"})));
    methods.push_back(descriptor("Input.pointer", "Synthesize one raw pointer event. Moves outside the client area are accepted while " "this session holds the pointer (drag capture semantics).", true,
        object_schema({
            {"windowId", string_property("Target window (decimal id string)")},
            {"kind", enum_property("Event kind", {"down", "move", "up"})},
            {"x", integer_property("Client x in pixels (may be off-window during capture)")},
            {"y", integer_property("Client y in pixels (may be off-window during capture)")},
            {"button", enum_property("Button for down/up", {"left", "right", "middle"})},
        },
                      {"windowId", "kind", "x", "y"})));
    methods.push_back(descriptor("Input.drag", "Drag by pointer synthesis from one client point to another. Crossing the strip edge " "tears the tab into a stepwise move session and can attach it over another window's " "strip; every step runs the production drag state machine.", true,
        object_schema({
            {"windowId", string_property("Source window (decimal id string)")},
            {"from", kPointSchema},
            {"to", kPointSchema},
            {"button", enum_property("Pointer button", {"left", "middle"})},
            {"steps", integer_property("Interpolation steps (default 14)", 2, 64)},
        },
                      {"windowId", "from", "to"})));
    methods.push_back(descriptor("Input.key", "Synthesize a key event with logical key names. press = down + up. Unknown key names " "report InvalidArgument instead of dropping modifiers.", true,
        object_schema({
            {"windowId", string_property("Target window (decimal id string)")},
            {"key", string_property("Logical key name (e.g. Enter, Escape, a, F4)")},
            {"kind", enum_property("Event kind", {"press", "down", "up"})},
            {"ctrl", boolean_property("Ctrl modifier")},
            {"shift", boolean_property("Shift modifier")},
            {"alt", boolean_property("Alt modifier")},
        },
                      {"windowId", "key", "kind"})));
    methods.push_back(descriptor("Input.typeText", "Deliver text as one TextInput event per code point. Text never passes the shortcut " "hook; chord generation is Input.key's job.", true,
        object_schema({
            {"windowId", string_property("Target window (decimal id string)")},
            {"text", string_property("UTF-8 text to deliver")},
        },
                      {"windowId", "text"})));
    methods.push_back(descriptor("Input.wheel", "Synthesize a wheel event with vertical and horizontal components in native units.", true,
        object_schema({
            {"windowId", string_property("Target window (decimal id string)")},
            {"x", integer_property("Client x in pixels")},
            {"y", integer_property("Client y in pixels")},
            {"deltaY", integer_property("Vertical delta (+120 per notch)")},
            {"deltaX", integer_property("Horizontal delta (+120 per notch)")},
        },
                      {"windowId", "x", "y", "deltaY"})));
    methods.push_back(descriptor("Input.ime", "Drive an IME composition session: start, update (preedit), commit, or cancel.", true,
        object_schema({
            {"windowId", string_property("Target window (decimal id string)")},
            {"kind", enum_property("Composition action", {"start", "update", "commit", "cancel"})},
            {"text", string_property("Preedit or committed string")},
        },
                      {"windowId", "kind"})));
    methods.push_back(descriptor(
        "Input.releaseAll",
        "Release everything this session holds: buttons (as a pointer up at the last "
        "position), keys, and any open IME composition.",
        true, kObjectSchema));

    MethodHandler handler = [this](const RequestContext& context, CompletionSink sink) {
        InputSession& session = sessions[context.connection];

        if (context.method == "Input.releaseAll") {
            release_session_input(session);
            sink(MethodOutcome::success(JsonValue::object()));
            return;
        }

        WindowId window = 0;
        if (!wire_window_id(context.params, "windowId", window, sink)) return;
        if (!shell.model().window(window)) {
            sink(MethodOutcome::failure(Error::target_not_found("window " + id_to_wire(window))));
            return;
        }
        const SpanScope span_scope(this, context.method, "input", context.request_id, window);

        if (context.method == "Input.click") {
            PointerButton button;
            if (!wire_button(context.params, button, sink)) return;
            std::int64_t count = 1;
            if (const JsonValue* value = context.params.find("count");
                value && value->is_integer()) {
                if (value->as_integer() < 1 || value->as_integer() > 2) {
                    sink(MethodOutcome::failure(
                        Error::invalid_params("params.count: supported values are 1 and 2")));
                    return;
                }
                count = value->as_integer();
            }
            if (session.pointer_window != 0 && session.pointer_window != window) {
                sink(MethodOutcome::failure(Error::make(
                    ErrorCode::SessionConflict,
                    "this session already holds the pointer on another window; call "
                    "Input.releaseAll first")));
                return;
            }
            for (const auto& [connection, other] : sessions) {
                if (connection != context.connection && other.pointer_window == window &&
                    other.held_buttons != 0) {
                    sink(MethodOutcome::failure(Error::make(
                        ErrorCode::SessionConflict,
                        "another session holds the pointer on this window")));
                    return;
                }
            }
            // Node/stableKey mode: resolve through the authoritative owner
            // (engine kinds or the issuing provider's resolve_node), so a
            // ref from a dead generation never silently matches a
            // replacement object. A snapshot id may pin the merged layout
            // revision (strict); without `strict` the click recomputes
            // geometry for animation movement. The click lands inside the
            // visible clip, never at a covered coordinate.
            Point point;
            const JsonValue* node_value = context.params.find("node");
            const JsonValue* key_value = context.params.find("stableKey");
            if (node_value && node_value->is_string()) {
                std::uint64_t node = 0;
                if (!id_from_wire(node_value->as_string(), node)) {
                    sink(MethodOutcome::failure(
                        Error::invalid_params("params.node: not a decimal id string")));
                    return;
                }
                SemanticNode target;
                if (!resolve_node_ref(window, node, target)) {
                    sink(MethodOutcome::failure(Error::stale_node(
                        "node " + node_value->as_string() + " is not in the current tree")));
                    return;
                }
                if (strict_snapshot_mismatch(window, context.params)) {
                    sink(MethodOutcome::failure(
                        Error::stale_node("layout changed since the snapshot was taken")));
                    return;
                }
                if (!target.enabled || !target.visible || target.clip.width <= 0 ||
                    target.clip.height <= 0) {
                    sink(MethodOutcome::failure(Error::make(
                        ErrorCode::NotInteractable,
                        "node " + node_value->as_string() + " is not interactable now")));
                    return;
                }
                point = interactable_point(target);
            } else if (key_value && key_value->is_string()) {
                SemanticNode target;
                if (!resolve_stable_key(window, key_value->as_string(), target)) {
                    sink(MethodOutcome::failure(Error::stale_node(
                        "no node with stableKey '" + key_value->as_string() + "'")));
                    return;
                }
                if (strict_snapshot_mismatch(window, context.params)) {
                    sink(MethodOutcome::failure(
                        Error::stale_node("layout changed since the snapshot was taken")));
                    return;
                }
                if (!target.enabled || !target.visible || target.clip.width <= 0 ||
                    target.clip.height <= 0) {
                    sink(MethodOutcome::failure(Error::make(
                        ErrorCode::NotInteractable,
                        "node '" + key_value->as_string() + "' is not interactable now")));
                    return;
                }
                point = interactable_point(target);
            } else if (!wire_xy(context.params, point, sink)) {
                return;
            }
            for (std::int64_t i = 1; i <= count; ++i) {
                Event down;
                down.type = EventType::PointerDown;
                down.window = window;
                down.size = platform.client_size(window);
                down.client = point;
                down.screen = client_to_screen(window, point);
                down.button = button;
                down.click_count = static_cast<int>(i);
                shell.on_event(down);
                // Releases carry no click count, matching the native path
                // (WM_*BUTTONDBLCLK marks only the second press).
                Event up;
                up.type = EventType::PointerUp;
                up.window = window;
                up.size = platform.client_size(window);
                up.client = point;
                up.screen = down.screen;
                up.button = button;
                shell.on_event(up);
            }
            session.pointer_window = window;
            session.last_client = point;
            session.has_last_client = true;
            JsonValue result = JsonValue::object();
            result.set("windowId", id_to_wire(window));
            result.set("clicked", count);
            result.set("x", static_cast<std::int64_t>(point.x));
            result.set("y", static_cast<std::int64_t>(point.y));
            sink(MethodOutcome::success(std::move(result)));
            return;
        }

        if (context.method == "Input.pointer") {
            const JsonValue* kind_value = context.params.find("kind");
            const std::string kind = kind_value && kind_value->is_string()
                                         ? kind_value->as_string()
                                         : std::string{};
            Point point;
            if (!wire_xy(context.params, point, sink)) return;
            if (kind == "down") {
                PointerButton button;
                if (!wire_button(context.params, button, sink)) return;
                int click_count = 1;
                if (const JsonValue* value = context.params.find("clickCount");
                    value && value->is_integer() && value->as_integer() >= 1 &&
                    value->as_integer() <= 2) {
                    click_count = static_cast<int>(value->as_integer());
                }
                if (session.pointer_window != 0 && session.pointer_window != window) {
                    sink(MethodOutcome::failure(Error::make(
                        ErrorCode::SessionConflict,
                        "this session already holds the pointer on another window; call "
                        "Input.releaseAll first")));
                    return;
                }
                for (const auto& [connection, other] : sessions) {
                    if (connection != context.connection && other.pointer_window == window &&
                        other.held_buttons != 0) {
                        sink(MethodOutcome::failure(Error::make(
                            ErrorCode::SessionConflict,
                            "another session holds the pointer on this window")));
                        return;
                    }
                }
                Event down;
                down.type = EventType::PointerDown;
                down.window = window;
                down.size = platform.client_size(window);
                down.client = point;
                down.screen = client_to_screen(window, point);
                down.button = button;
                down.click_count = click_count;
                shell.on_event(down);
                session.pointer_window = window;
                session.held_buttons |= button_bit(button);
                session.last_client = point;
                session.has_last_client = true;
            } else if (kind == "move") {
                Event move;
                move.type = EventType::PointerMove;
                move.window = window;
                move.size = platform.client_size(window);
                move.client = point;
                move.screen = client_to_screen(window, point);
                move.button = session.held_buttons & kPointerButtonLeft     ? PointerButton::Left
                              : session.held_buttons & kPointerButtonRight  ? PointerButton::Right
                              : session.held_buttons & kPointerButtonMiddle ? PointerButton::Middle
                                                                            : PointerButton::None;
                move.buttons = session.held_buttons;
                shell.on_event(move);
                if (session.pointer_window == window || session.pointer_window == 0) {
                    session.last_client = point;
                    session.has_last_client = true;
                }
            } else if (kind == "up") {
                PointerButton button;
                if (!wire_button(context.params, button, sink)) return;
                Event up;
                up.type = EventType::PointerUp;
                up.window = window;
                up.size = platform.client_size(window);
                up.client = point;
                up.screen = client_to_screen(window, point);
                up.button = button;
                shell.on_event(up);
                session.held_buttons &= ~button_bit(button);
                if (session.held_buttons == 0) session.pointer_window = 0;
            } else {
                sink(MethodOutcome::failure(
                    Error::invalid_params("params.kind: expected down, move or up")));
                return;
            }
            JsonValue result = JsonValue::object();
            result.set("windowId", id_to_wire(window));
            result.set("kind", kind);
            sink(MethodOutcome::success(std::move(result)));
            return;
        }

        if (context.method == "Input.drag") {
            Point from;
            Point to;
            if (!wire_point(context.params, "from", from, sink) ||
                !wire_point(context.params, "to", to, sink)) {
                return;
            }
            PointerButton button;
            if (!wire_button(context.params, button, sink)) return;
            if (button == PointerButton::None) {
                sink(MethodOutcome::failure(
                    Error::invalid_params("params.button: drags need a real button")));
                return;
            }
            if (session.pointer_window != 0 && session.pointer_window != window) {
                sink(MethodOutcome::failure(Error::make(
                    ErrorCode::SessionConflict,
                    "this session already holds the pointer on another window; call "
                    "Input.releaseAll first")));
                return;
            }
            for (const auto& [connection, other] : sessions) {
                if (connection != context.connection && other.pointer_window == window &&
                    other.held_buttons != 0) {
                    sink(MethodOutcome::failure(Error::make(
                        ErrorCode::SessionConflict,
                        "another session holds the pointer on this window")));
                    return;
                }
            }
            std::int64_t steps = 14;
            if (const JsonValue* value = context.params.find("steps");
                value && value->is_integer() && value->as_integer() >= 2 &&
                value->as_integer() <= 64) {
                steps = value->as_integer();
            }
            const WindowTabs* source = shell.model().window(window);
            if (!source || source->tabs.empty()) {
                sink(MethodOutcome::failure(
                    Error::target_not_found("a tab to drag in window " + id_to_wire(window))));
                return;
            }
            const Point source_origin = platform.client_origin(window);
            const Point from_screen = client_to_screen(window, from);
            const float scale = platform.scale(window);
            const int slop =
                std::max(1, static_cast<int>(std::lround((kDragSlopPx + 2) * scale)));
            // Press to arm the drag on the tab under the point.
            Event down;
            down.type = EventType::PointerDown;
            down.window = window;
            down.size = platform.client_size(window);
            down.client = from;
            down.screen = from_screen;
            down.button = button;
            shell.on_event(down);
            const WindowTabs* after_down = shell.model().window(window);
            const TabId tab = after_down ? after_down->active : 0;
            session.pointer_window = window;
            session.held_buttons |= button_bit(button);
            session.last_client = from;
            session.has_last_client = true;
            const auto window_of_tab = [&](TabId target) {
                for (const WindowId id : shell.model().window_ids()) {
                    const WindowTabs* tabs = shell.model().window(id);
                    if (std::any_of(tabs->tabs.begin(), tabs->tabs.end(),
                                    [target](const Tab& candidate) {
                                        return candidate.id == target;
                                    })) {
                        return id;
                    }
                }
                return static_cast<WindowId>(0);
            };
            WindowId current = window;
            bool attached_or_lost = false;
            for (std::int64_t i = 1; i <= steps; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(steps);
                Point target{static_cast<int>(std::lround(from.x + (to.x - from.x) * t)),
                             static_cast<int>(std::lround(from.y + (to.y - from.y) * t))};
                if (i == 1) {
                    // Guarantee the slop crossing on the first move.
                    const int dx = target.x - from.x;
                    const int dy = target.y - from.y;
                    const double distance = std::sqrt(static_cast<double>(dx) * dx + dy * dy);
                    if (distance < slop && distance > 0.5) {
                        const double grow = slop / distance;
                        target = {from.x + static_cast<int>(std::lround(dx * grow)),
                                  from.y + static_cast<int>(std::lround(dy * grow))};
                    } else if (distance <= 0.5) {
                        target = {from.x + slop, from.y};
                    }
                }
                current = window_of_tab(tab);
                if (current == 0) {
                    attached_or_lost = true;   // tab vanished mid-drag
                    break;
                }
                if (current != window) {
                    // Torn into its own window: move it along the same
                    // grab vector, then deliver the Moving event the drag
                    // state machine consumes.
                    const Point screen{from_screen.x + (target.x - from.x),
                                       from_screen.y + (target.y - from.y)};
                    platform.set_client_origin(
                        current, {source_origin.x + (screen.x - from_screen.x),
                                  source_origin.y + (screen.y - from_screen.y)});
                    Event moving;
                    moving.type = EventType::Moving;
                    moving.window = current;
                    moving.screen = screen;
                    shell.on_event(moving);
                    const WindowId after_move = window_of_tab(tab);
                    if (after_move != current) {
                        current = after_move;   // attached over another strip
                        attached_or_lost = true;
                        break;
                    }
                } else {
                    Event move;
                    move.type = EventType::PointerMove;
                    move.window = window;
                    move.size = platform.client_size(window);
                    move.client = target;
                    move.screen = client_to_screen(window, target);
                    move.button = button;
                    move.buttons = button_bit(button);
                    shell.on_event(move);
                    if (!shell.model().window(window)) {
                        attached_or_lost = true;
                        break;
                    }
                }
            }
            session.pointer_window = 0;
            if (!attached_or_lost) {
                current = window_of_tab(tab);
            }
            std::string outcome;
            if (current == 0) {
                outcome = "lost";
            } else if (current == window) {
                // Still in the source window: finish as a plain release.
                Event up;
                up.type = EventType::PointerUp;
                up.window = window;
                up.size = platform.client_size(window);
                up.client = to;
                up.screen = client_to_screen(window, to);
                up.button = button;
                shell.on_event(up);
                outcome = "released";
            } else {
                // Tab lives in a torn window whose stepwise session is
                // still armed; end it so the state machine settles.
                platform.end_move_session(current, MoveLoopResult::Completed);
                outcome = "floated";
            }
            JsonValue result = JsonValue::object();
            result.set("outcome", outcome);
            result.set("tabId", id_to_wire(tab));
            result.set("windowId", id_to_wire(current));
            sink(MethodOutcome::success(std::move(result)));
            return;
        }

        if (context.method == "Input.key") {
            const JsonValue* key_value = context.params.find("key");
            if (!key_value || !key_value->is_string()) {
                sink(MethodOutcome::failure(
                    Error::invalid_params("params.key: expected a logical key name")));
                return;
            }
            int vk = 0;
            if (!logical_key_to_vk(key_value->as_string(), vk)) {
                sink(MethodOutcome::failure(Error::make(
                    ErrorCode::InvalidArgument,
                    "params.key: unknown logical key '" + key_value->as_string() + "'")));
                return;
            }
            const JsonValue* kind_value = context.params.find("kind");
            const std::string kind = kind_value && kind_value->is_string()
                                         ? kind_value->as_string()
                                         : std::string("press");
            const auto modifier = [&](const char* name) {
                const JsonValue* value = context.params.find(name);
                return value && value->is_boolean() && value->as_boolean();
            };
            const bool ctrl = modifier("ctrl");
            const bool shift = modifier("shift");
            const bool alt = modifier("alt");
            const auto send_key = [&](EventType type) {
                Event key;
                key.type = type;
                key.window = window;
                key.size = platform.client_size(window);
                key.key = vk;
                key.ctrl = ctrl;
                key.shift = shift;
                key.alt = alt;
                shell.on_event(key);
            };
            if (kind == "press") {
                send_key(EventType::KeyDown);
                send_key(EventType::KeyUp);
            } else if (kind == "down") {
                send_key(EventType::KeyDown);
                session.held_keys.push_back(vk);
                session.keyboard_window = session.keyboard_window ? session.keyboard_window
                                                                 : window;
            } else if (kind == "up") {
                send_key(EventType::KeyUp);
                std::erase(session.held_keys, vk);
            } else {
                sink(MethodOutcome::failure(
                    Error::invalid_params("params.kind: expected press, down or up")));
                return;
            }
            JsonValue result = JsonValue::object();
            result.set("windowId", id_to_wire(window));
            result.set("kind", kind);
            sink(MethodOutcome::success(std::move(result)));
            return;
        }

        if (context.method == "Input.typeText") {
            const JsonValue* text_value = context.params.find("text");
            if (!text_value || !text_value->is_string()) {
                sink(MethodOutcome::failure(
                    Error::invalid_params("params.text: expected a string")));
                return;
            }
            std::vector<char32_t> code_points;
            if (!decode_utf8(text_value->as_string(), code_points)) {
                sink(MethodOutcome::failure(
                    Error::invalid_params("params.text: not valid UTF-8")));
                return;
            }
            for (const char32_t code_point : code_points) {
                Event text;
                text.type = EventType::TextInput;
                text.window = window;
                text.size = platform.client_size(window);
                text.code_point = code_point;
                shell.on_event(text);
            }
            JsonValue result = JsonValue::object();
            result.set("windowId", id_to_wire(window));
            result.set("delivered", static_cast<std::int64_t>(code_points.size()));
            sink(MethodOutcome::success(std::move(result)));
            return;
        }

        if (context.method == "Input.wheel") {
            Point point;
            if (!wire_xy(context.params, point, sink)) return;
            std::int64_t delta_y = 0;
            if (const JsonValue* value = context.params.find("deltaY");
                value && value->is_integer()) {
                delta_y = value->as_integer();
            }
            std::int64_t delta_x = 0;
            if (const JsonValue* value = context.params.find("deltaX");
                value && value->is_integer()) {
                delta_x = value->as_integer();
            }
            Event wheel;
            wheel.type = EventType::PointerWheel;
            wheel.window = window;
            wheel.size = platform.client_size(window);
            wheel.client = point;
            wheel.screen = client_to_screen(window, point);
            wheel.wheel = static_cast<int>(std::clamp<std::int64_t>(
                delta_y, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
            wheel.wheel_x = static_cast<int>(std::clamp<std::int64_t>(
                delta_x, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
            shell.on_event(wheel);
            sink(MethodOutcome::success(JsonValue::object()));
            return;
        }

        if (context.method == "Input.ime") {
            const JsonValue* kind_value = context.params.find("kind");
            const std::string kind = kind_value && kind_value->is_string()
                                         ? kind_value->as_string()
                                         : std::string{};
            EventType type = EventType::ImeCancel;
            if (kind == "start") {
                type = EventType::ImeStart;
                session.ime_open = true;
                session.keyboard_window = session.keyboard_window ? session.keyboard_window
                                                                 : window;
            } else if (kind == "update") {
                type = EventType::ImeUpdate;
                session.ime_open = true;
            } else if (kind == "commit") {
                type = EventType::ImeCommit;
                session.ime_open = false;
            } else if (kind == "cancel") {
                type = EventType::ImeCancel;
                session.ime_open = false;
            } else {
                sink(MethodOutcome::failure(Error::invalid_params(
                    "params.kind: expected start, update, commit or cancel")));
                return;
            }
            Event composition;
            composition.type = type;
            composition.window = window;
            composition.size = platform.client_size(window);
            if (const JsonValue* value = context.params.find("text");
                value && value->is_string()) {
                composition.ime_text = value->as_string();
            }
            shell.on_event(composition);
            sink(MethodOutcome::success(JsonValue::object()));
            return;
        }

        sink(MethodOutcome::failure(
            Error::make(ErrorCode::InternalError, "input handler fell through")));
    };
    owned_domains.push_back(
        runtime.register_engine_methods("Input", std::move(methods), std::move(handler)));
}

} // namespace tabengine::automation
