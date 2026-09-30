// Loopback transport: real HTTP round trips over a controlled inbound
// socket. Covers auth and lockout, loopback Host/Origin enforcement,
// framing rejections, schema/artifact routes, JSON-RPC error mapping and
// the transport response deadline. The UI thread pumps drain() while a
// client thread performs each request.

#include "tabengine/automation/transport.h"

#include "check.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace automation = tabengine::automation;
using automation::AutomationRuntime;
using automation::AutomationTransport;
using automation::JsonValue;
using automation::TransportConfig;

namespace {

struct Clock {
    double now_value = 10.0;
    double operator()() const { return now_value; }
};

struct HttpResponse {
    int status = 0;
    std::string body;
};

HttpResponse http_request(std::uint16_t port, const std::string& request) {
    HttpResponse response;
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(socket != INVALID_SOCKET);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    CHECK(connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    CHECK(send(socket, request.data(), static_cast<int>(request.size()), 0) ==
          static_cast<int>(request.size()));
    char buffer[4096];
    std::string raw;
    while (true) {
        const int result = recv(socket, buffer, sizeof(buffer), 0);
        if (result <= 0) break;
        raw.append(buffer, static_cast<std::size_t>(result));
    }
    closesocket(socket);
    const std::size_t line_end = raw.find("\r\n");
    if (line_end != std::string::npos) {
        response.status = std::atoi(raw.substr(9, 3).c_str());
    }
    const std::size_t body_start = raw.find("\r\n\r\n");
    if (body_start != std::string::npos) response.body = raw.substr(body_start + 4);
    if (const char* dump = std::getenv("TE_HTTP_DEBUG")) {
        if (dump[0] == '1') std::fprintf(stderr, "RAW[%.300s]\n", raw.c_str());
    }
    return response;
}

std::string rpc_request(const std::string& token, const std::string& body,
                        const std::string& host = "127.0.0.1", bool with_token = true) {
    std::string request = "POST /automation/v1/rpc HTTP/1.1\r\n";
    request += "Host: " + host + "\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    if (with_token) request += "Authorization: Bearer " + token + "\r\n";
    request += "\r\n";
    request += body;
    return request;
}

struct Harness {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0", "transport-test"}, std::ref(clock)};
    AutomationTransport transport{runtime, [] {
                                      TransportConfig config;
                                      config.token = "0123456789abcdef0123456789abcdef";
                                      config.response_timeout_s = 2.0;
                                      return config;
                                  }()};

    Harness() { CHECK(transport.start()); }

    // Pumps UI drains on this thread while `request` runs on a client
    // thread; returns the client's response.
    HttpResponse round_trip(const std::function<HttpResponse()>& request) {
        HttpResponse response;
        std::atomic<bool> done{false};
        std::thread client([&] {
            response = request();
            done.store(true);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done.load()) {
            runtime.drain(clock.now_value);
            clock.now_value += 0.001;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CHECK(std::chrono::steady_clock::now() < deadline);
        }
        client.join();
        return response;
    }
};

JsonValue parse_body(const HttpResponse& response) {
    JsonValue value;
    CHECK(automation::parse_json_strict(response.body, 65536, value).status ==
          automation::JsonParseStatus::Ok);
    return value;
}

void test_rpc_round_trip_and_auth() {
    Harness harness;
    const std::string token = "0123456789abcdef0123456789abcdef";
    const std::string body =
        R"({"jsonrpc":"2.0","id":"r1","method":"System.getInfo"})";

    // Valid request.
    const HttpResponse ok = harness.round_trip(
        [&] { return http_request(harness.transport.port(), rpc_request(token, body)); });
    CHECK(ok.status == 200);
    const JsonValue parsed = parse_body(ok);
    CHECK(parsed.find("result")->find("instanceId")->is_string());
    CHECK(parsed.find("id")->as_string() == "r1");

    // Missing token.
    const HttpResponse unauthorized = harness.round_trip([&] {
        return http_request(harness.transport.port(), rpc_request(token, body, "127.0.0.1", false));
    });
    CHECK(unauthorized.status == 401);
    CHECK(!harness.transport.describe().empty());
    CHECK(harness.transport.describe().find(token) == std::string::npos);

    // Non-loopback Host is refused before auth and never reaches the runtime.
    const HttpResponse forbidden = harness.round_trip([&] {
        return http_request(harness.transport.port(), rpc_request(token, body, "evil.example"));
    });
    CHECK(forbidden.status == 403);

    // Invalid JSON still gets a JSON-RPC parse error with a null id.
    const HttpResponse malformed = harness.round_trip([&] {
        return http_request(harness.transport.port(), rpc_request(token, "not json"));
    });
    CHECK(malformed.status == 200);
    CHECK(parse_body(malformed).find("error")->find("code")->as_integer() == -32700);
}

void test_auth_lockout_and_framing() {
    Harness harness;
    const std::string token = "0123456789abcdef0123456789abcdef";
    const std::string body =
        R"({"jsonrpc":"2.0","id":"r1","method":"System.getInfo"})";

    // Framing rejections: chunked encoding and duplicate Content-Length.
    std::string chunked = "POST /automation/v1/rpc HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                          "Authorization: Bearer " +
                          token + "\r\nTransfer-Encoding: chunked\r\n\r\n";
    const HttpResponse chunked_response =
        http_request(harness.transport.port(), chunked);
    CHECK(chunked_response.status == 400);

    std::string duplicate = "POST /automation/v1/rpc HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                            "Authorization: Bearer " +
                            token + "\r\nContent-Length: 4\r\nContent-Length: 4\r\n\r\n";
    duplicate += body;
    const HttpResponse duplicate_response =
        http_request(harness.transport.port(), duplicate);
    CHECK(duplicate_response.status == 400);

    // Unknown route after auth.
    std::string unknown = "GET /automation/v1/nope HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                          "Authorization: Bearer " +
                          token + "\r\n\r\n";
    const HttpResponse unknown_response = http_request(harness.transport.port(), unknown);
    CHECK(unknown_response.status == 404);

    // Five wrong tokens trigger the lockout; the next attempt is refused
    // with 429 even with the right token. This runs last: the lockout lasts
    // real seconds and would otherwise swallow the checks above.
    for (int i = 0; i < 5; ++i) {
        const HttpResponse response = harness.round_trip([&] {
            return http_request(harness.transport.port(), rpc_request("wrong", body));
        });
        CHECK(response.status == 401);
    }
    const HttpResponse locked = harness.round_trip(
        [&] { return http_request(harness.transport.port(), rpc_request(token, body)); });
    CHECK(locked.status == 429);
}

void test_schema_and_artifact_routes() {
    Harness harness;
    const std::string token = "0123456789abcdef0123456789abcdef";

    std::string schema = "GET /automation/v1/schema HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                         "Authorization: Bearer " +
                         token + "\r\n\r\n";
    const HttpResponse schema_response = harness.round_trip(
        [&] { return http_request(harness.transport.port(), schema); });
    CHECK(schema_response.status == 200);
    const JsonValue document = parse_body(schema_response);
    CHECK(document.find("protocolVersion")->as_string() == "1.0");
    bool saw_system = false;
    for (const JsonValue& method : document.find("methods")->items()) {
        if (method.find("name")->as_string() == "System.getInfo") saw_system = true;
    }
    CHECK(saw_system);

    // Artifact bytes route with an ETag.
    const std::vector<std::uint8_t> bytes{0x89, 'P', 'N', 'G', 1, 2, 3, 4};
    const std::string artifact = harness.runtime.artifacts().store(
        bytes, "image/png", JsonValue::object(), harness.clock.now_value);
    CHECK(!artifact.empty());
    std::string get_artifact = "GET /automation/v1/artifacts/" + artifact +
                               " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                               "Authorization: Bearer " +
                               token + "\r\n\r\n";
    const HttpResponse artifact_response = harness.round_trip(
        [&] { return http_request(harness.transport.port(), get_artifact); });
    CHECK(artifact_response.status == 200);
    if (artifact_response.body != std::string(bytes.begin(), bytes.end())) {
        std::fprintf(stderr, "artifact body (%zu): ", artifact_response.body.size());
        for (const unsigned char c : artifact_response.body)
            std::fprintf(stderr, "%02x ", c);
        std::fprintf(stderr, "\n");
    }
    CHECK(artifact_response.body == std::string(bytes.begin(), bytes.end()));

    std::string missing = "GET /automation/v1/artifacts/a999 HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                          "Authorization: Bearer " +
                          token + "\r\n\r\n";
    const HttpResponse missing_response = harness.round_trip(
        [&] { return http_request(harness.transport.port(), missing); });
    CHECK(missing_response.status == 404);

    // Oversized bodies are refused before reaching the runtime.
    std::string huge_body(300 * 1024, 'x');
    std::string oversized = "POST /automation/v1/rpc HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                            "Authorization: Bearer " +
                            token + "\r\nContent-Type: application/json\r\nContent-Length: " +
                            std::to_string(huge_body.size()) + "\r\n\r\n";
    const HttpResponse oversized_response =
        http_request(harness.transport.port(), oversized);
    CHECK(oversized_response.status == 413);
}

void test_response_deadline() {
    // A handler that never settles: the transport must answer within its own
    // deadline instead of hanging the client.
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0", "deadline-test"}, std::ref(clock)};
    TransportConfig config;
    config.token = "tok";
    config.response_timeout_s = 0.3;
    AutomationTransport transport{runtime, config};
    CHECK(transport.start());
    std::vector<automation::MethodDescriptor> methods;
    automation::MethodDescriptor descriptor;
    descriptor.name = "app.hang.work";
    descriptor.summary = "never settles";
    JsonValue params = JsonValue::object();
    params.set("type", "object");
    params.set("properties", JsonValue::object());
    descriptor.params = std::move(params);
    const auto registration = runtime.register_methods(
        "app.hang", {descriptor}, [](const automation::RequestContext&, automation::CompletionSink) {
            // Defer forever: the scheduler, not the transport, owns the
            // request; the transport still answers its client.
        });
    CHECK(registration.valid());

    std::atomic<bool> done{false};
    HttpResponse response;
    const std::string hang_body = R"({"jsonrpc":"2.0","id":"h1","method":"app.hang.work"})";
    std::thread client([&] {
        response = http_request(
            transport.port(),
            "POST /automation/v1/rpc HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Authorization: Bearer tok\r\nContent-Type: application/json\r\n"
            "Content-Length: " + std::to_string(hang_body.size()) + "\r\n\r\n" + hang_body);
        done.store(true);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done.load()) {
        runtime.drain(clock.now_value);
        clock.now_value += 0.001;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    client.join();
    if (response.status != 503) {
        std::fprintf(stderr, "deadline status=%d body=%.200s\n", response.status,
                     response.body.c_str());
    }
    CHECK(response.status == 503);

    // Stop is bounded and idempotent; the runtime keeps serving afterwards.
    transport.stop();
    transport.stop();
    CHECK(!transport.running());
}

void test_bind_conflict() {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0", "bind-test"}, std::ref(clock)};
    TransportConfig first_config;
    first_config.token = "tok";
    AutomationTransport first{runtime, first_config};
    CHECK(first.start());
    CHECK(first.port() != 0);

    // Binding the exact port a second time fails instead of silently
    // listening elsewhere.
    TransportConfig second_config;
    second_config.token = "tok";
    second_config.port = first.port();
    AutomationTransport second{runtime, second_config};
    CHECK(!second.start());

    // An empty token refuses to start.
    TransportConfig no_token;
    AutomationTransport third{runtime, no_token};
    CHECK(!third.start());
    first.stop();
}

std::string rpc_request_session(const std::string& token, const std::string& body,
                               const std::string& session) {
    std::string request = "POST /automation/v1/rpc HTTP/1.1\r\n";
    request += "Host: 127.0.0.1\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "Authorization: Bearer " + token + "\r\n";
    request += "X-TabEngine-Session: " + session + "\r\n";
    request += "\r\n";
    request += body;
    return request;
}

// Logical sessions: one stable connection identity across short-lived HTTP
// requests, with explicit begin/end and 404 after the end. Without a
// session header each request is its own ephemeral connection (the bridge
// end-to-end coverage lives in the bridge test).
void test_sessions_begin_use_end() {
    Harness harness;
    const std::string token = "0123456789abcdef0123456789abcdef";
    const std::string body = R"({"jsonrpc":"2.0","id":"s1","method":"System.getInfo"})";

    const HttpResponse begin = harness.round_trip([&] {
        return http_request(
            harness.transport.port(),
            "POST /automation/v1/session HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Authorization: Bearer " +
                token + "\r\n\r\n");
    });
    CHECK(begin.status == 200);
    const JsonValue session_doc = parse_body(begin);
    const std::string session = session_doc.find("sessionId")->as_string();
    CHECK(session.size() >= 32);

    // Three requests share one logical connection; no session conflicts.
    for (int i = 0; i < 3; ++i) {
        const HttpResponse ok = harness.round_trip([&] {
            return http_request(harness.transport.port(),
                                rpc_request_session(token, body, session));
        });
        CHECK(ok.status == 200);
        CHECK(parse_body(ok).find("id")->as_string() == "s1");
    }

    // An unknown session id is refused (auth already checked).
    const HttpResponse unknown = harness.round_trip([&] {
        return http_request(harness.transport.port(),
                            rpc_request_session(token, body, "deadbeefdeadbeef"));
    });
    CHECK(unknown.status == 404);

    // Explicit end: the same id is no longer valid afterwards.
    const HttpResponse end = harness.round_trip([&] {
        return http_request(
            harness.transport.port(),
            "DELETE /automation/v1/session HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Authorization: Bearer " +
                token + "\r\nX-TabEngine-Session: " + session + "\r\n\r\n");
    });
    CHECK(end.status == 200);
    const HttpResponse after_end = harness.round_trip([&] {
        return http_request(harness.transport.port(),
                            rpc_request_session(token, body, session));
    });
    CHECK(after_end.status == 404);
}

// Sessions idle out even when the client never returns, and the session
// table is bounded.
void test_session_idle_expiry_and_limit() {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0", "session-test"}, std::ref(clock)};
    TransportConfig config;
    config.token = "tok";
    config.session_idle_timeout_s = 0.4;
    config.max_sessions = 2;
    AutomationTransport transport{runtime, config};
    CHECK(transport.start());

    const auto begin_session = [&] {
        std::string request = "POST /automation/v1/session HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                              "Authorization: Bearer tok\r\n\r\n";
        return http_request(transport.port(), request);
    };
    const HttpResponse first = begin_session();
    CHECK(first.status == 200);

    // Idle past the timeout (the accept loop sweeps every 250 ms).
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    const std::string session = parse_body(first).find("sessionId")->as_string();
    const std::string body = R"({"jsonrpc":"2.0","id":"s","method":"System.getInfo"})";
    const HttpResponse expired = http_request(
        transport.port(), rpc_request_session("tok", body, session));
    CHECK(expired.status == 404);

    // The session cap refuses a third concurrent session.
    CHECK(begin_session().status == 200);
    CHECK(begin_session().status == 200);
    const HttpResponse capped = begin_session();
    CHECK(capped.status == 429);

    transport.stop();
}

// The transport response timeout must cancel the runtime request: a queued
// mutating method never executes after the client already saw a 503
// (regression: the request used to keep its full deadline and run later).
void test_response_timeout_cancels_side_effects() {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0", "cancel-test"}, std::ref(clock)};
    TransportConfig config;
    config.token = "tok";
    config.response_timeout_s = 0.3;
    AutomationTransport transport{runtime, config};
    CHECK(transport.start());

    std::atomic<int> mutations{0};
    std::vector<automation::MethodDescriptor> methods;
    automation::MethodDescriptor descriptor;
    descriptor.name = "app.review.mutate";
    descriptor.summary = "counts executions";
    JsonValue params = JsonValue::object();
    params.set("type", "object");
    params.set("properties", JsonValue::object());
    descriptor.params = std::move(params);
    const auto registration =
        runtime.register_methods("app.review", {descriptor},
                                 [&](const automation::RequestContext&,
                                     automation::CompletionSink sink) {
                                     ++mutations;
                                     sink(automation::MethodOutcome::success(
                                         JsonValue::object()));
                                 });
    CHECK(registration.valid());

    // UI never drains: the request stays queued while the transport waits.
    const std::string body = R"({"jsonrpc":"2.0","id":"m","method":"app.review.mutate"})";
    const HttpResponse response = http_request(
        transport.port(),
        "POST /automation/v1/rpc HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Authorization: Bearer tok\r\nContent-Type: application/json\r\n"
        "Content-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body);
    CHECK(response.status == 503);
    CHECK(mutations.load() == 0);

    // Draining later must NOT run the cancelled request.
    runtime.drain(clock.now_value + 10.0);
    CHECK(mutations.load() == 0);
    transport.stop();
}

// Artifact downloads go through the same session authorization and observe
// grant as every RPC (regression: the byte route used to bypass policy).
void test_artifact_route_uses_policy_and_grants() {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0", "policy-test"}, std::ref(clock)};

    struct VariablePolicy : automation::IAutomationPolicy {
        std::atomic<int> mode{0};   // 0 deny session, 1 no observe grant, 2 allow
        bool authorize_session(const automation::SessionInfo&) override {
            return mode.load() != 0;
        }
        [[nodiscard]] std::vector<std::string> grants(
            const automation::SessionInfo&) override {
            if (mode.load() == 1) return {std::string(automation::kGrantInput)};
            return {std::string(automation::kGrantObserve),
                    std::string(automation::kGrantInput),
                    std::string(automation::kGrantWindowTab)};
        }
    };
    VariablePolicy policy;
    runtime.set_policy(&policy);

    TransportConfig config;
    config.token = "tok";
    AutomationTransport transport{runtime, config};
    CHECK(transport.start());

    // Pump drains on this thread while one request runs on a client thread.
    const auto pump = [&](const std::function<HttpResponse()>& request) {
        HttpResponse response;
        std::atomic<bool> done{false};
        std::thread client([&] {
            response = request();
            done.store(true);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done.load()) {
            runtime.drain(clock.now_value);
            clock.now_value += 0.001;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CHECK(std::chrono::steady_clock::now() < deadline);
        }
        client.join();
        return response;
    };

    const std::vector<std::uint8_t> bytes{0x89, 'P', 'N', 'G', 9, 9, 9, 9};
    const std::string artifact =
        runtime.artifacts().store(bytes, "image/png", JsonValue::object(),
                                  clock.now_value);
    CHECK(!artifact.empty());

    const auto get_artifact = [&] {
        return http_request(transport.port(),
                            "GET /automation/v1/artifacts/" + artifact +
                                " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                "Authorization: Bearer tok\r\n\r\n");
    };
    const std::string rpc = R"({"jsonrpc":"2.0","id":"p","method":"System.getInfo"})";
    const auto rpc_call = [&] {
        return http_request(transport.port(),
                            "POST /automation/v1/rpc HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                            "Authorization: Bearer tok\r\nContent-Type: application/json\r\n"
                            "Content-Length: " +
                                std::to_string(rpc.size()) + "\r\n\r\n" + rpc);
    };

    // Denied session: the RPC reports PermissionDenied and the artifact
    // byte route answers 403 - no content leak past the policy.
    policy.mode.store(0);
    const HttpResponse denied_rpc = pump(rpc_call);
    CHECK(denied_rpc.status == 200);   // JSON-RPC errors travel as 200
    CHECK(parse_body(denied_rpc).find("error")->find("code")->as_integer() == 1001);
    CHECK(pump(get_artifact).status == 403);

    // Authorized session without the observe grant: same refusal on both
    // surfaces.
    policy.mode.store(1);
    const HttpResponse ungranted_rpc = pump(rpc_call);
    CHECK(parse_body(ungranted_rpc).find("error")->find("code")->as_integer() == 1001);
    CHECK(pump(get_artifact).status == 403);

    // Full grants: metadata and bytes both flow.
    policy.mode.store(2);
    const HttpResponse allowed_rpc = pump(rpc_call);
    CHECK(parse_body(allowed_rpc).find("result") != nullptr);
    const HttpResponse bytes_response = pump(get_artifact);
    CHECK(bytes_response.status == 200);
    CHECK(bytes_response.body == std::string(bytes.begin(), bytes.end()));

    transport.stop();
}

// TTL is enforced on the read path with the runtime's clock: an artifact
// that expired while the app was idle is not served (regression: expiry
// only ran on drains).
void test_artifact_ttl_expires_without_drain() {
    automation::RuntimeBudgets budgets;
    budgets.artifact_ttl_s = 0.5;
    Clock clock;
    AutomationRuntime runtime{budgets, {"1.0", "0.1.0", "ttl-test"}, std::ref(clock)};
    TransportConfig config;
    config.token = "tok";
    AutomationTransport transport{runtime, config};
    CHECK(transport.start());

    const std::vector<std::uint8_t> bytes{1, 2, 3, 4, 5};
    const std::string artifact =
        runtime.artifacts().store(bytes, "application/octet-stream", JsonValue::object(),
                                  clock.now_value);
    CHECK(!artifact.empty());

    // Read-side check with an explicit advanced time (no drain involved).
    automation::ArtifactStore::Entry entry;
    CHECK(runtime.artifacts().get(artifact, entry, clock.now_value));
    clock.now_value += 2.0;   // well past the TTL, no expire() call
    CHECK(!runtime.artifacts().get(artifact, entry, clock.now_value));
    JsonValue metadata;
    CHECK(!runtime.artifacts().metadata(artifact, metadata, clock.now_value));

    // And over HTTP: 404 for the expired id (the synthesized Artifact.get
    // request must run on a UI drain, like every other request).
    std::atomic<bool> done{false};
    HttpResponse stale;
    std::thread client([&] {
        stale = http_request(
            transport.port(),
            "GET /automation/v1/artifacts/" + artifact +
                " HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer tok\r\n\r\n");
        done.store(true);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done.load()) {
        runtime.drain(clock.now_value);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    client.join();
    CHECK(stale.status == 404);
    transport.stop();
}

// A trickling client cannot extend its own I/O deadline, other connections
// keep working meanwhile, and stop() joins within a bounded delay without
// cross-thread socket writes.
void test_slow_client_deadline_and_bounded_stop() {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0", "trickle-test"}, std::ref(clock)};
    TransportConfig config;
    config.token = "tok";
    config.io_timeout_s = 1.0;
    AutomationTransport transport{runtime, config};
    CHECK(transport.start());

    // Trickle: a few header bytes every 200 ms. Under the old per-call
    // timeout reset this could extend forever; with an absolute deadline
    // the worker gives up after io_timeout_s.
    SOCKET trickle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(transport.port());
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    CHECK(connect(trickle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    const char* part = "POST /automation/v1/rpc HTTP/1.1\r\nHost: 12";
    (void)send(trickle, part, static_cast<int>(std::strlen(part)), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Other clients are served while the trickle connection dangles.
    const std::string body = R"({"jsonrpc":"2.0","id":"t","method":"System.getInfo"})";
    std::atomic<bool> done{false};
    HttpResponse other;
    std::thread client([&] {
        other = http_request(
            transport.port(),
            "POST /automation/v1/rpc HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Authorization: Bearer tok\r\nContent-Type: application/json\r\n"
            "Content-Length: " +
                std::to_string(body.size()) + "\r\n\r\n" + body);
        done.store(true);
    });
    while (!done.load()) {
        runtime.drain(clock.now_value);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    client.join();
    CHECK(other.status == 200);

    // The trickle connection is dropped by its absolute deadline (worker
    // closes; the client sees EOF).
    const auto drop_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(4);
    char scratch[16];
    while (recv(trickle, scratch, sizeof(scratch), 0) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(std::chrono::steady_clock::now() < drop_deadline);
    }
    closesocket(trickle);

    // stop() with a fresh dangling connection joins quickly.
    SOCKET dangling = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(connect(dangling, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    (void)send(dangling, "GET /aut", 8, 0);
    const auto stop_started = std::chrono::steady_clock::now();
    transport.stop();
    const double stop_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - stop_started)
            .count();
    if (stop_seconds >= 5.0) {
        std::fprintf(stderr, "stop took %.2fs with a slow client connected\n", stop_seconds);
    }
    CHECK(stop_seconds < 5.0);
    closesocket(dangling);
}

// Host-mounted routes: same token and framing, static replies, relayed
// JSON-RPC through the runtime, declines falling through to 404, and the
// auth-failure audit hook.
void test_extra_routes_and_auth_callback() {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0", "extra-routes"}, std::ref(clock)};
    const std::string runtime_artifact_note =
        runtime.artifacts().store(std::vector<std::uint8_t>{9, 8, 7}, "application/octet-stream",
                                  JsonValue::object(), clock.now_value);
    TransportConfig config;
    config.token = "tok";
    config.response_timeout_s = 2.0;
    AutomationTransport transport{runtime, config};
    std::atomic<std::uint64_t> seen_failures{0};
    std::atomic<bool> seen_locked{false};
    transport.set_auth_failure_callback([&](std::uint64_t failures, bool locked) {
        seen_failures.store(failures);
        seen_locked.store(locked || seen_locked.load());
    });
    transport.set_route_handler(
        [note = runtime_artifact_note](
            const AutomationTransport::RouteRequest& request,
            const AutomationTransport::RpcCall& call,
            const AutomationTransport::ArtifactReader& artifacts)
            -> AutomationTransport::RouteReply {
            if (request.method == "GET" && request.path == "/health") {
                return {200, "application/json", R"({"ok":true})"};
            }
            if (request.method == "POST" && request.path == "/relay") {
                const std::string envelope =
                    call(R"({"jsonrpc":"2.0","id":"x","method":"System.getInfo"})", 2.0);
                if (envelope.empty()) {
                    return {503, "application/json", R"({"error":"no response"})"};
                }
                return {200, "application/json", envelope};
            }
            if (request.method == "GET" && request.path == "/bytes") {
                // Artifact bytes flow to host routes after a capture.
                std::string bytes;
                std::string media_type;
                if (!artifacts(note, bytes, media_type)) return {};
                return {200, media_type, bytes};
            }
            return {};   // decline everything else
        });
    CHECK(transport.start());

    const auto pump = [&](const std::function<HttpResponse()>& request) {
        HttpResponse response;
        std::atomic<bool> done{false};
        std::thread client([&] {
            response = request();
            done.store(true);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done.load()) {
            runtime.drain(clock.now_value);
            clock.now_value += 0.001;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CHECK(std::chrono::steady_clock::now() < deadline);
        }
        client.join();
        return response;
    };

    // Static host route answers with its own body.
    const HttpResponse health = pump([&] {
        return http_request(transport.port(),
                            "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                            "Authorization: Bearer tok\r\n\r\n");
    });
    CHECK(health.status == 200);
    CHECK(health.body == R"({"ok":true})");

    // Relayed route reaches the runtime through the same queue.
    const HttpResponse relay = pump([&] {
        return http_request(transport.port(),
                            "POST /relay HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                            "Authorization: Bearer tok\r\nContent-Length: 0\r\n\r\n");
    });
    CHECK(relay.status == 200);
    {
        JsonValue envelope;
        CHECK(parse_json_strict(relay.body, 65536, envelope).status ==
              automation::JsonParseStatus::Ok);
        const JsonValue* result = envelope.find("result");
        CHECK(result != nullptr);
        CHECK(result->find("instanceId")->is_string());
    }

    // Artifact bytes are readable from host routes.
    const HttpResponse bytes = pump([&] {
        return http_request(transport.port(),
                            "GET /bytes HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                            "Authorization: Bearer tok\r\n\r\n");
    });
    CHECK(bytes.status == 200);
    CHECK(bytes.body == std::string({static_cast<char>(9), static_cast<char>(8),
                                     static_cast<char>(7)}));

    // Declined host routes answer the transport's 404.
    const HttpResponse declined = pump([&] {
        return http_request(transport.port(),
                            "GET /declined HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                            "Authorization: Bearer tok\r\n\r\n");
    });
    CHECK(declined.status == 404);

    // The same surface refuses bad tokens and reports to the audit hook.
    const HttpResponse denied = http_request(
        transport.port(),
        "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer bad\r\n\r\n");
    CHECK(denied.status == 401);
    CHECK(seen_failures.load() == 1);

    // Engine routes keep working alongside the mounted ones.
    const HttpResponse rpc = pump([&] {
        return http_request(transport.port(), rpc_request("tok",
            R"({"jsonrpc":"2.0","id":1,"method":"System.getInfo"})"));
    });
    CHECK(rpc.status == 200);

    transport.stop();
}

} // namespace

int main() {
    test_rpc_round_trip_and_auth();
    test_auth_lockout_and_framing();
    test_schema_and_artifact_routes();
    test_response_deadline();
    test_bind_conflict();
    test_sessions_begin_use_end();
    test_session_idle_expiry_and_limit();
    test_response_timeout_cancels_side_effects();
    test_artifact_route_uses_policy_and_grants();
    test_artifact_ttl_expires_without_drain();
    test_slow_client_deadline_and_bounded_stop();
    test_extra_routes_and_auth_callback();
    return 0;
}
