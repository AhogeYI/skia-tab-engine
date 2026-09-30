// Winsock implementation of the loopback HTTP transport. One thread per
// active connection; the socket is owned exclusively by its worker (the
// runtime's response sink only hands payloads over), so no cross-thread
// writes or closes can race. UI work is never done here: JSON-RPC payloads
// go through AutomationRuntime::receive and responses come back through the
// response sink.

#include "tabengine/automation/transport.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tabengine::automation {
namespace {

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr double kAcceptPollSeconds = 0.25;

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
                        std::string& origin, std::string& authorization, bool& has_body) {
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

bool send_all(SOCKET socket, const char* data, std::size_t size, double timeout_s) {
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(socket, &writable);
    timeval timeout{static_cast<long>(timeout_s), 0};
    std::size_t sent = 0;
    while (sent < size) {
        if (select(0, nullptr, &writable, nullptr, &timeout) <= 0) return false;
        const int result = send(socket, data + sent, static_cast<int>(size - sent), 0);
        if (result <= 0) return false;
        sent += static_cast<std::size_t>(result);
        if (sent < size) {
            FD_ZERO(&writable);
            FD_SET(socket, &writable);
            timeout = {static_cast<long>(timeout_s), 0};
        }
    }
    return true;
}

bool receive_all(SOCKET socket, char* data, std::size_t size, double timeout_s) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);
    timeval timeout{static_cast<long>(timeout_s), 0};
    std::size_t received = 0;
    while (received < size) {
        if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) return false;
        const int result = recv(socket, data + received, static_cast<int>(size - received), 0);
        if (result <= 0) return false;
        received += static_cast<std::size_t>(result);
        if (received < size) {
            FD_ZERO(&readable);
            FD_SET(socket, &readable);
            timeout = {static_cast<long>(timeout_s), 0};
        }
    }
    return true;
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
    std::vector<std::thread> workers;
    mutable std::mutex workers_mutex;

    std::atomic<std::uint64_t> next_connection{1};
    std::atomic<std::uint64_t> next_transport_request{1};

    // Active connections awaiting their runtime response or finishing I/O.
    struct Pending {
        std::mutex mutex;
        std::condition_variable ready;
        bool responded = false;
        std::string payload;
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

    bool send_simple(SOCKET socket, int status, std::string_view content_type,
                     const std::string& body) {
        const std::string head = response_head(status, content_type, body.size());
        return send_all(socket, head.data(), head.size(), config.io_timeout_s) &&
               (body.empty() || send_all(socket, body.data(), body.size(), config.io_timeout_s));
    }

    void handle_connection(SOCKET socket, std::uint64_t connection);
    void accept_loop();
    static std::string url_path_only(const std::string& target) {
        const std::size_t query = target.find('?');
        return query == std::string::npos ? target : target.substr(0, query);
    }
};

