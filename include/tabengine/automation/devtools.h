#pragma once

// Static web devtools panel mounted on the automation transport. The panel
// is plain HTML/CSS/JS (no web framework); this class only serves those
// bytes through the transport's unauthenticated static entry so a browser
// can open the page directly. Every piece of data and every action the
// panel performs still goes through the authenticated JSON-RPC and artifact
// routes with the bearer token held in memory by the page - never in a URL,
// a cookie, or storage.
//
// Threading: mount() runs before transport start(); the serving handler
// runs on a connection worker thread and only reads files it owns.

#include "tabengine/automation/transport.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace tabengine::automation {

class DevtoolsPanel {
public:
    struct Options {
        // Directory holding panel.html and the assets/ subdirectory.
        std::filesystem::path resources_root;
        // Entry path (also the assets prefix base).
        std::string mount_path = "/automation/v1/panel";
    };

    // Wires the static routes onto the transport. False when the resources
    // root or panel.html is missing (the transport then serves nothing for
    // the panel; hosts can treat that as "panel not shipped").
    [[nodiscard]] bool mount(AutomationTransport& transport, const Options& options);

    // A static reply never streams unbounded data.
    static constexpr std::size_t kMaxFileBytes = 2 * 1024 * 1024;
};

} // namespace tabengine::automation
