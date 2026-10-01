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
//   GET  /automation/v1/artifacts/<id>      artifact bytes (expired = 404);
//                                           authorized through the same
//                                           Artifact.get request contract
//                                           as the RPC route
//   POST /automation/v1/session             begin a logical session
//   DELETE /automation/v1/session           end it (with the session header)
//
// Sessions: HTTP requests are one-shot, but raw input (pointer down/move/up,
// key down/up, IME segments) needs one stable identity across requests.
// POST /automation/v1/session returns an opaque session id; sending it as
// the X-TabEngine-Session header on later requests binds them to one
// logical connection. Sessions are bound to the Bearer token (auth is
// checked before the header is consulted), expire after an idle timeout,
// and are capped by max_sessions. Ending one (explicitly, by idle expiry,
// or on transport stop) cancels its in-flight requests and releases any
// input the session holds on the UI thread. Requests without the header
// are ephemeral: their logical connection is closed right after the single
// response, releasing anything that request left held.

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
    double io_timeout_s = 10.0;       // absolute per-socket read/write deadline
    double response_timeout_s = 30.0; // max wait for a runtime response;
                                      // also caps the request deadline and
                                      // is cancelled against on timeout
    std::size_t auth_failure_limit = 5;   // then refuse new connections
    double auth_lockout_s = 5.0;
    // Optional compatibility header carrying the SAME token (e.g. a
    // product's legacy control clients). "Authorization: Bearer" stays
    // the canonical form; both feed one lockout counter.
    std::string alt_token_header;
    double session_idle_timeout_s = 30.0; // logical session idle expiry
    std::size_t max_sessions = 16;        // concurrent logical sessions
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
        std::uint64_t peer_resets = 0;   // hard disconnects mid-request
        std::uint64_t active_connections = 0;
    };
    [[nodiscard]] Stats stats() const;

    // ---- host-mounted routes ----
    // Embedders may serve additional HTTP routes (product-compat surfaces)
    // behind the SAME token, framing and loopback checks as the engine's
    // own. The handler runs on a connection worker thread: it must not
    // touch UI objects. UI work goes through `call`, which submits one
    // JSON-RPC payload to the runtime with the transport's deadline and
    // cancellation and returns the response envelope ("" on timeout or
    // stop). A reply with status 0 declines; the transport then answers
    // 404. Install before start().
    struct RouteRequest {
        std::string method;   // "GET" / "POST"
        std::string path;     // path only, no query
        std::string query;    // raw query string, no '?'
        std::string body;
    };
    struct RouteReply {
        int status = 0;                                    // 0 = not handled
        std::string content_type = "application/json";
        std::string body;
        // Optional Cache-Control value for static replies ("" sends none).
        std::string cache_control;
    };
    // `session` optionally names a logical session (see create_session);
    // empty runs the request on the incoming connection's identity.
    using RpcCall = std::function<std::string(
        std::string payload, double timeout_s, const std::string& session)>;
    // Reads artifact bytes by id (false when unknown or expired). Serves
    // host routes that relay binary results (e.g. screenshots captured
    // through Capture.screenshot).
    using ArtifactReader =
        std::function<bool(const std::string& id, std::string& bytes, std::string& media_type)>;
    using RouteHandler =
        std::function<RouteReply(const RouteRequest&, const RpcCall&, const ArtifactReader&)>;
    void set_route_handler(RouteHandler handler);
    // ---- unauthenticated static entry ----
    // Serves the devtools panel shell: GET-only static bytes the handler
    // itself owns, answered after the loopback/Origin boundary but before
    // auth so a browser can load the page without header tricks. The shell
    // carries no secrets; every data, action, and artifact request still
    // goes through the authenticated routes. A handler answering status 0
    // falls through to normal auth processing. Install before start().
    using PublicFileHandler =
        std::function<bool(const std::string& path, RouteReply& reply)>;
    void set_public_file_handler(PublicFileHandler handler);
    // Mints a logical session (stable connection identity across
    // requests) for host-side use such as legacy input gestures; returns
    // its id, or empty when max_sessions is reached. Thread-safe; idle
    // expiry and the session limit apply like any session.
    [[nodiscard]] std::string create_session();
    // Audit hook for refused authentication: invoked on the worker thread
    // with the failure count and whether the lockout was armed. Never
    // receives the token. Install before start().
    void set_auth_failure_callback(
        std::function<void(std::uint64_t failures, bool locked)> callback);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tabengine::automation
