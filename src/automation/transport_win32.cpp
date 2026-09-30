// Winsock implementation of the loopback HTTP transport. One thread per
// active connection; each socket is owned exclusively by its worker (the
// runtime's response sink only hands payloads over), so no cross-thread
// writes or closes can race. UI work is never done here: JSON-RPC payloads
// go through AutomationRuntime::receive_with_deadline and responses come
// back through the response sink.
//
// Lifecycle rules:
//  - Every socket read/write runs against an ABSOLUTE deadline (checked in
//    <= 250 ms slices) and aborts on stop, so a slow client cannot extend
//    its own budget and stop() joins within a bounded delay without ever
//    closing a socket from another thread.
//  - The listener is closed only after the accept thread joined (single
//    owner at a time).
//  - Workers publish a finished flag; the accept loop reaps only finished
//    threads, so bookkeeping never blocks on a busy worker.
//  - Concurrency slots are reserved before a worker spawns, so admission
//    cannot be raced.

#include "tabengine/automation/transport.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tabengine::automation {
namespace {

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr double kAcceptPollSeconds = 0.25;
// Select slices: bounded stop() latency without cross-thread socket writes.
constexpr double kIoSliceSeconds = 0.25;

struct SocketGuard {
    SOCKET socket = INVALID_SOCKET;
    ~SocketGuard() {
        if (socket != INVALID_SOCKET) closesocket(socket);
    }
    SocketGuard() = default;
    explicit SocketGuard(SOCKET value) : socket(value) {}
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;
};

bool parse_http_request(const std::string& header, std::string& method, std::string& target,
                        std::size_t& content_length, std::string& host,
                        std::string& origin, std::string& authorization,
                        std::string& session_header, bool& has_body) {
    const std::size_t line_end = header.find("\r\n");
    if (line_end == std::string::npos) return false;
    const std::string request_line = header.substr(0, line_end);
    const std::size_t first_space = request_line.find(' ');
    const std::size_t second_space =
        first_space == std::string::npos ? std::string::npos : request_line.find(' ', first_space + 1);
    if (first_space == std::string::npos || second_space == std::string::npos) return false;
    method = request_line.substr(0, first_space);
    target = request_line.substr(first_space + 1, second_space - first_space - 1);
    const std::string version = request_line.substr(second_space + 1);
    if (version != "HTTP/1.1" && version != "HTTP/1.0") return false;

    content_length = 0;
    bool saw_length = false;
    std::size_t position = line_end + 2;
    while (position < header.size()) {
        const std::size_t end = header.find("\r\n", position);
        if (end == std::string::npos) break;
        if (end == position) break;   // end of headers
        const std::string line = header.substr(position, end - position);
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) return false;
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.erase(value.begin());
        for (char& c : name) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if (name == "content-length") {
            if (saw_length) return false;   // duplicate Content-Length
            saw_length = true;
            try {
                content_length = static_cast<std::size_t>(std::stoull(value));
            } catch (...) {
                return false;
            }
        } else if (name == "transfer-encoding") {
            return false;   // chunked bodies are not part of the v1 profile
        } else if (name == "host") {
            host = value;
        } else if (name == "origin") {
            origin = value;
        } else if (name == "authorization") {
            authorization = value;
        } else if (name == "x-tabengine-session") {
            session_header = value;
        }
        position = end + 2;
    }
    has_body = saw_length;
    return true;
}

bool header_ends(const std::string& data) {
    return data.find("\r\n\r\n") != std::string::npos;
}

bool is_loopback_host_header(const std::string& host) {
    if (host.empty()) return false;
    const std::size_t colon = host.rfind(':');
    std::string name = colon == std::string::npos ? host : host.substr(0, colon);
    if (!name.empty() && name.front() == '[' && name.back() == ']') {
        name = name.substr(1, name.size() - 2);
    }
    return name == "127.0.0.1" || name == "localhost" || name == "::1";
}

std::string http_status_text(int status) {
    switch (status) {
    case 200: return "200 OK";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 413: return "413 Payload Too Large";
    case 429: return "429 Too Many Requests";
    case 500: return "500 Internal Server Error";
    case 503: return "503 Service Unavailable";
    default: return std::to_string(status) + " Status";
    }
}