void AutomationTransport::Impl::handle_connection(SOCKET socket, std::uint64_t connection) {
    SocketGuard guard(socket);
    bump([](Stats& stats) { ++stats.active_connections; });
    struct ActiveGuard {
        const Impl* self;
        ~ActiveGuard() {
            self->bump([](Stats& stats) { --stats.active_connections; });
        }
    } active{this};

    std::string received;
    received.reserve(4096);
    char buffer[4096];
    while (!header_ends(received)) {
        if (received.size() >= kMaxHeaderBytes) {
            bump([](Stats& stats) { ++stats.rejected; });
            send_simple(socket, 413, "application/json", json_error_payload(413, "header too large"));
            return;
        }
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket, &readable);
        timeval timeout{static_cast<long>(config.io_timeout_s), 0};
        if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) {
            bump([](Stats& stats) { ++stats.timeouts; });
            return;
        }
        const int result = recv(socket, buffer, sizeof(buffer), 0);
        if (result <= 0) return;
        received.append(buffer, static_cast<std::size_t>(result));
        if (received.size() > kMaxHeaderBytes + config.body_bytes) {
            bump([](Stats& stats) { ++stats.rejected; });
            send_simple(socket, 413, "application/json", json_error_payload(413, "request too large"));
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
    std::size_t content_length = 0;
    bool has_body = false;
    if (!parse_http_request(header, method, target, content_length, host, origin, authorization,
                            has_body)) {
        bump([](Stats& stats) { ++stats.rejected; });
        send_simple(socket, 400, "application/json", json_error_payload(400, "malformed request"));
        return;
    }
    // Host and Origin must both look loopback; a browser page served from
    // anywhere else cannot reach this service even if it learns the token.
    if (!is_loopback_host_header(host) ||
        (!origin.empty() && !is_loopback_host_header(origin))) {
        bump([](Stats& stats) { ++stats.rejected; });
        send_simple(socket, 403, "application/json", json_error_payload(403, "non-loopback host"));
        return;
    }
    // Auth failure lockout sits ahead of parsing the token so brute force is
    // rate-limited, not just rejected.
    if (monotonic() < auth_lock_until.load()) {
        bump([](Stats& stats) { ++stats.rejected; });
        send_simple(socket, 429, "application/json", json_error_payload(429, "auth lockout"));
        return;
    }
    const std::string expected = "Bearer " + config.token;
    if (authorization != expected) {
        bump([](Stats& stats) { ++stats.rejected; ++stats.auth_failures; });
        const std::uint64_t failures = auth_failures.fetch_add(1) + 1;
        if (failures >= config.auth_failure_limit) {
            auth_failures.store(0);
            auth_lock_until.store(monotonic() + config.auth_lockout_s);
        }
        send_simple(socket, 401, "application/json",
                    json_error_payload(401, "missing or invalid token"));
        return;
    }
    auth_failures.store(0);

    // Body framing: read exactly Content-Length more bytes when needed.
    if (content_length > config.body_bytes) {
        bump([](Stats& stats) { ++stats.rejected; });
        send_simple(socket, 413, "application/json", json_error_payload(413, "body too large"));
        return;
    }
    if (has_body && body.size() < content_length) {
        const std::size_t remaining = content_length - body.size();
        std::vector<char> tail(remaining);
        if (!receive_all(socket, tail.data(), remaining, config.io_timeout_s)) {
            bump([](Stats& stats) { ++stats.timeouts; });
            return;
        }
        body.append(tail.data(), remaining);
    }
    if (has_body && content_length == 0) body.clear();
    if (body.size() > content_length) body.resize(content_length);

    const std::string path = url_path_only(target);

    if (method == "GET" && path == "/automation/v1/artifacts") {
        bump([](Stats& stats) { ++stats.rejected; });
        send_simple(socket, 404, "application/json", json_error_payload(404, "missing artifact id"));
        return;
    }
    if (method == "GET" && path.rfind("/automation/v1/artifacts/", 0) == 0) {
        const std::string id = path.substr(std::strlen("/automation/v1/artifacts/"));
        ArtifactStore::Entry entry;
        if (id.empty() || !runtime.artifacts().get(id, entry)) {
            send_simple(socket, 404, "application/json",
                        json_error_payload(404, "artifact not found or expired"));
            return;
        }
        const std::string head =
            response_head(200, entry.media_type, entry.bytes.size());
        std::string with_etag = head;
        // Insert the ETag as its own header line ahead of the blank line.
        const std::string etag = "\r\nETag: \"" + entry.sha256 + "\"";
        with_etag.insert(with_etag.find("\r\n\r\n"), etag);
        const bool ok = send_all(socket, with_etag.data(), with_etag.size(), config.io_timeout_s) &&
                        send_all(socket, reinterpret_cast<const char*>(entry.bytes.data()),
                                 entry.bytes.size(), config.io_timeout_s);
        if (!ok) bump([](Stats& stats) { ++stats.timeouts; });
        return;
    }
    if (method == "GET" && path == "/automation/v1/schema") {
        // Schema generation touches UI-thread state; run it as a real
        // System.getSchema request through the runtime.
        body = R"({"jsonrpc":"2.0","id":"schema","method":"System.getSchema"})";
        method = "POST";
    }
    if (!(method == "POST" && path == "/automation/v1/rpc") &&
        !(method == "POST" && path == "/automation/v1/schema")) {
        bump([](Stats& stats) { ++stats.rejected; });
        send_simple(socket, 404, "application/json", json_error_payload(404, "unknown route"));
        return;
    }
    if (body.empty()) {
        bump([](Stats& stats) { ++stats.rejected; });
        send_simple(socket, 400, "application/json", json_error_payload(400, "empty body"));
        return;
    }
    if (body.size() > config.body_bytes) {
        bump([](Stats& stats) { ++stats.rejected; });
        send_simple(socket, 413, "application/json", json_error_payload(413, "body too large"));
        return;
    }

    const std::uint64_t transport_request = next_transport_request.fetch_add(1);
    auto pending = std::make_shared<Pending>();
    {
        std::lock_guard<std::mutex> guard(pendings_mutex);
        pendings.emplace(transport_request, pending);
    }
    bump([](Stats& stats) { ++stats.requests; });
    runtime.receive(connection, transport_request, std::move(body));

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
        send_simple(socket, 503, "application/json",
                    json_error_payload(503, "no response before the transport deadline"));
        return;
    }
    if (path == "/automation/v1/schema") {
        // Serve the document itself, not the JSON-RPC envelope.
        JsonValue envelope;
        if (parse_json_strict(payload, config.body_bytes * 4, envelope).status ==
            JsonParseStatus::Ok) {
            if (const JsonValue* result = envelope.find("result")) {
                payload = result->serialize();
            }
        }
    }
    if (!send_simple(socket, 200, "application/json", payload)) {
        bump([](Stats& stats) { ++stats.timeouts; });
    }
}

