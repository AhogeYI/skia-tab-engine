#pragma once

// Observation events. The runtime keeps one bounded, sequence-numbered ring
// per instance. Engine domains emit lifecycle, input, frame and operation
// events; applications emit their own through the runtime with names under
// "app.<owner>.". Consumers poll; gaps are reported explicitly and mean the
// caller must re-synchronize from a fresh snapshot.

#include "tabengine/automation/values.h"
#include "tabengine/types.h"

#include <cstdint>
#include <string>

namespace tabengine::automation {

// Event names used by the engine bridge. Applications may add
// "app.<owner>.<event>" names; the schema endpoint lists every name the
// instance can emit.
namespace event_names {
inline constexpr std::string_view kWindowAdded = "window.added";
inline constexpr std::string_view kWindowRemoved = "window.removed";
inline constexpr std::string_view kWindowActivated = "window.activated";
inline constexpr std::string_view kWindowDeactivated = "window.deactivated";
inline constexpr std::string_view kWindowBoundsChanged = "window.boundsChanged";
inline constexpr std::string_view kTabAdded = "tab.added";
inline constexpr std::string_view kTabRemoved = "tab.removed";
inline constexpr std::string_view kTabMoved = "tab.moved";
inline constexpr std::string_view kTabSelected = "tab.selected";
inline constexpr std::string_view kTabUpdated = "tab.updated";
inline constexpr std::string_view kTabTransferred = "tab.transferred";
inline constexpr std::string_view kInputDispatched = "input.dispatched";
inline constexpr std::string_view kAnimationSettled = "animation.settled";
inline constexpr std::string_view kFramePresented = "frame.presented";
inline constexpr std::string_view kCaptureCompleted = "capture.completed";
inline constexpr std::string_view kOperationUpdated = "operation.updated";
} // namespace event_names

struct AutomationEvent {
    std::uint64_t sequence = 0;   // per-instance, monotonic, gap-free until trimmed
    std::string name;             // dotted event name, e.g. "tab.selected"
    WindowId window = 0;
    TabId tab = 0;
    ContentId content = 0;
    std::uint64_t generation = 0; // content/document generation when applicable
    std::string request_id;       // correlating JSON-RPC id when applicable
    std::uint64_t operation = 0;  // correlating operation id when applicable
    double at = 0.0;              // monotonic seconds
    JsonValue details;            // event payload; never holds document bodies by default
};

} // namespace tabengine::automation