std::string json_error_payload(int http_status, std::string message) {
    JsonValue body = JsonValue::object();
    body.set("error", std::move(message));
    body.set("httpStatus", static_cast<std::int64_t>(http_status));
    return body.serialize();
}

std::string response_head(int status, std::string_view content_type, std::size_t content_length,
                          bool keep_alive = false) {
    std::string head = "HTTP/1.1 " + http_status_text(status) + "\r\n";
    head += "Content-Type: " + std::string(content_type) + "\r\n";
    head += "Content-Length: " + std::to_string(content_length) + "\r\n";
    head += "X-Content-Type-Options: nosniff\r\n";
    head += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    head += "\r\n";
    return head;
}

// Fixed-work comparison: no early exit on the first differing byte, and
// the loop length depends only on the longer input, so a probing client
// cannot learn the token's content or length from response timing.
bool constant_time_equal(const std::string& received, const std::string& expected) {
    const std::size_t length = std::max(received.size(), expected.size());
    unsigned difference = static_cast<unsigned>(received.size() ^ expected.size());
    for (std::size_t i = 0; i < length; ++i) {
        const auto byte_of = [](const std::string& text, std::size_t index) {
            return index < text.size() ? static_cast<unsigned char>(text[index])
                                       : static_cast<unsigned char>(0);
        };
        difference |= static_cast<unsigned>(byte_of(received, i) ^ byte_of(expected, i));
    }
    return difference == 0;
}

std::string make_session_token() {
    std::random_device device;
    char buffer[33];
    for (int i = 0; i < 16; ++i) {
        const unsigned byte = static_cast<unsigned>(device()) & 0xFFu;
        std::snprintf(buffer + i * 2, 3, "%02x", byte);
    }
    return buffer;
}

} // namespace

struct AutomationTransport::Impl {
    Impl(AutomationRuntime& runtime_value, TransportConfig config_value)
        : runtime(runtime_value), config(std::move(config_value)) {}

    AutomationRuntime& runtime;
    TransportConfig config;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    SOCKET listener = INVALID_SOCKET;
    std::uint16_t bound_port = 0;
    std::thread accept_thread;

    // Workers publish `done` when their handler returned; only finished
    // entries are reaped, so the reap pass never blocks on live I/O.
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex workers_mutex;
    std::deque<Worker> workers;

    std::atomic<std::uint64_t> next_connection{1};
    std::atomic<std::uint64_t> next_transport_request{1};

    // Logical sessions: one stable connection identity across many
    // short-lived HTTP requests (raw input gestures). Idle-expired and
    // explicitly ended sessions release their state through
    // runtime.connection_closed().
    struct LogicalSession {
        std::uint64_t connection = 0;
        double last_used = 0.0;
    };
    std::mutex sessions_mutex;
    std::unordered_map<std::string, LogicalSession> sessions;

    // Active connections awaiting their runtime response or finishing I/O.
    struct Pending {
        std::mutex mutex;
        std::condition_variable ready;
        bool responded = false;
        std::string payload;
        std::shared_ptr<RequestCancellation> cancellation;
    };
    std::mutex pendings_mutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<Pending>> pendings;

    mutable std::mutex stats_mutex;
    mutable Stats stats_value;

    std::atomic<std::uint64_t> auth_failures{0};
    std::atomic<double> auth_lock_until{0.0};

