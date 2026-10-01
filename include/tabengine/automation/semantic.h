#pragma once

// Semantic tree contract. The tree is not the application's widget tree and
// not an RML tree: it is the merged, automation-facing view of one window.
// The engine bridge contributes the tab strip and window chrome; registered
// providers contribute the body (address bars, file rows, menus, in-page
// controls). Providers keep authority over hit order and geometry; the
// engine never recomputes application layout.

#include "tabengine/automation/values.h"
#include "tabengine/types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tabengine::automation {

// Role strings on the wire. Engine roles come from the tab strip and window
// chrome; application roles extend the list. Unknown roles are preserved
// verbatim so newer providers stay readable to older tools.
namespace roles {
inline constexpr std::string_view kWindow = "window";
inline constexpr std::string_view kTabStrip = "tabStrip";
inline constexpr std::string_view kTab = "tab";
inline constexpr std::string_view kTabCloseButton = "tabCloseButton";
inline constexpr std::string_view kNewTabButton = "newTabButton";
inline constexpr std::string_view kCaptionButton = "captionButton";
inline constexpr std::string_view kCaptionArea = "captionArea";
inline constexpr std::string_view kBody = "body";
inline constexpr std::string_view kButton = "button";
inline constexpr std::string_view kText = "text";
inline constexpr std::string_view kEdit = "edit";
inline constexpr std::string_view kListItem = "listItem";
inline constexpr std::string_view kPane = "pane";
inline constexpr std::string_view kCustom = "custom";
} // namespace roles

// One actionable element. `node` is the provider-assigned node ref, unique
// within its provider slot and meaningful only together with the snapshot
// revision that produced it. `stable_key` is the business identity (never a
// display string and never an index); name is display text and may be
// localized.
struct SemanticNode {
    std::string role;
    std::string name;
    std::string stable_key;
    std::uint64_t node = 0;
    std::uint64_t parent = 0;      // 0 for roots
    WindowId window = 0;
    ContentId content = 0;
    Rect bounds{};                 // client pixels at snapshot time
    Rect clip{};                   // visible portion the provider computed;
                                   // empty rect = fully clipped
    bool visible = true;
    bool enabled = true;
    bool focused = false;
    bool selected = false;
    std::vector<std::string> actions;   // "click", "select", "activate", ...
    JsonValue details;
};

struct SemanticSnapshot {
    WindowId window = 0;
    std::uint64_t revision = 0;             // tree/content revision, per window, monotonic
    std::uint64_t layout_revision = 0;      // geometry revision, per window, monotonic
    std::uint64_t document_generation = 0;  // application content generation
    double captured_at = 0.0;               // monotonic seconds
    bool truncated = false;                 // node/page budget hit
    std::uint64_t total_nodes = 0;          // before truncation
    std::vector<SemanticNode> nodes;        // depth-first; roots have parent 0
};

// Providers run on the UI thread. A provider returns false from
// provide_semantic when it cannot describe the window (then the window's
// snapshot simply lacks that provider's subtree - the engine bridge
// description still stands).
class ISemanticProvider {
public:
    virtual ~ISemanticProvider() = default;
    // Build the provider's subtree for the window. Implementations must be
    // consistent with the application's own hit testing.
    virtual bool provide_semantic(WindowId window, SemanticSnapshot& out) = 0;
    // Re-resolve a node ref issued by this provider. Return false when the
    // ref is stale (replaced, removed, or from an older generation);
    // `resolved.revision`/`layout_revision` stay authoritative in the caller.
    virtual bool resolve_node(WindowId window, std::uint64_t node, SemanticNode& resolved) = 0;
};

// Registration handle for an observation provider. Destroying it detaches
// the provider; in-flight requests then fail with ProviderUnavailable.
class ProviderRegistration {
public:
    ProviderRegistration() = default;
    ProviderRegistration(ProviderRegistration&& other) noexcept;
    ProviderRegistration& operator=(ProviderRegistration&& other) noexcept;
    ProviderRegistration(const ProviderRegistration&) = delete;
    ProviderRegistration& operator=(const ProviderRegistration&) = delete;
    ~ProviderRegistration();

    [[nodiscard]] bool valid() const { return releaser_ != nullptr; }

private:
    friend class AutomationRuntime;
    struct Releaser;
    explicit ProviderRegistration(std::shared_ptr<Releaser> releaser);
    std::shared_ptr<Releaser> releaser_;
};

} // namespace tabengine::automation
