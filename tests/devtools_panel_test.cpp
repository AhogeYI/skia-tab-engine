// Devtools panel static serving over the real transport: the shell entry
// is reachable without a token (after the loopback/Origin boundary) while
// every data route still authenticates; asset serving is whitelist-only,
// traversal-proof, bounded, and served no-store with nosniff.

#include "tabengine/automation/devtools.h"
#include "tabengine/automation/transport.h"

#include "check.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

namespace automation = tabengine::automation;
using automation::AutomationRuntime;
using automation::AutomationTransport;
using automation::DevtoolsPanel;
using automation::JsonValue;
using automation::TransportConfig;

namespace {

struct Clock {
    double now_value = 10.0;
    double operator()() const { return now_value; }
};

struct RawResponse {
    int status = 0;
    std::string head;
    std::string body;
};

RawResponse http_raw(std::uint16_t port, const std::string& request) {
    RawResponse response;
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(socket != INVALID_SOCKET);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    CHECK(connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    CHECK(send(socket, request.data(), static_cast<int>(request.size()), 0) ==
          static_cast<int>(request.size()));
    char buffer[8192];
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
    if (body_start != std::string::npos) {
        response.head = raw.substr(0, body_start);
        response.body = raw.substr(body_start + 4);
    }
    return response;
}

std::string get_request(const std::string& path, bool with_token,
                        const std::string& token, const std::string& host = "127.0.0.1") {
    std::string request = "GET " + path + " HTTP/1.1\r\n";
    request += "Host: " + host + "\r\n";
    if (with_token) request += "Authorization: Bearer " + token + "\r\n";
    request += "Connection: close\r\n\r\n";
    return request;
}

std::filesystem::path make_resources() {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("te-devtools-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    auto write = [&dir](const char* name, const std::string& bytes) {
        std::ofstream file(dir / name, std::ios::binary);
        file << bytes;
    };
    write("panel.html", "<!doctype html><title>panel</title><p>shell</p>"
                        "<script type='module' src='{{MOUNT}}/assets/panel.js'></script>");
    write("panel.css", "body{color:#eee}");
    write("panel.js", "import { core } from '{{MOUNT}}/assets/panel_core.js';"
                      "export const panel = true;");
    write("panel_core.js", "export const core = true;");
    write("logo.svg", "<svg xmlns='http://www.w3.org/2000/svg'/>");
    write("secret.txt", "not served");
    return dir;
}

} // namespace

int main() {
    const std::filesystem::path resources = make_resources();
    const std::string token = "0123456789abcdef0123456789abcdef";

    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.3.0", "devtools-test"}, std::ref(clock)};
    AutomationTransport transport{runtime, [&] {
                                      TransportConfig config;
                                      config.token = token;
                                      config.response_timeout_s = 2.0;
                                      return config;
                                  }()};
    DevtoolsPanel panel;
    CHECK(panel.mount(transport, {resources, "/automation/v1/panel"}));
    CHECK(transport.start());

    const auto round_trip = [&](const std::function<RawResponse()>& request) {
        RawResponse response;
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

    // D1: the static entry loads without a token - a browser can open the
    // page; the shell contains no secrets.
    {
        const RawResponse page = round_trip([&] {
            return http_raw(transport.port(),
                            get_request("/automation/v1/panel", false, token));
        });
        CHECK(page.status == 200);
        CHECK(page.body.find("shell") != std::string::npos);
        CHECK(page.head.find("Content-Type: text/html") != std::string::npos);
        CHECK(page.head.find("Cache-Control: no-store") != std::string::npos);
        CHECK(page.head.find("X-Content-Type-Options: nosniff") != std::string::npos);
    }
    // Assets serve with their content types, still without a token.
    {
        const RawResponse script = round_trip([&] {
            return http_raw(transport.port(),
                            get_request("/automation/v1/panel/assets/panel.js", false, token));
        });
        CHECK(script.status == 200);
        CHECK(script.head.find("Content-Type: text/javascript") != std::string::npos);
        CHECK(script.body.find("panel = true") != std::string::npos);
        const RawResponse style = round_trip([&] {
            return http_raw(transport.port(),
                            get_request("/automation/v1/panel/assets/panel.css", false, token));
        });
        CHECK(style.status == 200);
        CHECK(style.head.find("Content-Type: text/css") != std::string::npos);
    }
    // Traversal and non-whitelisted names are refused, not resolved.
    {
        const RawResponse traversal = round_trip([&] {
            return http_raw(transport.port(), get_request(
                                 "/automation/v1/panel/assets/..%2Fsecret.txt", false, token));
        });
        CHECK(traversal.status == 404);
        CHECK(traversal.body.find("secret") == std::string::npos);
        const RawResponse dotdot = round_trip([&] {
            return http_raw(transport.port(), get_request(
                                 "/automation/v1/panel/assets/../panel.html", false, token));
        });
        CHECK(dotdot.status != 200 || dotdot.body.find("shell") == std::string::npos);
        const RawResponse text = round_trip([&] {
            return http_raw(transport.port(),
                            get_request("/automation/v1/panel/assets/secret.txt", false, token));
        });
        CHECK(text.status == 404);
        CHECK(text.body.find("not served") == std::string::npos);
        const RawResponse missing = round_trip([&] {
            return http_raw(transport.port(),
                            get_request("/automation/v1/panel/assets/nope.js", false, token));
        });
        CHECK(missing.status == 404);
    }
    // Only GET serves the public entry; POST falls through to auth.
    {
        std::string request = "POST /automation/v1/panel HTTP/1.1\r\n";
        request += "Host: 127.0.0.1\r\nContent-Length: 0\r\n\r\n";
        const RawResponse post = round_trip([&] { return http_raw(transport.port(), request); });
        CHECK(post.status == 401);
    }
    // Non-loopback Host never reaches the panel (DNS-rebinding posture).
    {
        const RawResponse external = round_trip([&] {
            return http_raw(transport.port(),
                            get_request("/automation/v1/panel", false, token, "evil.example"));
        });
        CHECK(external.status == 403);
    }
    // Data routes still require the token, with or without the panel.
    {
        const RawResponse unauth = round_trip([&] {
            return http_raw(transport.port(),
                            get_request("/automation/v1/schema", false, token));
        });
        CHECK(unauth.status == 401);
        const RawResponse authed = round_trip([&] {
            return http_raw(transport.port(),
                            get_request("/automation/v1/schema", true, token));
        });
        CHECK(authed.status == 200);
        CHECK(authed.body.find("System.getSchema") != std::string::npos);
    }
    // The token never appears in any served byte.
    {
        const RawResponse page = round_trip([&] {
            return http_raw(transport.port(),
                            get_request("/automation/v1/panel", true, token));
        });
        CHECK(page.body.find(token) == std::string::npos);
    }
    // A missing resources root fails the mount instead of serving 404 pages
    // as if they were the panel.
    {
        AutomationTransport other{runtime, [&] {
                                      TransportConfig config;
                                      config.token = token;
                                      return config;
                                  }()};
        DevtoolsPanel unmounted;
        CHECK(!unmounted.mount(other, {resources / "missing", "/automation/v1/panel"}));
    }

    // A custom mount_path serves the same shell and assets relative to the
    // mount (the HTML references its assets relatively, so the page works
    // under any mount without hardcoded route knowledge).
    {
        AutomationTransport custom{runtime, [&] {
                                       TransportConfig config;
                                       config.token = token;
                                       config.response_timeout_s = 2.0;
                                       return config;
                                   }()};
        DevtoolsPanel custom_panel;
        CHECK(custom_panel.mount(custom, {resources, "/panel/custom"}));
        CHECK(custom.start());
        const RawResponse page = round_trip([&] {
            return http_raw(custom.port(),
                            get_request("/panel/custom", false, token));
        });
        CHECK(page.status == 200 && page.body.find("shell") != std::string::npos);
        const RawResponse script = round_trip([&] {
            return http_raw(custom.port(),
                            get_request("/panel/custom/assets/panel.js", false, token));
        });
        CHECK(script.status == 200 &&
              script.head.find("Content-Type: text/javascript") != std::string::npos);
        // The shell rewrites asset references to the actual mount path (the
        // page URL has no trailing slash, so plain relative URLs would
        // resolve outside the assets route).
        CHECK(page.body.find("/panel/custom/assets/panel.js") != std::string::npos);
        CHECK(script.body.find("/panel/custom/assets/panel_core.js") != std::string::npos);
        CHECK(page.body.find("{{MOUNT}}") == std::string::npos);
        CHECK(script.body.find("{{MOUNT}}") == std::string::npos);
        const RawResponse core = round_trip([&] {
            return http_raw(custom.port(),
                            get_request("/panel/custom/assets/panel_core.js", false, token));
        });
        CHECK(core.status == 200 && core.body.find("core = true") != std::string::npos);
        const RawResponse wrong_assets = round_trip([&] {
            return http_raw(custom.port(),
                            get_request("/automation/v1/panel/assets/panel.js", false, token));
        });
        // A path outside the custom mount falls through to normal auth
        // processing: without a token that is a 401, never panel bytes.
        CHECK(wrong_assets.status == 401);
        CHECK(wrong_assets.body.find("panel = true") == std::string::npos);
        custom.stop();
    }

    transport.stop();
    std::error_code ec;
    std::filesystem::remove_all(resources, ec);
    return 0;
}