    [[nodiscard]] double monotonic() const {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    void bump(const std::function<void(Stats&)>& update) const {
        std::lock_guard<std::mutex> guard(stats_mutex);
        update(stats_value);
    }

    // Absolute-deadline readiness wait. Slices the wait so stop() is
    // honored promptly; false on deadline, stop or select failure.
    [[nodiscard]] bool wait_socket(SOCKET socket, bool write, double deadline) const {
        while (!stop_.load()) {
            const double remain = deadline - monotonic();
            if (remain <= 0.0) return false;
            const double slice = std::min(remain, kIoSliceSeconds);
            const long seconds = static_cast<long>(slice);
            timeval timeout{seconds,
                            static_cast<long>((slice - static_cast<double>(seconds)) * 1e6)};
            fd_set set;
            FD_ZERO(&set);
            FD_SET(socket, &set);
            const int ready = write ? select(0, nullptr, &set, nullptr, &timeout)
                                    : select(0, &set, nullptr, nullptr, &timeout);
            if (ready != 0) return ready > 0;
        }
        return false;
    }

    [[nodiscard]] bool send_all_deadline(SOCKET socket, const char* data, std::size_t size,
                                         double deadline) const {
        std::size_t sent = 0;
        while (sent < size) {
            if (!wait_socket(socket, /*write=*/true, deadline)) return false;
            const int result = send(socket, data + sent, static_cast<int>(size - sent), 0);
            if (result <= 0) return false;
            sent += static_cast<std::size_t>(result);
        }
        return true;
    }

    [[nodiscard]] bool receive_all_deadline(SOCKET socket, char* data, std::size_t size,
                                            double deadline) const {
        std::size_t received = 0;
        while (received < size) {
            if (!wait_socket(socket, /*write=*/false, deadline)) return false;
            const int result = recv(socket, data + received, static_cast<int>(size - received), 0);
            if (result <= 0) return false;
            received += static_cast<std::size_t>(result);
        }
        return true;
    }

    bool send_simple(SOCKET socket, int status, std::string_view content_type,
                     const std::string& body, double deadline) {
        const std::string head = response_head(status, content_type, body.size());
        return send_all_deadline(socket, head.data(), head.size(), deadline) &&
               (body.empty() ||
                send_all_deadline(socket, body.data(), body.size(), deadline));
    }

    // Session bookkeeping. Sweeps idle sessions and returns the logical
    // connections that were dropped (callers close them on the runtime).
    [[nodiscard]] std::vector<std::uint64_t> sweep_idle_sessions_locked() {
        std::vector<std::uint64_t> closed;
        const double now = monotonic();
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (config.session_idle_timeout_s > 0.0 &&
                now - it->second.last_used > config.session_idle_timeout_s) {
                closed.push_back(it->second.connection);
                it = sessions.erase(it);
            } else {
                ++it;
            }
        }
        return closed;
    }

    void close_connections_on_runtime(const std::vector<std::uint64_t>& closed) {
        for (const std::uint64_t connection : closed) {
            runtime.connection_closed(connection);
        }
    }

    void handle_connection(SOCKET socket, std::uint64_t connection);
    void accept_loop();
    void reap_finished_workers();
    static std::string url_path_only(const std::string& target) {
        const std::size_t query = target.find('?');
        return query == std::string::npos ? target : target.substr(0, query);
    }
};