void AutomationTransport::Impl::accept_loop() {
    while (!stop_.load()) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        timeval timeout{0, static_cast<long>(kAcceptPollSeconds * 1'000'000)};
        if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) continue;
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        {
            std::lock_guard<std::mutex> guard(stats_mutex);
            if (stats_value.active_connections >= config.max_connections) {
                const std::string body = json_error_payload(503, "connection limit reached");
                const std::string head =
                    response_head(503, "application/json", body.size());
                (void)send_all(client, head.data(), head.size(), config.io_timeout_s);
                (void)send_all(client, body.data(), body.size(), config.io_timeout_s);
                closesocket(client);
                ++stats_value.rejected;
                continue;
            }
        }
        const std::uint64_t connection = next_connection.fetch_add(1);
        {
            std::lock_guard<std::mutex> guard(workers_mutex);
            workers.emplace_back([this, client, connection] {
                handle_connection(client, connection);
            });
        }
        // Reap finished workers.
        std::lock_guard<std::mutex> guard(workers_mutex);
        if (workers.size() > config.max_connections * 4) {
            for (auto it = workers.begin(); it != workers.end();) {
                if (it->joinable() && it->get_id() != std::this_thread::get_id()) {
                    it->join();
                }
                it = workers.erase(it);
                break;
            }
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
    if (impl.listener != INVALID_SOCKET) {
        closesocket(impl.listener);
        impl.listener = INVALID_SOCKET;
    }
    if (impl.accept_thread.joinable()) impl.accept_thread.join();
    // Wake every worker waiting on a runtime response, then join.
    {
        std::lock_guard<std::mutex> guard(impl.pendings_mutex);
        for (auto& [request, pending] : impl.pendings) {
            (void)request;
            {
                std::lock_guard<std::mutex> lock(pending->mutex);
                pending->responded = true;
            }
            pending->ready.notify_all();
        }
    }
    {
        std::lock_guard<std::mutex> guard(impl.workers_mutex);
        for (std::thread& worker : impl.workers) {
            if (worker.joinable()) worker.join();
        }
        impl.workers.clear();
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
