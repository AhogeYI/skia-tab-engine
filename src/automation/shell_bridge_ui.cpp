// Shell bridge UI domain: semantic snapshot assembly (engine chrome plus
// application providers) and node resolution with staleness checks.

#include "shell_bridge_impl.h"

#include <algorithm>
#include <string>
#include <utility>

namespace tabengine::automation {
namespace {

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

JsonValue node_to_wire(const SemanticNode& node) {
    JsonValue value = JsonValue::object();
    value.set("node", id_to_wire(node.node));
    if (node.parent) value.set("parent", id_to_wire(node.parent));
    value.set("role", node.role);
    if (!node.name.empty()) value.set("name", node.name);
    if (!node.stable_key.empty()) value.set("stableKey", node.stable_key);
    value.set("bounds", rect_to_wire(node.bounds));
    value.set("clip", rect_to_wire(node.clip));
    value.set("visible", node.visible);
    value.set("enabled", node.enabled);
    value.set("focused", node.focused);
    value.set("selected", node.selected);
    JsonValue actions = JsonValue::array();
    for (const std::string& action : node.actions) actions.push_back(action);
    value.set("actions", std::move(actions));
    return value;
}

Rect intersect(const Rect& a, const Rect& b) {
    const int left = std::max(a.x, b.x);
    const int top = std::max(a.y, b.y);
    const int right = std::min(a.right(), b.right());
    const int bottom = std::min(a.bottom(), b.bottom());
    if (right <= left || bottom <= top) return {};
    return {left, top, right - left, bottom - top};
}

const JsonValue kObjectSchema = [] {
    JsonValue schema = JsonValue::object();
    schema.set("type", "object");
    schema.set("properties", JsonValue::object());
    return schema;
}();

} // namespace

// ---- engine node resolution ----

bool ShellAutomationBridge::Impl::resolve_engine_node(WindowId window, unsigned kind,
                                                      std::uint64_t local,
                                                      SemanticNode& out) const {
    const WindowTabs* tabs = shell.model().window(window);
    if (!tabs) return false;
    const Size size = platform.client_size(window);
    out.window = window;
    out.visible = true;
    out.enabled = true;
    switch (kind) {
    case kNodeWindow:
        out.role = std::string(roles::kWindow);
        out.stable_key = "window";
        out.bounds = {0, 0, size.width, size.height};
        out.clip = out.bounds;
        out.node = engine_node_ref(kNodeWindow, 0);
        return true;
    case kNodeBody:
        out.role = std::string(roles::kBody);
        out.stable_key = "body";
        out.bounds = shell.body_bounds(window);
        out.clip = out.bounds;
        out.node = engine_node_ref(kNodeBody, 0);
        return true;
    case kNodeTab: {
        const auto it = std::find_if(tabs->tabs.begin(), tabs->tabs.end(),
                                     [local](const Tab& tab) { return tab.id == local; });
        if (it == tabs->tabs.end() || it->closing) return false;
        for (const Shell::ChromeTarget& target : shell.chrome_targets(window)) {
            if (target.kind == Shell::ChromeTarget::Kind::Tab && target.tab == local) {
                out.role = std::string(roles::kTab);
                out.name = it->title;
                out.stable_key = "tab:" + id_to_wire(local);
                out.bounds = target.bounds;
                out.clip = target.bounds;
                out.selected = it->id == tabs->active;
                out.actions = {"select", "close", "reorder", "drag"};
                out.node = engine_node_ref(kNodeTab, local);
                out.content = it->content;
                return true;
            }
        }
        return false;
    }
    case kNodeTabClose: {
        const auto it = std::find_if(tabs->tabs.begin(), tabs->tabs.end(),
                                     [local](const Tab& tab) { return tab.id == local; });
        if (it == tabs->tabs.end() || it->closing) return false;
        for (const Shell::ChromeTarget& target : shell.chrome_targets(window)) {
            if (target.kind == Shell::ChromeTarget::Kind::CloseTab && target.tab == local) {
                out.role = std::string(roles::kTabCloseButton);
                out.name = "Close tab";
                out.stable_key = "tabClose:" + id_to_wire(local);
                out.bounds = target.bounds;
                out.clip = target.bounds;
                out.actions = {"click"};
                out.node = engine_node_ref(kNodeTabClose, local);
                return true;
            }
        }
        return false;
    }
    case kNodeNewTab:
        for (const Shell::ChromeTarget& target : shell.chrome_targets(window)) {
            if (target.kind == Shell::ChromeTarget::Kind::NewTab) {
                out.role = std::string(roles::kNewTabButton);
                out.name = "New tab";
                out.stable_key = "newTab";
                out.bounds = target.bounds;
                out.clip = target.bounds;
                out.actions = {"click"};
                out.node = engine_node_ref(kNodeNewTab, 0);
                return true;
            }
        }
        return false;
    case kNodeCaption: {
        for (const Shell::ChromeTarget& target : shell.chrome_targets(window)) {
            if (target.kind == Shell::ChromeTarget::Kind::CaptionButton &&
                target.button == static_cast<int>(local)) {
                out.role = std::string(roles::kCaptionButton);
                out.stable_key = "caption:" + std::to_string(local);
                out.bounds = target.bounds;
                out.clip = target.bounds;
                out.actions = {"click"};
                out.node = engine_node_ref(kNodeCaption, local);
                return true;
            }
        }
        return false;
    }
    case kNodeCaptionArea:
        for (const Shell::ChromeTarget& target : shell.chrome_targets(window)) {
            if (target.kind == Shell::ChromeTarget::Kind::CaptionArea) {
                out.role = std::string(roles::kCaptionArea);
                out.stable_key = "captionArea";
                out.bounds = target.bounds;
                out.clip = target.bounds;
                out.actions = {"drag"};
                out.node = engine_node_ref(kNodeCaptionArea, 0);
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

bool ShellAutomationBridge::Impl::engine_node_exists(WindowId window,
                                                     std::string_view stable_key) const {
    SemanticNode node;
    const auto starts_with = [](std::string_view text, std::string_view prefix) {
        return text.starts_with(prefix);
    };
    if (stable_key == "window" || stable_key == "body" || stable_key == "newTab" ||
        stable_key == "captionArea") {
        const unsigned kind = stable_key == "window"   ? kNodeWindow
                              : stable_key == "body"   ? kNodeBody
                              : stable_key == "newTab" ? kNodeNewTab
                                                       : kNodeCaptionArea;
        return resolve_engine_node(window, kind, 0, node);
    }
    if (starts_with(stable_key, "tab:")) {
        std::uint64_t tab = 0;
        if (id_from_wire(stable_key.substr(4), tab)) {
            return resolve_engine_node(window, kNodeTab, tab, node);
        }
        return false;
    }
    if (starts_with(stable_key, "tabClose:")) {
        std::uint64_t tab = 0;
        if (id_from_wire(stable_key.substr(9), tab)) {
            return resolve_engine_node(window, kNodeTabClose, tab, node);
        }
        return false;
    }
    if (starts_with(stable_key, "caption:")) {
        std::uint64_t index = 0;
        if (id_from_wire(stable_key.substr(8), index)) {
            return resolve_engine_node(window, kNodeCaption, index, node);
        }
        return false;
    }
    // Ask application providers.
    for (ISemanticProvider* provider : runtime.semantic_providers()) {
        SemanticSnapshot snapshot;
        if (provider->provide_semantic(window, snapshot)) {
            for (const SemanticNode& candidate : snapshot.nodes) {
                if (candidate.stable_key == stable_key) return true;
            }
        }
    }
    return false;
}

// ---- snapshot assembly ----

SemanticSnapshot ShellAutomationBridge::Impl::build_snapshot(WindowId window,
                                                             std::size_t max_nodes,
                                                             bool& truncated) {
    SemanticSnapshot snapshot;
    snapshot.window = window;
    const WindowSemantic& state = semantic[window];
    snapshot.revision = state.revision;
    snapshot.layout_revision = state.layout_revision;
    truncated = false;
    snapshot.total_nodes = 0;

    const Size size = platform.client_size(window);
    const Rect client{0, 0, size.width, size.height};
    const Rect body = shell.body_bounds(window);
    const int strip_height = size.height - body.height;

    auto append = [&](SemanticNode node) {
        snapshot.total_nodes += 1;
        if (snapshot.nodes.size() >= max_nodes ||
            snapshot.nodes.size() >= kMaxSnapshotNodes) {
            truncated = true;
            return;
        }
        snapshot.nodes.push_back(std::move(node));
    };

    SemanticNode window_node;
    if (!resolve_engine_node(window, kNodeWindow, 0, window_node)) return snapshot;
    const std::uint64_t window_ref = window_node.node;
    window_node.parent = 0;
    append(std::move(window_node));

    SemanticNode strip_node;
    strip_node.role = std::string(roles::kTabStrip);
    strip_node.stable_key = "tabStrip";
    strip_node.window = window;
    strip_node.node = engine_node_ref(kNodeStrip, 0);
    strip_node.parent = window_ref;
    strip_node.bounds = {0, 0, size.width, strip_height};
    strip_node.clip = strip_node.bounds;
    const std::uint64_t strip_ref = strip_node.node;
    append(std::move(strip_node));

    SemanticNode body_node;
    if (resolve_engine_node(window, kNodeBody, 0, body_node)) {
        body_node.parent = window_ref;
        append(std::move(body_node));
    }
    const std::uint64_t body_ref = engine_node_ref(kNodeBody, 0);

    if (const WindowTabs* tabs = shell.model().window(window)) {
        for (const Shell::ChromeTarget& target : shell.chrome_targets(window)) {
            SemanticNode node;
            switch (target.kind) {
            case Shell::ChromeTarget::Kind::Tab: {
                const auto it = std::find_if(tabs->tabs.begin(), tabs->tabs.end(),
                                             [id = target.tab](const Tab& tab) {
                                                 return tab.id == id;
                                             });
                if (it == tabs->tabs.end()) continue;
                if (!resolve_engine_node(window, kNodeTab, target.tab, node)) continue;
                node.content = it->content;
                break;
            }
            case Shell::ChromeTarget::Kind::CloseTab:
                if (!resolve_engine_node(window, kNodeTabClose, target.tab, node)) continue;
                break;
            case Shell::ChromeTarget::Kind::NewTab:
                if (!resolve_engine_node(window, kNodeNewTab, 0, node)) continue;
                break;
            case Shell::ChromeTarget::Kind::CaptionButton:
                if (!resolve_engine_node(window, kNodeCaption,
                                         static_cast<std::uint64_t>(target.button), node)) {
                    continue;
                }
                break;
            case Shell::ChromeTarget::Kind::CaptionArea:
                if (!resolve_engine_node(window, kNodeCaptionArea, 0, node)) continue;
                break;
            default:
                continue;
            }
            node.parent = strip_ref;
            node.clip = intersect(node.bounds, client);
            append(std::move(node));
        }
    }

    // Application provider subtrees attach under the body node.
    const Rect provider_clip = body;
    for (std::size_t slot = 0; slot < runtime.semantic_providers().size(); ++slot) {
        ISemanticProvider* provider = runtime.semantic_providers()[slot];
        // Providers fill a scratch snapshot; their node refs are remapped
        // into the merged tree under the body node.
        SemanticSnapshot scratch;
        if (!provider->provide_semantic(window, scratch)) continue;
        const std::uint64_t slot_base =
            static_cast<std::uint64_t>(kNodeProviderBase + slot) << 56;
        std::unordered_map<std::uint64_t, std::uint64_t> remap;
        for (const SemanticNode& node : scratch.nodes) {
            remap[node.node] = slot_base | (node.node & 0x00FFFFFFFFFFFFFFull);
        }
        for (SemanticNode node : scratch.nodes) {
            snapshot.total_nodes += 1;
            if (snapshot.nodes.size() >= max_nodes ||
                snapshot.nodes.size() >= kMaxSnapshotNodes) {
                truncated = true;
                break;
            }
            node.node = remap[node.node];
            node.parent = node.parent ? remap[node.parent] : body_ref;
            node.window = window;
            node.clip = intersect(node.bounds, provider_clip);
            snapshot.nodes.push_back(std::move(node));
        }
    }
    return snapshot;
}

void ShellAutomationBridge::Impl::register_ui_domain() {
    const std::string observe{std::string(kGrantObserve)};

    const auto descriptor = [&observe](std::string name, std::string summary, bool mutating,
                                       JsonValue params) {
        MethodDescriptor value;
        value.name = std::move(name);
        value.summary = std::move(summary);
        value.grants = {observe};
        value.mutating = mutating;
        value.params = std::move(params);
        value.result = kObjectSchema;
        return value;
    };

    std::vector<MethodDescriptor> methods;
    methods.push_back(descriptor(
        "UI.snapshot",
        "Semantic snapshot of one window: engine chrome plus application provider subtrees. "
        "Node refs are stable per provider slot; use them with Input.click node mode.",
        false,
        object_schema({
            {"windowId", string_property("Target window (decimal id string)")},
            {"maxNodes", integer_property("Maximum nodes returned", 1)},
        },
                      {"windowId"})));
    methods.push_back(descriptor(
        "UI.resolveNode",
        "Re-resolve a node ref or stable key against the current tree, returning fresh "
        "geometry and the current revisions.",
        false,
        object_schema({
            {"windowId", string_property("Target window (decimal id string)")},
            {"node", string_property("Node ref from a snapshot (decimal string)")},
            {"stableKey", string_property("Business identity instead of a node ref")},
        },
                      {"windowId"})));

    MethodHandler handler = [this](const RequestContext& context, CompletionSink sink) {
        WindowId window = 0;
        const JsonValue* window_value = context.params.find("windowId");
        if (!window_value || !window_value->is_string() ||
            !id_from_wire(window_value->as_string(), window)) {
            sink(MethodOutcome::failure(
                Error::invalid_params("params.windowId: not a decimal id string")));
            return;
        }
        if (!shell.model().window(window)) {
            sink(MethodOutcome::failure(Error::target_not_found("window " + id_to_wire(window))));
            return;
        }
        if (context.method == "UI.snapshot") {
            std::size_t max_nodes = kMaxSnapshotNodes;
            if (const JsonValue* value = context.params.find("maxNodes");
                value && value->is_integer() && value->as_integer() > 0) {
                max_nodes = std::min<std::size_t>(static_cast<std::size_t>(value->as_integer()),
                                                  kMaxSnapshotNodes);
            }
            bool truncated = false;
            const SemanticSnapshot snapshot = build_snapshot(window, max_nodes, truncated);
            JsonValue result = JsonValue::object();
            result.set("windowId", id_to_wire(window));
            result.set("revision", static_cast<std::int64_t>(snapshot.revision));
            result.set("layoutRevision", static_cast<std::int64_t>(snapshot.layout_revision));
            result.set("truncated", truncated);
            result.set("totalNodes", static_cast<std::int64_t>(snapshot.total_nodes));
            JsonValue nodes = JsonValue::array();
            for (const SemanticNode& node : snapshot.nodes) nodes.push_back(node_to_wire(node));
            result.set("nodes", std::move(nodes));
            sink(MethodOutcome::success(std::move(result)));
            return;
        }
        // UI.resolveNode
        const JsonValue* node_value = context.params.find("node");
        const JsonValue* key_value = context.params.find("stableKey");
        SemanticNode resolved;
        bool found = false;
        if (node_value && node_value->is_string()) {
            std::uint64_t node = 0;
            if (!id_from_wire(node_value->as_string(), node)) {
                sink(MethodOutcome::failure(
                    Error::invalid_params("params.node: not a decimal id string")));
                return;
            }
            const unsigned slot = static_cast<unsigned>(node >> 56);
            const std::uint64_t local = node & 0x00FFFFFFFFFFFFFFull;
            if (slot < kNodeProviderBase) {
                found = resolve_engine_node(window, slot, local, resolved);
            } else {
                const std::size_t index = slot - kNodeProviderBase;
                if (index < runtime.semantic_providers().size()) {
                    SemanticNode provider_node;
                    if (runtime.semantic_providers()[index]->resolve_node(window, local,
                                                                          provider_node)) {
                        resolved = provider_node;
                        resolved.node = node;
                        found = true;
                    }
                }
            }
        } else if (key_value && key_value->is_string()) {
            bool truncated = false;
            const SemanticSnapshot snapshot = build_snapshot(window, kMaxSnapshotNodes, truncated);
            const auto it = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
                                         [&](const SemanticNode& node) {
                                             return node.stable_key == key_value->as_string();
                                         });
            if (it != snapshot.nodes.end()) {
                resolved = *it;
                found = true;
            }
        } else {
            sink(MethodOutcome::failure(Error::invalid_params(
                "params: provide node or stableKey to resolve")));
            return;
        }
        if (!found) {
            sink(MethodOutcome::failure(Error::stale_node(
                "node no longer exists in window " + id_to_wire(window))));
            return;
        }
        const WindowSemantic& state = semantic[window];
        JsonValue result = node_to_wire(resolved);
        result.set("revision", static_cast<std::int64_t>(state.revision));
        result.set("layoutRevision", static_cast<std::int64_t>(state.layout_revision));
        sink(MethodOutcome::success(std::move(result)));
    };
    owned_domains.push_back(
        runtime.register_engine_methods("UI", std::move(methods), std::move(handler)));
}

} // namespace tabengine::automation