void AutomationTransport::Impl::handle_connection(SOCKET socket, std::uint64_t connection) {
    SocketGuard guard(socket);
    // The accept loop reserved the concurrency slot before spawning this
    // worker; only the decrement lives here.
    struct ActiveGuard {
        const Impl* self;
        ~ActiveGuard() {
            self->bump([](Stats& stats) { --stats.active_connections; });
        }
    } active{this};

    const double io_deadline = monotonic() + config.io_timeout_s;

    std::string received;
    received.reserve(4096);
    char buffer[4096];
    while (!header_ends(received)) {
        if (received.size() >= kMaxHeaderBytes) {
            bump([](Stats& stats) { ++stats.rejected; });
            send_simple(socket, 413, "application/json",
                        json_error_payload(413, "header too large"), monotonic() + config.io_timeout_s);
            return;
        }
        if (!wait_socket(socket, /*write=*/false, io_deadline)) {
            bump([](Stats& stats) { ++stats.timeouts; });
            return;
        }
        const int result = recv(socket, buffer, sizeof(buffer), 0);
        if (result <= 0) return;
        received.append(buffer, static_cast<std::size_t>(result));
        if (received.size() > kMaxHeaderBytes + config.body_bytes) {
            bump([](Stats& stats) { ++stats.rejected; });
            send_simple(socket, 413, "application/json",
                        json_error_payload(413, "request too large"), monotonic() + config.io_timeout_s);
            return;
        }
    }

    const std::size_t header_end = received.find("\r\n\r\n") + 4;
    const std::string header = received.substr(0, header_end);
    std::string body = received.substr(header_end);

    std::string method;
    std::string target;
    std::string host;
    std::string origin;
    std::string authorization;
    std::string session_header;
    std::size_t content_length = 0;
    bool has_body = false;
    if (!parse_http_request(header, method, target, content_length, host, origin, authorization,
                            session_header, has_body)) {
        bump([](Stats& stats) { ++stats.rejected; });
        send_simple(socket, 400, "application/json",
                    json_error_payload(400, "malformed request"), monotonic() + config.io_timeout_s);
        return;
    }
    // Host and Origin must both look loopback; a browser page served from
    // anywhere else cannot reach this service even if it learns the token.
    if (!is_loopback_host_header(host) ||
        (!origin.empty() && !is_loopback_host_header(origin))) {
        bump([](Stats& stats) { ++stats.rejected; });
        send_simple(socket, 403, "application/json",
                    json_error_payload(403, "non-loopback host"), monotonic() + config.io_timeout_s);
        return;
    }
    // Auth failure lockout sits ahead of parsing the token so brute force is
    // rate-limited, not just rejected.
    if (monotonic() < auth_lock_until.load()) {
        bump([](Stats& stats) { ++stats.rejected; });
        send_simple(socket, 429, "application/json",
                    json_error_payload(429, "auth lockout"), monotonic() + config.io_timeout_s);
        return;
    }
    const std::string expected = "Bearer " + config.token;
    if (!constant_time_equal(authorization, expected)) {
        bump([](Stats& stats) { ++stats.rejected; ++stats.auth_failures; });
        const std::uint64_t failures = auth_failures.fetch_add(1) + 1;
        if (failures >= config.auth_failure_limit) {
            auth_failures.store(0);
            auth_lock_until.store(monotonic() + config.auth_lockout_s);
        }
        send_simple(socket, 401, "application/json",
                    json_error_payload(401, "missing or invalid token"), monotonic() + config.io_timeout_s);
        return;
    }
    auth_failures.store(0);

    // Body framing: read exactly Content-Length more bytes when needed.
    if (content_length > config.body_bytes) {
        bump([](Stats& stats) { ++stats.rejected; });
        send_simple(socket, 413, "application/json",
                    json_error_payload(413, "body too large"), monotonic() + config.io_timeout_s);
        return;
    }
    if (has_body && body.size() < content_length) {
        const std::size_t remaining = content_length - body.size();
        std::vector<char> tail(remaining);
        if (!receive_all_deadline(socket, tail.data(), remaining, io_deadline)) {
            bump([](Stats& stats) { ++stats.timeouts; });
            return;
        }
        body.append(tail.data(), remaining);
    }
    if (has_body && content_length == 0) body.clear();
    if (body.size() > content_length) body.resize(content_length);

    const std::string path = url_path_only(target);
    const double respond_deadline = monotonic() + config.io_timeout_s;

    // Session management routes (auth already verified above).
    if (method == "POST" && path == "/automation/v1/session") {
        bool limit_reached = false;
        std::vector<std::uint64_t> closed;
        {
            std::lock_guard<std::mutex> guard(sessions_mutex);
            closed = sweep_idle_sessions_locked();
            limit_reached = sessions.size() >= config.max_sessions;
        }
        close_connections_on_runtime(closed);
        if (limit_reached) {
            bump([](Stats& stats) { ++stats.rejected; });
            send_simple(socket, 429, "application/json",
                        json_error_payload(429, "session limit reached"), respond_deadline);
            return;
        }
        const std::string id = make_session_token();
        {
            std::lock_guard<std::mutex> guard(sessions_mutex);
            sessions.emplace(id, LogicalSession{next_connection.fetch_add(1), monotonic()});
        }
        JsonValue result = JsonValue::object();
        result.set("sessionId", id);
        result.set("idleTimeoutS", config.session_idle_timeout_s);
        send_simple(socket, 200, "application/json", result.serialize(), respond_deadline);
        return;
    }
    if (method == "DELETE" && path == "/automation/v1/session") {
        std::uint64_t ended = 0;
        {
            std::lock_guard<std::mutex> guard(sessions_mutex);
            const auto it = sessions.find(session_header);
            if (it == sessions.end()) {
                bump([](Stats& stats) { ++stats.rejected; });
                send_simple(socket, 404, "application/json",
                            json_error_payload(404, "session not found or expired"),
                            respond_deadline);
                return;
            }
            ended = it->second.connection;
            sessions.erase(it);
        }
        // Cancels the session's in-flight requests and releases any input
        // it holds (observers run on the next UI drain).
        runtime.connection_closed(ended);
        JsonValue result = JsonValue::object();
        result.set("ended", true);
        send_simple(socket, 200, "application/json", result.serialize(), respond_deadline);
        return;
    }

    // Route classification. RPC keeps the posted body; schema and artifact
    // GETs are served through real runtime requests so authorization and
    // TTL follow the same contract as every other method.
    enum class Route { Rpc, Schema, ArtifactGet };
    Route route = Route::Rpc;
    std::string artifact_id;
    if (method == "GET" && path == "/automation/v1/schema") {
        body = R"({"jsonrpc":"2.0","id":"schema","method":"System.getSchema"})";
        route = Route::Schema;
    } else if (method == "GET" && path.rfind("/automation/v1/artifacts/", 0) == 0) {
        artifact_id = path.substr(std::strlen("/automation/v1/artifacts/"));
        JsonValue request = JsonValue::object();
        request.set("jsonrpc", "2.0");
        request.set("id", "artifact");
        request.set("method", "Artifact.get");
        JsonValue params = JsonValue::object();
        params.set("artifactId", artifact_id);
        request.set("params", std::move(params));
        body = request.serialize();
        route = Route::ArtifactGet;
    } else if (method == "GET" && path == "/automation/v1/artifacts") {
        bump([](Stats& stats) { ++stats.rejected; });
        send_simple(socket, 404, "application/json",
                    json_error_payload(404, "missing artifact id"), respond_deadline);
        return;
    } else if (method == "POST" && path == "/automation/v1/schema") {
        route = Route::Schema;
    } else if (!(method == "POST" && path == "/automation/v1/rpc")) {
        bump([](Stats& stats) { ++stats.rejected; });
        send_simple(socket, 404, "application/json",
                    json_error_payload(404, "unknown route"), respond_deadline);
        return;
    }
    if (body.empty()) {
        bump([](Stats& stats) { ++stats.rejected; });
        send_simple(socket, 400, "application/json",
                    json_error_payload(400, "empty body"), respond_deadline);
        return;
    }
    if (body.size() > config.body_bytes) {
        bump([](Stats& stats) { ++stats.rejected; });
        send_simple(socket, 413, "application/json",
                    json_error_payload(413, "body too large"), respond_deadline);
        return;
    }

    // Logical connection: a session header binds this request to one stable
    // identity; without one the connection is ephemeral and closed right
    // after the response. Auth already ran, so the id is only handed to
    // callers holding the token.
    bool ephemeral = session_header.empty();
    if (!ephemeral) {
        std::uint64_t logical = 0;
        bool valid = false;
        std::uint64_t expired_session = 0;
        {
            std::lock_guard<std::mutex> guard(sessions_mutex);
            const auto it = sessions.find(session_header);
            if (it != sessions.end() &&
                (config.session_idle_timeout_s <= 0.0 ||
                 monotonic() - it->second.last_used <= config.session_idle_timeout_s)) {
                logical = it->second.connection;
                it->second.last_used = monotonic();
                valid = true;
            } else if (it != sessions.end()) {
                expired_session = it->second.connection;
                sessions.erase(it);
            }
        }
        if (expired_session != 0) runtime.connection_closed(expired_session);
        if (!valid) {
            bump([](Stats& stats) { ++stats.rejected; });
            send_simple(socket, 404, "application/json",
                        json_error_payload(404, "session not found or expired"),
                        respond_deadline);
            return;
        }
        connection = logical;
        ephemeral = false;
    }

    const std::uint64_t transport_request = next_transport_request.fetch_add(1);
    auto pending = std::make_shared<Pending>();
    {
        std::lock_guard<std::mutex> guard(pendings_mutex);
        pendings.emplace(transport_request, pending);
    }
    bump([](Stats& stats) { ++stats.requests; });
    // The response timeout also caps the runtime deadline, and the returned
    // handle cancels the request when the transport gives up on it, so a
    // queued mutation never executes after the client saw a failure.
    // Assignment happens under the pending mutex: stop() reads the handle
    // through the same mutex and must never miss a just-submitted request.
    {
        auto cancellation = runtime.receive_with_deadline(connection, transport_request,
                                                          std::move(body),
                                                          config.response_timeout_s);
        std::lock_guard<std::mutex> lock(pending->mutex);
        pending->cancellation = std::move(cancellation);
    }

    std::string payload;
    {
        std::unique_lock<std::mutex> lock(pending->mutex);
        if (!pending->ready.wait_for(lock,
                                     std::chrono::duration<double>(config.response_timeout_s),
                                     [&] { return pending->responded || stop_.load(); })) {
            bump([](Stats& stats) { ++stats.timeouts; });
        }
        payload = pending->payload;
    }
    {
        std::lock_guard<std::mutex> guard(pendings_mutex);
        pendings.erase(transport_request);
    }
    if (payload.empty()) {
        // Cancel precisely: a still-queued request is dropped, an executing
        // one settles Cancelled at the next drain (already-executed side
        // effects stay queryable through the operation record).
        std::shared_ptr<RequestCancellation> cancellation;
        {
            std::lock_guard<std::mutex> lock(pending->mutex);
            cancellation = pending->cancellation;
        }
        if (cancellation) cancellation->cancel();
        send_simple(socket, 503, "application/json",
                    json_error_payload(503, "no response before the transport deadline"),
                    monotonic() + config.io_timeout_s);
    } else if (route == Route::ArtifactGet) {
        // Map the JSON-RPC outcome of the authorization/TTL request onto
        // HTTP; only then read the bytes.
        int status = 500;
        std::string message = "artifact request failed";
        bool authorized = false;
        JsonValue envelope;
        if (parse_json_strict(payload, config.body_bytes * 4, envelope).status ==
            JsonParseStatus::Ok) {
            if (const JsonValue* error = envelope.find("error")) {
                std::int64_t code = 0;
                if (const JsonValue* code_value = error->find("code");
                    code_value && code_value->is_integer()) {
                    code = code_value->as_integer();
                }
                if (const JsonValue* message_value = error->find("message");
                    message_value && message_value->is_string()) {
                    message = message_value->as_string();
                }
                status = code == 1001 ? 403
                        : code == 1002 ? 404
                        : code == 1009 || code == 1006 || code == 1007 ? 503
                        : code == -32602                                    ? 400
                                                                       : 500;
            } else if (envelope.find("result") != nullptr) {
                authorized = true;
            }
        }
        ArtifactStore::Entry entry;
        if (authorized &&
            (artifact_id.empty() || !runtime.artifacts().get(artifact_id, entry))) {
            // Authorized, but the artifact expired or was evicted between
            // the request and this read.
            authorized = false;
            status = 404;
            message = "artifact not found or expired";
        }
        if (!authorized) {
            send_simple(socket, status, "application/json",
                        json_error_payload(status, message),
                        monotonic() + config.io_timeout_s);
        } else {
            const std::string head =
                response_head(200, entry.media_type, entry.bytes.size());
            std::string with_etag = head;
            // Insert the ETag as its own header line ahead of the blank line.
            const std::string etag = "\r\nETag: \"" + entry.sha256 + "\"";
            with_etag.insert(with_etag.find("\r\n\r\n"), etag);
            const bool ok =
                send_all_deadline(socket, with_etag.data(), with_etag.size(),
                                  monotonic() + config.io_timeout_s) &&
                send_all_deadline(socket, reinterpret_cast<const char*>(entry.bytes.data()),
                                  entry.bytes.size(), monotonic() + config.io_timeout_s);
            if (!ok) bump([](Stats& stats) { ++stats.timeouts; });
        }
    } else if (route == Route::Schema) {
        // Serve the document itself, not the JSON-RPC envelope.
        JsonValue envelope;
        if (parse_json_strict(payload, config.body_bytes * 4, envelope).status ==
            JsonParseStatus::Ok) {
            if (const JsonValue* result = envelope.find("result")) {
                payload = result->serialize();
            }
        }
        if (!send_simple(socket, 200, "application/json", payload,
                         monotonic() + config.io_timeout_s)) {
            bump([](Stats& stats) { ++stats.timeouts; });
        }
    } else if (!send_simple(socket, 200, "application/json", payload,
                            monotonic() + config.io_timeout_s)) {
        bump([](Stats& stats) { ++stats.timeouts; });
    }

    // Ephemeral requests end their logical connection here: queued work is
    // cancelled and any input the request left held is released on the next
    // UI drain. Session requests keep their identity across requests.
    if (ephemeral) runtime.connection_closed(connection);
}

