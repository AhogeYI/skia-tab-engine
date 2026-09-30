#pragma once

// Activity contract: a bounded description of finite work in flight, used
// by wait conditions. Providers report explicit scopes ("listing",
// "fileOperations", "previews"). Long-lived work that never settles -
// terminal watchers, folder watchers, blinking carets - must NOT be counted:
// wait conditions never block on it. An instance without a provider simply
// reports the scope as unsupported instead of pretending zero tasks.

#include "tabengine/automation/values.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace tabengine::automation {

struct ActivityReport {
    std::uint64_t generation = 0;  // bumped when the scope's work set changes
    std::size_t pending = 0;       // finite tasks outstanding in this scope
    std::string describe;          // one-line summary of the oldest task; may be empty
};

class IActivityProvider {
public:
    virtual ~IActivityProvider() = default;
    // UI-thread snapshot. Return false when the scope name is unknown; the
    // wait condition then reports the scope as unsupported.
    virtual bool activity(std::string_view scope, ActivityReport& out) = 0;
    // Ask to be notified ON THE UI THREAD when the named scope reaches zero
    // pending tasks for a generation >= `generation`. Providers that cannot
    // watch (or already-quiet scopes) may invoke the callback immediately.
    // The callback must be invoked exactly once and never after the
    // provider is unregistered; the runtime ignores late calls.
    virtual void await_quiet(std::string_view scope, std::uint64_t generation,
                             std::function<void()>) {}
    // Scopes this provider can describe; surfaced in the schema/capabilities.
    [[nodiscard]] virtual std::vector<std::string> scopes() const { return {}; }
};

} // namespace tabengine::automation
