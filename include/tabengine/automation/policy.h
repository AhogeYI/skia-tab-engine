#pragma once

// Connection authorization contract. The host installs one policy; the
// runtime consults it when a transport connection arrives and before every
// method dispatch. The policy sees grants and method descriptors only - it
// never sees sockets, raw payloads, or platform types. Product concerns
// (file/process/clipboard side effects) stay gated by the application's own
// services; granting "engine.input" here already allows a client to drive
// real UI actions, so policies must treat input grants as potent.

#include "tabengine/automation/method.h"
#include "tabengine/automation/values.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tabengine::automation {

struct SessionInfo {
    std::uint64_t connection = 0;
    std::string origin;         // transport-supplied origin label (may be empty)
    std::string user_agent;     // transport-supplied client label (may be empty)
    bool loopback = true;       // v1 transports only accept loopback
};

class IAutomationPolicy {
public:
    virtual ~IAutomationPolicy() = default;
    // Return false to refuse the connection before any request executes.
    virtual bool authorize_session(const SessionInfo& session) { return true; }
    // Grant names in effect for the session. The default grants everything
    // except test hooks; hosts override to constrain.
    [[nodiscard]] virtual std::vector<std::string> grants(const SessionInfo& session) {
        return {std::string(kGrantObserve), std::string(kGrantInput),
                std::string(kGrantWindowTab)};
    }
    // Final per-method admission after grant matching. Use for test-hook
    // gating or per-owner rules. The runtime has already matched grants.
    virtual bool allow_method(const SessionInfo& session, const MethodDescriptor& method) {
        return true;
    }
    // True when this session may register... (reserved for future dynamic
    // registration; v1 has none.) Kept out of v1 on purpose.
};

} // namespace tabengine::automation