void AutomationTransport::Impl::accept_loop() {
    while (!stop_.load()) {
        // Idle session sweep: expiry must not depend on client traffic.
        {
            std::vector<std::uint64_t> closed;
            {
                std::lock_guard<std::mutex> guard(sessions_mutex);
                closed = sweep_idle_sessions_locked();
            }
            close_connections_on_runtime(closed);
        }

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        timeval timeout{0, static_cast<long>(kAcceptPollSeconds * 1'000'000)};
        if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) {
            reap_finished_workers();
            continue;
        }
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            reap_finished_workers();
            continue;
        }
        // Reserve the concurrency slot before spawning, so admission cannot
        // be raced by workers that have not started yet.
        {
            std::lock_guard<std::mutex> guard(stats_mutex);
            if (stats_value.active_connections >= config.max_connections) {
                const std::string body = json_error_payload(503, "connection limit reached");
                const std::string head = response_head(503, "application/json", body.size());
                (void)send_all_deadline(client, head.data(), head.size(),
                                        monotonic() + config.io_timeout_s);
                (void)send_all_deadline(client, body.data(), body.size(),
                                        monotonic() + config.io_timeout_s);
                closesocket(client);
                ++stats_value.rejected;
                continue;
            }
            ++stats_value.active_connections;
        }
        const std::uint64_t connection = next_connection.fetch_add(1);
        {
            auto done = std::make_shared<std::atomic<bool>>(false);
            std::lock_guard<std::mutex> guard(workers_mutex);
            workers.push_back({{}, done});
            workers.back().thread = std::thread([this, client, connection, done] {
                handle_connection(client, connection);
                done->store(true);
            });
        }
        reap_finished_workers();
    }
}

