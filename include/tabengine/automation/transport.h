#pragma once

// Optional loopback HTTP transport for the automation runtime. This is the
// only place sockets appear: the runtime, the bridge and every provider stay
// transport-neutral. The service is opt-in, binds loopback only (port 0 asks
// the OS for a free port), requires a token on every route, refuses
// non-loopback Host/Origin headers, and never enables CORS.
//
// Routes (v1):
//   POST /automation/v1/rpc                 one JSON-RPC request per HTTP
//                                           request; the connection closes
//                                           after its single response
//   GET  /automation/v1/schema              the schema document
//   GET  /automation/v1/artifacts/<id>      artifact bytes (expired = 404)

#include "tabengine/automation/runtime.h"

#include <cstdint>
#include <memory>
#include <string>

namespace tabengine::automation {

struct TransportConfig {
    std::string host = "127.0.0.1";   // loopback literals only
    std::uint16_t port = 0;           // 0 = OS-assigned
    std::string token;                // required; start() fails when empty
    std::size_t header_bytes = 16 * 1024;
    std::size_t body_bytes = 256 * 1024;
    std::size_t max_connections = 8;
    double io_timeout_s = 10.0;       // per socket read/write deadline
    double response_timeout_s = 30.0; // max wait for a runtime response
    std::size_t auth_failure_limit = 5;   // then refuse new connections
    double auth_lockout_s = 5.0;
};

class AutomationTransport {
public:
    AutomationTransport(AutomationRuntime& runtime, TransportConfig config);
    ~AutomationTransport();
    AutomationTransport(const AutomationTransport&) = delete;
    AutomationTransport& operator=(const AutomationTransport&) = delete;

    // Starts the listener and worker threads. False when the token is empty,
    // the bind fails (port already used), or Winsock cannot start. A failed
    // start never silently listens on another interface.
    [[nodiscard]] bool start();
    // Stops accepting, cancels outstanding waits, joins workers and closes
    // every socket this transport owns. Safe to call repeatedly.
    void stop();

    [[nodiscard]] bool running() const;
    // Actual bound port after start (0 before).
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] std::string endpoint() const;   // "http://127.0.0.1:<port>"
    // Connection descriptor for hosts to publish: protocol version, instance
    // id, endpoint, pid. Never contains the token.
    [[nodiscard]] std::string describe() const;

    struct Stats {
        std::uint64_t requests = 0;
        std::uint64_t rejected = 0;      // auth/framing/origin refusals
        std::uint64_t auth_failures = 0;
        std::uint64_t timeouts = 0;
        std::uint64_t active_connections = 0;
    };
    [[nodiscard]] Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tabengine::automation
