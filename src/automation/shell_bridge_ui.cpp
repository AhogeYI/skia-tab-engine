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
    if (!node.details.is_null()) value.set("details", node.details);
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

// Effective clip for a provider node inside the merged tree: the body
// (provider) viewport intersected with the provider's own clip. An empty
// provider clip means fully clipped per the SemanticNode contract.
Rect merged_clip(const Rect& bounds, const Rect& provider_clip, const Rect& node_clip) {
    if (node_clip.width <= 0 || node_clip.height <= 0) return {};
    return intersect(intersect(bounds, provider_clip), node_clip);
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
    return resolve_engine_node_from(shell.chrome_targets(window), window, kind, local, out);
}

bool ShellAutomationBridge::Impl::resolve_engine_node_from(
    const std::vector<Shell::ChromeTarget>& targets, WindowId window, unsigned kind,
    std::uint64_t local, SemanticNode& out) const {
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
        for (const Shell::ChromeTarget& target : targets) {
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
                JsonValue details = JsonValue::object();
                details.set("tabId", id_to_wire(local));
                details.set("contentId", id_to_wire(it->content));
                out.details = std::move(details);
                return true;
            }
        }
        return false;
    }
    case kNodeTabClose: {
        const auto it = std::find_if(tabs->tabs.begin(), tabs->tabs.end(),
                                     [local](const Tab& tab) { return tab.id == local; });
        if (it == tabs->tabs.end() || it->closing) return false;
        for (const Shell::ChromeTarget& target : targets) {
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
        for (const Shell::ChromeTarget& target : targets) {
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
        for (const Shell::ChromeTarget& target : targets) {
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
        for (const Shell::ChromeTarget& target : targets) {
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
    for (const AutomationRuntime::SemanticProviderEntry& entry :
         runtime.semantic_provider_entries()) {
        SemanticSnapshot snapshot;
        if (entry.provider->provide_semantic(window, snapshot)) {
            for (const SemanticNode& candidate : snapshot.nodes) {
                if (candidate.stable_key == stable_key) return true;
            }
        }
    }
    return false;
}

// ---- merged-tree node resolution ----

bool ShellAutomationBridge::Impl::resolve_node_ref(WindowId window, std::uint64_t node,
                                                   SemanticNode& out) const {
    const unsigned slot = static_cast<unsigned>(node >> 56);
    const std::uint64_t local = node & 0x00FFFFFFFFFFFFFFull;
    if (slot == 0) return false;
    if (slot < kNodeProviderBase) return resolve_engine_node(window, slot, local, out);
    // Provider subtree: only the provider that issued the ref may resolve
    // it. Slots are never reused, so an unregistered provider's refs fail
    // instead of matching whatever provider took its place.
    for (const AutomationRuntime::SemanticProviderEntry& entry :
         runtime.semantic_provider_entries()) {
        if (entry.slot != slot) continue;
        SemanticNode provider_node;
        if (!entry.provider->resolve_node(window, local, provider_node)) return false;
        out = std::move(provider_node);
        out.node = node;
        out.window = window;
        const Rect body = shell.body_bounds(window);
        out.clip = merged_clip(out.bounds, body, out.clip);
        return true;
    }
    return false;
}

bool ShellAutomationBridge::Impl::resolve_stable_key(WindowId window, std::string_view key,
                                                     SemanticNode& out) const {
    const auto starts_with = [](std::string_view text, std::string_view prefix) {
        return text.starts_with(prefix);
    };
    if (key == "window" || key == "body" || key == "newTab" || key == "captionArea" ||
        starts_with(key, "caption:")) {
        std::uint64_t index = 0;
        const unsigned kind = key == "window"        ? kNodeWindow
                              : key == "body"        ? kNodeBody
                              : key == "newTab"      ? kNodeNewTab
                              : key == "captionArea" ? kNodeCaptionArea
                                                     : kNodeCaption;
        if (kind == kNodeCaption) {
            if (!id_from_wire(key.substr(8), index)) return false;
            return resolve_engine_node(window, kind, index, out);
        }
        return resolve_engine_node(window, kind, 0, out);
    }
    if (starts_with(key, "tab:")) {
        std::uint64_t tab = 0;
        if (!id_from_wire(key.substr(4), tab)) return false;
        return resolve_engine_node(window, kNodeTab, tab, out);
    }
    if (starts_with(key, "tabClose:")) {
        std::uint64_t tab = 0;
        if (!id_from_wire(key.substr(9), tab)) return false;
        return resolve_engine_node(window, kNodeTabClose, tab, out);
    }
    // Application providers: stable keys enumerate their subtrees (the
    // contract offers no key-based resolve); the merged ref is remapped
    // through the provider's stable slot.
    const Rect body = shell.body_bounds(window);
    for (const AutomationRuntime::SemanticProviderEntry& entry :
         runtime.semantic_provider_entries()) {
        SemanticSnapshot scratch;
        if (!entry.provider->provide_semantic(window, scratch)) continue;
        for (SemanticNode node : scratch.nodes) {
            if (node.stable_key != key) continue;
            node.node = (entry.slot << 56) | (node.node & 0x00FFFFFFFFFFFFFFull);
            node.parent = 0;
            node.window = window;
            node.clip = merged_clip(node.bounds, body, node.clip);
            out = std::move(node);
            return true;
        }
    }
    return false;
}

Point ShellAutomationBridge::Impl::interactable_point(const SemanticNode& node) {
    // Click inside the visible portion: partially clipped nodes are hit at
    // their visible center, never at a covered coordinate.
    const Rect visible = intersect(node.bounds, node.clip);
    return {visible.x + visible.width / 2, visible.y + visible.height / 2};
}

void ShellAutomationBridge::Impl::refresh_provider_revisions(WindowId window) {
    WindowSemantic& state = semantic[window];
    std::unordered_set<std::uint64_t> seen_slots;
    std::uint64_t document_generation = 0;
    for (const AutomationRuntime::SemanticProviderEntry& entry :
         runtime.semantic_provider_entries()) {
        SemanticSnapshot scratch;
        if (!entry.provider->provide_semantic(window, scratch)) continue;
        seen_slots.insert(entry.slot);
        document_generation = std::max(document_generation, scratch.document_generation);
        const auto it = state.provider_revisions.find(entry.slot);
        if (it == state.provider_revisions.end()) {
            // Newly attached provider: the tree changed.
            ++state.merged_revision;
            ++state.merged_layout_revision;
            state.provider_revisions.emplace(
                    entry.slot, std::make_pair(scratch.revision, scratch.layout_revision));
        } else {
            if (it->second.first != scratch.revision) {
                ++state.merged_revision;
                it->second.first = scratch.revision;
            }
            if (it->second.second != scratch.layout_revision) {
                ++state.merged_layout_revision;
                it->second.second = scratch.layout_revision;
            }
        }
    }
    // Detached providers (slot cached but no longer registered) changed the
    // tree too; a successor on a new slot never reuses their identity.
    std::erase_if(state.provider_revisions,
                  [&seen_slots](const auto& entry) { return !seen_slots.count(entry.first); });
    state.document_generation = document_generation;
}

bool ShellAutomationBridge::Impl::strict_snapshot_mismatch(WindowId window,
                                                           const JsonValue& params) {
    const JsonValue* snapshot_value = params.find("snapshotId");
    if (!snapshot_value || !snapshot_value->is_integer()) return false;
    const bool strict = params.find("strict") && params.find("strict")->is_boolean() &&
                        params.find("strict")->as_boolean();
    if (!strict) return false;
    refresh_provider_revisions(window);
    const WindowSemantic& state = semantic[window];
    return snapshot_value->as_integer() !=
           static_cast<std::int64_t>(state.merged_layout_revision);
}

// ---- snapshot assembly ----

SemanticSnapshot ShellAutomationBridge::Impl::build_snapshot(WindowId window,
                                                             std::size_t max_nodes,
                                                             bool& truncated) {
    SemanticSnapshot snapshot;
    snapshot.window = window;
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
        // One chrome-target pass feeds every strip node: chrome_targets()
        // recomputes the strip layout, so per-node queries made snapshot
        // assembly quadratic in the tab count.
        const std::vector<Shell::ChromeTarget> targets = shell.chrome_targets(window);
        for (const Shell::ChromeTarget& target : targets) {
            SemanticNode node;
            switch (target.kind) {
            case Shell::ChromeTarget::Kind::Tab: {
                const auto it = std::find_if(tabs->tabs.begin(), tabs->tabs.end(),
                                             [id = target.tab](const Tab& tab) {
                                                 return tab.id == id;
                                             });
                if (it == tabs->tabs.end()) continue;
                if (!resolve_engine_node_from(targets, window, kNodeTab, target.tab, node)) {
                    continue;
                }
                node.content = it->content;
                break;
            }
            case Shell::ChromeTarget::Kind::CloseTab:
                if (!resolve_engine_node_from(targets, window, kNodeTabClose, target.tab,
                                              node)) {
                    continue;
                }
                break;
            case Shell::ChromeTarget::Kind::NewTab:
                if (!resolve_engine_node_from(targets, window, kNodeNewTab, 0, node)) continue;
                break;
            case Shell::ChromeTarget::Kind::CaptionButton:
                if (!resolve_engine_node_from(targets, window, kNodeCaption,
                                              static_cast<std::uint64_t>(target.button),
                                              node)) {
                    continue;
                }
                break;
            case Shell::ChromeTarget::Kind::CaptionArea:
                if (!resolve_engine_node_from(targets, window, kNodeCaptionArea, 0, node)) {
                    continue;
                }
                break;
            default:
                continue;
            }
            node.parent = strip_ref;
            node.clip = intersect(node.bounds, client);
            append(std::move(node));
        }
    }

    // Application provider subtrees attach under the body node. Provider
    // revisions fold into the merged counters BEFORE the snapshot reports
    // them, so the pinned revision already includes provider state.
    refresh_provider_revisions(window);
    for (const AutomationRuntime::SemanticProviderEntry& entry :
         runtime.semantic_provider_entries()) {
        // Providers fill a scratch snapshot; their node refs are remapped
        // into the merged tree under the body node, keyed by the
        // provider's stable registration slot (never a list index).
        SemanticSnapshot scratch;
        if (!entry.provider->provide_semantic(window, scratch)) continue;
        const std::uint64_t slot_base = entry.slot << 56;
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
            node.clip = merged_clip(node.bounds, body, node.clip);
            snapshot.nodes.push_back(std::move(node));
        }
    }

    const WindowSemantic& state = semantic[window];
    snapshot.revision = state.merged_revision;
    snapshot.layout_revision = state.merged_layout_revision;
    snapshot.document_generation = state.document_generation;
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
            result.set("documentGeneration",
                       static_cast<std::int64_t>(snapshot.document_generation));
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
            found = resolve_node_ref(window, node, resolved);
        } else if (key_value && key_value->is_string()) {
            found = resolve_stable_key(window, key_value->as_string(), resolved);
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
        result.set("revision", static_cast<std::int64_t>(state.merged_revision));
        result.set("layoutRevision", static_cast<std::int64_t>(state.merged_layout_revision));
        result.set("documentGeneration", static_cast<std::int64_t>(state.document_generation));
        sink(MethodOutcome::success(std::move(result)));
    };
    owned_domains.push_back(
        runtime.register_engine_methods("UI", std::move(methods), std::move(handler)));
}

} // namespace tabengine::automation