void AutomationTransport::Impl::reap_finished_workers() {
    std::lock_guard<std::mutex> guard(workers_mutex);
    for (auto it = workers.begin(); it != workers.end();) {
        if (it->done->load()) {
            if (it->thread.joinable()) it->thread.join();
            it = workers.erase(it);
        } else {
            ++it;
        }
    }
}

// ---- public surface ----

AutomationTransport::AutomationTransport(AutomationRuntime& runtime, TransportConfig config)
    : impl_(std::make_unique<Impl>(runtime, std::move(config))) {}

AutomationTransport::~AutomationTransport() { stop(); }

bool AutomationTransport::start() {
    Impl& impl = *impl_;
    if (impl.running_.load()) return true;
    if (impl.config.token.empty() || impl.config.host != "127.0.0.1") return false;
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    impl.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl.listener == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }
    BOOL exclusive = TRUE;
    setsockopt(impl.listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(impl.config.port);
    inet_pton(AF_INET, impl.config.host.c_str(), &address.sin_addr);
    if (bind(impl.listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(impl.listener, 8) != 0) {
        closesocket(impl.listener);
        impl.listener = INVALID_SOCKET;
        WSACleanup();
        return false;
    }
    sockaddr_in bound{};
    int length = sizeof(bound);
    if (getsockname(impl.listener, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
        closesocket(impl.listener);
        impl.listener = INVALID_SOCKET;
        WSACleanup();
        return false;
    }
    impl.bound_port = ntohs(bound.sin_port);
    impl.stop_.store(false);
    impl.running_.store(true);
    impl.runtime.set_response_sink(
        [impl_ptr = impl_.get()](std::uint64_t, std::uint64_t transport_request,
                                 std::string payload) {
            std::shared_ptr<Impl::Pending> pending;
            {
                std::lock_guard<std::mutex> guard(impl_ptr->pendings_mutex);
                const auto it = impl_ptr->pendings.find(transport_request);
                if (it == impl_ptr->pendings.end()) return;   // response after the client left
                pending = it->second;
            }
            {
                std::lock_guard<std::mutex> guard(pending->mutex);
                pending->payload = std::move(payload);
                pending->responded = true;
            }
            pending->ready.notify_all();
        });
    impl.accept_thread = std::thread([&impl] { impl.accept_loop(); });
    return true;
}

void AutomationTransport::stop() {
    Impl& impl = *impl_;
    if (!impl.running_.load() && impl.listener == INVALID_SOCKET) return;
    impl.stop_.store(true);
    // The listener's single owner while running is the accept thread; it
    // exits within one poll slice, and only then does stop() close.
    if (impl.accept_thread.joinable()) impl.accept_thread.join();
    if (impl.listener != INVALID_SOCKET) {
        closesocket(impl.listener);
        impl.listener = INVALID_SOCKET;
    }
    // Cancel outstanding runtime requests first, then wake their waiters:
    // a still-queued request must not execute after the transport is gone.
    {
        std::vector<std::shared_ptr<Impl::Pending>> waiting;
        {
            std::lock_guard<std::mutex> guard(impl.pendings_mutex);
            waiting.reserve(impl.pendings.size());
            for (auto& [request, pending] : impl.pendings) {
                (void)request;
                waiting.push_back(pending);
            }
        }
        for (const std::shared_ptr<Impl::Pending>& pending : waiting) {
            std::shared_ptr<RequestCancellation> cancellation;
            {
                std::lock_guard<std::mutex> lock(pending->mutex);
                cancellation = pending->cancellation;
                pending->responded = true;
            }
            if (cancellation) cancellation->cancel();
            pending->ready.notify_all();
        }
        std::lock_guard<std::mutex> guard(impl.pendings_mutex);
        impl.pendings.clear();
    }
    // Worker I/O aborts on stop within one slice; joins are bounded.
    {
        std::lock_guard<std::mutex> guard(impl.workers_mutex);
        for (Impl::Worker& worker : impl.workers) {
            if (worker.thread.joinable()) worker.thread.join();
        }
        impl.workers.clear();
    }
    // End every logical session: in-flight requests cancel, held input
    // releases on the next UI drain.
    {
        std::vector<std::uint64_t> closed;
        {
            std::lock_guard<std::mutex> guard(impl.sessions_mutex);
            closed.reserve(impl.sessions.size());
            for (auto& [id, session] : impl.sessions) {
                (void)id;
                closed.push_back(session.connection);
            }
            impl.sessions.clear();
        }
        for (const std::uint64_t connection : closed) {
            impl.runtime.connection_closed(connection);
        }
    }
    impl.runtime.set_response_sink(nullptr);
    impl.running_.store(false);
    impl.bound_port = 0;
    WSACleanup();
}

bool AutomationTransport::running() const { return impl_->running_.load(); }

std::uint16_t AutomationTransport::port() const { return impl_->bound_port; }

std::string AutomationTransport::endpoint() const {
    return "http://127.0.0.1:" + std::to_string(impl_->bound_port);
}

std::string AutomationTransport::describe() const {
    JsonValue descriptor = JsonValue::object();
    descriptor.set("protocolVersion", impl_->runtime.identity().protocol_version);
    descriptor.set("instanceId", impl_->runtime.identity().instance_id);
    descriptor.set("engineVersion", impl_->runtime.identity().engine_version);
    descriptor.set("endpoint", endpoint());
    descriptor.set("pid", static_cast<std::int64_t>(GetCurrentProcessId()));
    descriptor.set("profile", "single-request; one response per HTTP request");
    return descriptor.serialize();
}

AutomationTransport::Stats AutomationTransport::stats() const {
    std::lock_guard<std::mutex> guard(impl_->stats_mutex);
    return impl_->stats_value;
}

} // namespace tabengine::automation
