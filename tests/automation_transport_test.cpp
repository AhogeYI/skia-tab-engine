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

} // namespace

int main() {
    test_rpc_round_trip_and_auth();
    test_auth_lockout_and_framing();
    test_schema_and_artifact_routes();
    test_response_deadline();
    test_bind_conflict();
    return 0;
}
