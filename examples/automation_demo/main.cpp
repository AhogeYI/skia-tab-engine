// TabEngine automation demo: a minimal application built ONLY against the
// SDK that embeds the engine, registers its own automation domain,
// providers and policy, and exposes the loopback transport so external
// tools can drive it.
//
// It paints one button, one text editor and one async task - no RML, no
// file manager, no product code - and it can verify itself end to end:
//
//   tabengine_automation_demo.exe --self-check
//
// opens a hidden raster window, starts the transport on an OS-assigned
// port and performs six real JSON-RPC calls against itself (info, semantic
// snapshot, semantic node click, application state, condition wait,
// capture) plus one artifact fetch with PNG validation, then prints a JSON
// summary and exits 0.

#include "tabengine/automation/runtime.h"
#include "tabengine/automation/shell_bridge.h"
#include "tabengine/automation/transport.h"
#include "tabengine/render.h"
#include "tabengine/shell.h"
#include "tabengine/text.h"
#include "tabengine/win32_platform.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace automation = tabengine::automation;

namespace {

constexpr SkColor kPanel = 0xFFF4F6FA;
constexpr SkColor kInk = 0xFF162238;
constexpr SkColor kMuted = 0xFF6B7280;
constexpr SkColor kAccent = 0xFF2F6FED;
constexpr SkColor kButton = 0xFF1F2937;

struct Layout {
    tabengine::Rect button;
    tabengine::Rect edit;
    tabengine::Rect status;
};

Layout body_layout(tabengine::Rect body) {
    Layout layout;
    layout.button = {body.x + 24, body.y + 24, 160, 36};
    layout.edit = {body.x + 24, body.y + 76, std::max(120, body.width - 48), 32};
    layout.status = {body.x + 24, body.y + 124, std::max(120, body.width - 48), 24};
    return layout;
}

struct ExampleState {
    std::function<double()> clock;
    std::string text;
    int completed_tasks = 0;
    bool task_running = false;
    double task_ends_at = 0.0;

    void update() {
        if (task_running && clock() >= task_ends_at) {
            task_running = false;
            ++completed_tasks;
        }
    }
    [[nodiscard]] double pending_since() const { return task_ends_at; }
};

class ExampleClient final : public tabengine::IClient {
public:
    explicit ExampleClient(ExampleState& state) : state_(state) {}

    tabengine::NewTab create_tab() override {
        const tabengine::ContentId content = next_content_++;
        return {content, "Example " + std::to_string(content)};
    }
    std::string window_title() override { return "Automation Demo"; }
    void tab_closed(tabengine::ContentId) override {}
    void body_geometry_changed(tabengine::WindowId, tabengine::Rect body, float) override {
        body_bounds_ = body;
    }

    void paint_body(tabengine::WindowId, tabengine::TabId, SkCanvas& canvas,
                    tabengine::Rect body) override {
        state_.update();
        body_bounds_ = body;
        canvas.clear(kPanel);
        const Layout layout = body_layout(body);
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setColor(kButton);
        canvas.drawRoundRect(SkRect::MakeXYWH(
                                 static_cast<float>(layout.button.x),
                                 static_cast<float>(layout.button.y),
                                 static_cast<float>(layout.button.width),
                                 static_cast<float>(layout.button.height)),
                             6, 6, paint);
        tabengine::paint_ui_text(canvas, "Run task",
                                 static_cast<float>(layout.button.x + 46),
                                 static_cast<float>(layout.button.y + 24), 15.0f,
                                 0xFFF9FAFB);
        paint.setColor(0xFFFFFFFF);
        canvas.drawRect(SkRect::MakeXYWH(
                            static_cast<float>(layout.edit.x),
                            static_cast<float>(layout.edit.y),
                            static_cast<float>(layout.edit.width),
                            static_cast<float>(layout.edit.height)),
                        paint);
        tabengine::paint_ui_text(canvas, state_.text.empty() ? "type here" : state_.text,
                                 static_cast<float>(layout.edit.x + 8),
                                 static_cast<float>(layout.edit.y + 22), 15.0f,
                                 state_.text.empty() ? kMuted : kInk);
        const std::string status =
            state_.task_running
                ? "task running..."
                : "completed tasks: " + std::to_string(state_.completed_tasks);
        tabengine::paint_ui_text(canvas, status,
                                 static_cast<float>(layout.status.x),
                                 static_cast<float>(layout.status.y + 16), 13.0f,
                                 state_.task_running ? kAccent : kMuted);
    }

    void body_event(const tabengine::Event& event, tabengine::Rect body) override {
        state_.update();
        body_bounds_ = body;
        const Layout layout = body_layout(body);
        if (event.type == tabengine::EventType::PointerDown &&
            event.button == tabengine::PointerButton::Left) {
            if (layout.button.contains(event.client)) {
                // The button click and the automation method start the
                // same tracked task (set from main once the runtime
                // exists); the fallback keeps the client usable alone.
                if (start_task_) {
                    start_task_(0.6);
                } else {
                    state_.task_running = true;
                    state_.task_ends_at = state_.clock() + 0.6;
                }
            } else if (layout.edit.contains(event.client)) {
                focused_ = true;
            } else {
                focused_ = false;
            }
            return;
        }
        if (!focused_) return;
        if (event.type == tabengine::EventType::TextInput && event.code_point >= 0x20) {
            const char32_t code_point = event.code_point;
            std::string encoded;
            if (code_point < 0x80) {
                encoded.push_back(static_cast<char>(code_point));
            } else if (code_point < 0x800) {
                encoded.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
                encoded.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
            } else if (code_point < 0x10000) {
                encoded.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
                encoded.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
                encoded.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
            } else {
                encoded.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
                encoded.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
                encoded.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
                encoded.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
            }
            if (state_.text.size() < 64) state_.text += encoded;
            return;
        }
        if (event.type == tabengine::EventType::KeyDown && event.key == 8 &&
            !state_.text.empty()) {
            state_.text.pop_back();
        }
    }

    [[nodiscard]] tabengine::Rect body_bounds() const { return body_bounds_; }

    // Set from main once the automation runtime exists: button clicks and
    // the app.example.runTask method start the same operation-tracked task.
    void set_task_starter(std::function<void(double)> starter) {
        start_task_ = std::move(starter);
    }

private:
    ExampleState& state_;
    tabengine::ContentId next_content_ = 1;
    bool focused_ = false;
    tabengine::Rect body_bounds_{0, 41, 720, 400};
    std::function<void(double)> start_task_;
};

// Semantic provider: the same layout rectangles used for painting and hit
// testing become the automation-facing nodes.
class ExampleSemanticProvider final : public automation::ISemanticProvider {
public:
    explicit ExampleSemanticProvider(ExampleClient& client) : client_(client) {}

    bool provide_semantic(tabengine::WindowId, automation::SemanticSnapshot& out) override {
        const Layout layout = body_layout(client_.body_bounds());
        out.nodes.push_back(node("runTask", "Run task", 1, layout.button,
                                 automation::roles::kButton, {"click"}));
        out.nodes.push_back(node("edit", "Text editor", 2, layout.edit,
                                 automation::roles::kEdit, {"click"}));
        out.nodes.push_back(node("status", "Status line", 3, layout.status,
                                 automation::roles::kText, {}));
        return true;
    }

    bool resolve_node(tabengine::WindowId, std::uint64_t node,
                      automation::SemanticNode& resolved) override {
        if (node < 1 || node > 3) return false;
        automation::SemanticSnapshot snapshot;
        provide_semantic(0, snapshot);
        resolved = snapshot.nodes[static_cast<std::size_t>(node - 1)];
        return true;
    }

private:
    automation::SemanticNode node(std::string stable_key, std::string name, std::uint64_t ref,
                                  tabengine::Rect bounds, std::string_view role_name,
                                  std::vector<std::string> actions) {
        automation::SemanticNode value;
        value.role = std::string(role_name);
        value.name = std::move(name);
        value.stable_key = std::move(stable_key);
        value.node = ref;
        value.bounds = bounds;
        value.clip = bounds;   // fully visible; empty would mean fully clipped
        value.actions = std::move(actions);
        return value;
    }

    ExampleClient& client_;
};

class ExampleActivityProvider final : public automation::IActivityProvider {
public:
    explicit ExampleActivityProvider(ExampleState& state) : state_(state) {}

    bool activity(std::string_view scope, automation::ActivityReport& out) override {
        if (scope != "exampleTasks") return false;
        state_.update();
        out.generation = static_cast<std::uint64_t>(state_.completed_tasks + 1);
        out.pending = state_.task_running ? 1 : 0;
        out.describe = state_.task_running ? "background task" : "";
        return true;
    }

    std::vector<std::string> scopes() const override { return {"exampleTasks"}; }

private:
    ExampleState& state_;
};

class ExampleDiagnosticsProvider final : public automation::IDiagnosticProvider {
public:
    bool read_logs(std::uint64_t after_sequence, std::size_t max,
                   std::vector<automation::LogRecord>& out) override {
        std::size_t written = 0;
        for (const std::string& line : lines_) {
            ++sequence_;
            if (sequence_ <= after_sequence) continue;
            if (written >= max) break;
            automation::LogRecord record;
            record.sequence = sequence_;
            record.at = 0.0;
            record.level = "info";
            record.source = "app.example";
            record.message = line;
            out.push_back(std::move(record));
            ++written;
        }
        return true;
    }

    void add(std::string line) { lines_.push_back(std::move(line)); }

private:
    std::vector<std::string> lines_;
    std::uint64_t sequence_ = 0;
};

// ---- tiny HTTP client used by --self-check only ----

struct HttpResult {
    int status = 0;
    std::string body;
};

HttpResult http_get(std::uint16_t port, const std::string& token, const std::string& path) {
    HttpResult result;
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        WSACleanup();
        return result;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(socket);
        WSACleanup();
        return result;
    }
    const std::string request = "GET " + path +
                                " HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
                                token + "\r\n\r\n";
    send(socket, request.data(), static_cast<int>(request.size()), 0);
    std::string raw;
    char buffer[4096];
    while (true) {
        const int received = recv(socket, buffer, sizeof(buffer), 0);
        if (received <= 0) break;
        raw.append(buffer, static_cast<std::size_t>(received));
    }
    closesocket(socket);
    WSACleanup();
    result.status = std::atoi(raw.substr(9, 3).c_str());
    const std::size_t split = raw.find("\r\n\r\n");
    if (split != std::string::npos) result.body = raw.substr(split + 4);
    return result;
}

HttpResult http_post(std::uint16_t port, const std::string& token, const std::string& body) {
    HttpResult result;
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        WSACleanup();
        return result;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(socket);
        WSACleanup();
        return result;
    }
    const std::string request = "POST /automation/v1/rpc HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                "Content-Type: application/json\r\nAuthorization: Bearer " +
                                token + "\r\nContent-Length: " + std::to_string(body.size()) +
                                "\r\n\r\n" + body;
    send(socket, request.data(), static_cast<int>(request.size()), 0);
    std::string raw;
    char buffer[4096];
    while (true) {
        const int received = recv(socket, buffer, sizeof(buffer), 0);
        if (received <= 0) break;
        raw.append(buffer, static_cast<std::size_t>(received));
    }
    closesocket(socket);
    WSACleanup();
    result.status = std::atoi(raw.substr(9, 3).c_str());
    const std::size_t split = raw.find("\r\n\r\n");
    if (split != std::string::npos) result.body = raw.substr(split + 4);
    return result;
}

std::string json_string_field(const std::string& body, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    const std::size_t start = body.find(needle);
    if (start == std::string::npos) return {};
    const std::size_t value_start = start + needle.size();
    return body.substr(value_start, body.find('"', value_start) - value_start);
}

// Runs on a client thread while the main thread pumps the platform loop.
int run_self_check(std::uint16_t port, const std::string& token, int& steps) {
    auto call = [&](const std::string& method, const std::string& params) {
        steps += 1;
        return http_post(port, token,
                         R"({"jsonrpc":"2.0","id":"self","method":")" + method +
                             R"(","params":)" + params + "}");
    };
    const HttpResult info = call("System.getInfo", "{}");
    if (info.status != 200 || info.body.find("\"instanceId\"") == std::string::npos) {
        std::fprintf(stderr, "self-check: getInfo failed (%d): %.200s\n", info.status,
                     info.body.c_str());
        return 1;
    }
    const HttpResult snapshot = call("UI.snapshot", R"({"windowId":"1"})");
    if (snapshot.body.find("\"runTask\"") == std::string::npos ||
        snapshot.body.find("\"role\":\"button\"") == std::string::npos) {
        std::fprintf(stderr, "self-check: provider nodes missing: %.300s\n",
                     snapshot.body.c_str());
        return 2;
    }
    const HttpResult click = call("Input.click", R"({"windowId":"1","stableKey":"runTask"})");
    if (click.body.find("\"clicked\":1") == std::string::npos) {
        std::fprintf(stderr, "self-check: semantic click failed: %.200s\n",
                     click.body.c_str());
        return 3;
    }
    const HttpResult state = call("app.example.getState", "{}");
    if (state.body.find("\"taskRunning\":true") == std::string::npos) {
        std::fprintf(stderr, "self-check: task did not start: %.200s\n", state.body.c_str());
        return 4;
    }
    const std::string first_operation = json_string_field(state.body, "operationId");
    if (first_operation.empty()) {
        std::fprintf(stderr, "self-check: task has no operation id: %.200s\n",
                     state.body.c_str());
        return 5;
    }
    const HttpResult quiet = call(
        "Wait.condition",
        R"({"condition":"activityQuiet","scope":"exampleTasks","timeoutS":10})");
    if (quiet.body.find("\"satisfied\":true") == std::string::npos) {
        std::fprintf(stderr, "self-check: condition wait failed: %.200s\n",
                     quiet.body.c_str());
        return 6;
    }
    // The async Operation reached its terminal state through the owner's
    // finish call, not through the wait timing out.
    const HttpResult done = call("Operation.get",
                                 R"({"operationId":")" + first_operation + "\"}");
    if (done.body.find("\"state\":\"completed\"") == std::string::npos) {
        std::fprintf(stderr, "self-check: operation terminal state wrong: %.200s\n",
                     done.body.c_str());
        return 7;
    }
    // Custom test action domain round-trip.
    const HttpResult assertion =
        call("test.example.assertState", R"({"completedTasks":1})");
    if (assertion.body.find("\"matches\":true") == std::string::npos) {
        std::fprintf(stderr, "self-check: test action failed: %.200s\n",
                     assertion.body.c_str());
        return 8;
    }
    // Cooperative cancel: request the stop, then observe the cancelled
    // terminal state once the work actually stops.
    const HttpResult long_task =
        call("app.example.runTask", R"({"durationS":30})");
    const std::string second_operation = json_string_field(long_task.body, "operationId");
    if (second_operation.empty() || second_operation == first_operation) {
        std::fprintf(stderr, "self-check: long task did not start: %.200s\n",
                     long_task.body.c_str());
        return 9;
    }
    const HttpResult cancel = call("Operation.cancel",
                                   R"({"operationId":")" + second_operation + "\"}");
    if (cancel.body.find("\"cancellationRequested\":true") == std::string::npos) {
        std::fprintf(stderr, "self-check: cancel not acknowledged: %.200s\n",
                     cancel.body.c_str());
        return 10;
    }
    const HttpResult cancelled = call(
        "Wait.condition",
        R"({"condition":"operationDone","operationId":")" + second_operation +
            R"(","timeoutS":10})");
    if (cancelled.body.find("\"satisfied\":true") == std::string::npos) {
        std::fprintf(stderr, "self-check: cancelled wait failed: %.200s\n",
                     cancelled.body.c_str());
        return 11;
    }
    const HttpResult cancelled_state = call(
        "Operation.get", R"({"operationId":")" + second_operation + "\"}");
    if (cancelled_state.body.find("\"state\":\"cancelled\"") == std::string::npos) {
        std::fprintf(stderr, "self-check: cancelled state wrong: %.200s\n",
                     cancelled_state.body.c_str());
        return 12;
    }
    const HttpResult shot = call("Capture.screenshot", R"({"windowId":"1","frame":"fresh"})");
    const std::string artifact = json_string_field(shot.body, "artifactId");
    if (artifact.empty() || shot.body.find("\"mediaType\":\"image/png\"") == std::string::npos) {
        std::fprintf(stderr, "self-check: capture failed: %.200s\n", shot.body.c_str());
        return 13;
    }
    const HttpResult png =
        http_get(port, token, "/automation/v1/artifacts/" + artifact);
    if (png.status != 200 || png.body.size() < 8 ||
        png.body.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) != 0) {
        std::fprintf(stderr, "self-check: artifact fetch failed (%d, %zu bytes)\n",
                     png.status, png.body.size());
        return 14;
    }
    steps += 1;   // the artifact fetch is HTTP but not JSON-RPC
    std::printf("{\"selfCheck\":\"passed\",\"rpcCalls\":%d,\"httpCalls\":%d,"
                "\"artifact\":\"%s\",\"pngBytes\":%zu}\n",
                steps - 1, steps, artifact.c_str(), png.body.size());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    bool self_check = false;
    bool force_raster = false;
    std::string token = "automation-demo-token";
    std::string descriptor_path;
    std::string token_path;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--self-check") self_check = true;
        else if (argument == "--raster") force_raster = true;
        else if (argument == "--token" && i + 1 < argc) token = argv[++i];
        else if (argument == "--descriptor-out" && i + 1 < argc) descriptor_path = argv[++i];
        else if (argument == "--token-out" && i + 1 < argc) token_path = argv[++i];
        else if (argument == "--help") {
            std::printf("usage: %s [--self-check] [--raster] [--token TOKEN]\n"
                        "          [--descriptor-out FILE] [--token-out FILE]\n",
                        argv[0]);
            return 0;
        }
    }

    auto platform = tabengine::make_win32_platform();
    auto renderer = force_raster ? tabengine::make_skia_raster_renderer()
                                 : tabengine::make_skia_windows_renderer();
    ExampleState state;
    state.clock = [&platform] { return platform->monotonic_seconds(); };

    ExampleClient client(state);
    tabengine::Shell shell(*platform, *renderer, client);

    automation::AutomationRuntime runtime(
        {},
        {"1.0", TABENGINE_DEMO_ENGINE_VERSION, "automation_demo", "1.0",
         "sdk", "demo-" + std::to_string(GetCurrentProcessId())},
        [&platform] { return platform->monotonic_seconds(); });
    automation::ShellAutomationBridge bridge({shell, *platform, *renderer, runtime});
    bridge.set_renderer_backend(force_raster ? "raster" : "d3d12");

    ExampleSemanticProvider semantic(client);
    ExampleActivityProvider activity(state);
    ExampleDiagnosticsProvider diagnostics;
    diagnostics.add("demo started");
    const auto semantic_registration = runtime.register_semantic_provider(&semantic);
    const auto activity_registration = runtime.register_activity_provider(&activity);
    const auto diagnostics_registration = runtime.register_diagnostics_provider(&diagnostics);
    (void)semantic_registration;
    (void)activity_registration;
    (void)diagnostics_registration;

    // Application domain: business actions and state, registered through
    // the public extension path (no engine changes needed). runTask is the
    // reference for an asynchronous Operation: the method returns after
    // starting, the runtime operation stays Running until this app reports
    // the task's own terminal state, and Operation.cancel flows through a
    // cooperative bridge instead of pretending the work stopped.
    struct TaskOperation {
        std::uint64_t id = 0;
        bool cancel_requested = false;
    } task_operation;
    const auto finish_settled_task = [&] {
        // The owner ticks its own work here: the clock may have passed the
        // task's end without any other observer driving state.update().
        state.update();
        if (task_operation.id == 0 || state.task_running) return;
        automation::JsonValue result = automation::JsonValue::object();
        result.set("completedTasks",
                   static_cast<std::int64_t>(state.completed_tasks));
        if (task_operation.cancel_requested) {
            runtime.finish_operation(task_operation.id,
                                     automation::OperationState::Cancelled,
                                     std::move(result), {});
        } else {
            runtime.finish_operation(task_operation.id,
                                     automation::OperationState::Completed,
                                     std::move(result), {});
        }
        task_operation.id = 0;
    };
    // One task at a time; both the button click and the automation method
    // come through here so the runtime operation covers every entry point.
    const auto start_tracked_task = [&](double duration) {
        state.update();
        if (state.task_running) return false;
        if (duration < 0.05) duration = 0.05;
        if (duration > 30.0) duration = 30.0;
        state.task_running = true;
        state.task_ends_at = state.clock() + duration;
        task_operation.cancel_requested = false;
        task_operation.id = runtime.start_operation(
            "app.example.runTask", [&task_operation, &state, &platform] {
                // Cooperative: acknowledge the request and stop the work;
                // the terminal state follows when it actually stops.
                task_operation.cancel_requested = true;
                state.task_ends_at = platform->monotonic_seconds() + 0.05;
                return true;
            });
        diagnostics.add("task started via automation");
        return true;
    };
    client.set_task_starter([&](double duration) {
        (void)start_tracked_task(duration);
    });
    automation::MethodRegistration app_registration;
    {
        automation::JsonValue run_params = automation::JsonValue::object();
        run_params.set("type", "object");
        automation::JsonValue run_properties = automation::JsonValue::object();
        automation::JsonValue duration = automation::JsonValue::object();
        duration.set("type", "number");
        duration.set("description", "Task duration in seconds (0.05..30).");
        duration.set("default", 0.6);
        run_properties.set("durationS", std::move(duration));
        run_params.set("properties", std::move(run_properties));

        std::vector<automation::MethodDescriptor> methods;
        automation::MethodDescriptor run;
        run.name = "app.example.runTask";
        run.summary = "Start the demo's finite background task as an async Operation.";
        run.mutating = true;
        run.params = run_params;
        run.result = automation::JsonValue::object();
        methods.push_back(std::move(run));
        automation::MethodDescriptor get_state;
        get_state.name = "app.example.getState";
        get_state.summary = "Text, task flag, completion counter, live operation.";
        get_state.params = automation::JsonValue::object();
        get_state.result = automation::JsonValue::object();
        methods.push_back(std::move(get_state));
        auto registration = runtime.register_methods(
            "app.example", std::move(methods),
            [&state, &task_operation, &start_tracked_task](
                const automation::RequestContext& context,
                automation::CompletionSink sink) {
                state.update();
                automation::JsonValue result = automation::JsonValue::object();
                if (context.method == "app.example.runTask") {
                    double duration = 0.6;
                    if (const auto* requested = context.params.find("durationS")) {
                        duration = requested->as_number();
                    }
                    if (!start_tracked_task(duration)) {
                        sink(automation::MethodOutcome::failure(
                            automation::Error::make(
                                automation::ErrorCode::InvalidOperationState,
                                "a task is already running")));
                        return;
                    }
                    result.set("started", true);
                    result.set("operationId",
                               automation::id_to_wire(task_operation.id));
                } else {
                    result.set("taskRunning", state.task_running);
                    result.set("completedTasks",
                               static_cast<std::int64_t>(state.completed_tasks));
                    result.set("text", state.text);
                    if (state.task_running && task_operation.id != 0) {
                        result.set("operationId",
                                   automation::id_to_wire(task_operation.id));
                        result.set("cancellationRequested",
                                   task_operation.cancel_requested);
                    }
                }
                sink(automation::MethodOutcome::success(std::move(result)));
            });
        if (!registration.valid()) {
            std::fprintf(stderr, "failed to register app.example\n");
            return 2;
        }
        app_registration = std::move(registration);
    }

    // Custom test action domain: the test.<owner>.* prefix is reserved for
    // exactly this - deterministic assertions over the app's observable
    // state, never business actions. Read-only, no engine knowledge.
    automation::MethodRegistration test_registration;
    {
        automation::JsonValue assert_params = automation::JsonValue::object();
        assert_params.set("type", "object");
        automation::JsonValue assert_properties = automation::JsonValue::object();
        automation::JsonValue completed = automation::JsonValue::object();
        completed.set("type", "integer");
        completed.set("description", "Expected completedTasks count.");
        completed.set("minimum", 0);
        assert_properties.set("completedTasks", std::move(completed));
        automation::JsonValue running = automation::JsonValue::object();
        running.set("type", "boolean");
        running.set("description", "Expected taskRunning flag.");
        assert_properties.set("taskRunning", std::move(running));
        assert_params.set("properties", std::move(assert_properties));

        std::vector<automation::MethodDescriptor> methods;
        automation::MethodDescriptor assert_state;
        assert_state.name = "test.example.assertState";
        assert_state.summary = "Assert the demo's observable task state.";
        assert_state.mutating = false;
        assert_state.params = assert_params;
        assert_state.result = automation::JsonValue::object();
        methods.push_back(std::move(assert_state));
        auto registration = runtime.register_methods(
            "test.example", std::move(methods),
            [&state](const automation::RequestContext& context,
                     automation::CompletionSink sink) {
                state.update();
                bool matches = true;
                if (const auto* completed = context.params.find("completedTasks")) {
                    matches = matches && static_cast<std::int64_t>(
                                              state.completed_tasks) ==
                                              completed->as_integer();
                }
                if (const auto* running = context.params.find("taskRunning")) {
                    matches = matches && state.task_running == running->as_boolean();
                }
                automation::JsonValue result = automation::JsonValue::object();
                result.set("matches", matches);
                if (!matches) {
                    result.set("actualCompletedTasks",
                               static_cast<std::int64_t>(state.completed_tasks));
                    result.set("actualTaskRunning", state.task_running);
                }
                sink(automation::MethodOutcome::success(std::move(result)));
            });
        if (!registration.valid()) {
            std::fprintf(stderr, "failed to register test.example\n");
            return 2;
        }
        test_registration = std::move(registration);
    }

    // Compose wakes: the pump owns the platform wake handler; this app's
    // runtime drains on it, and other subsystems can subscribe alongside.
    automation::UiTaskPump pump;
    pump.bind_platform(*platform);
    runtime.set_wake([&platform] { platform->wake(); });
    runtime.set_deferred_wake(
        [&platform](double delay) { platform->request_delayed_wake(delay); });
    auto drain_subscription = pump.subscribe([&] {
        runtime.drain(platform->monotonic_seconds());
        // The task owner reports its own terminal state: once the demo's
        // task stopped running, the runtime operation settles with it.
        finish_settled_task();
    });

    const tabengine::WindowId window =
        shell.open_window({120, 120, 720, 460}, true, !self_check);
    if (!window) {
        std::fprintf(stderr, "failed to open the window\n");
        return 3;
    }

    automation::TransportConfig transport_config;
    transport_config.token = token;
    automation::AutomationTransport transport(runtime, transport_config);
    if (!transport.start()) {
        std::fprintf(stderr, "failed to start the automation transport\n");
        return 4;
    }
    std::printf("automation endpoint: %s\n", transport.endpoint().c_str());
    std::printf("descriptor: %s\n", transport.describe().c_str());
    std::fflush(stdout);
    // Publish the connection descriptor and the token separately: tools read
    // the descriptor for addressing and fetch the token from the controlled
    // file (never from the descriptor itself).
    if (!descriptor_path.empty()) {
        if (FILE* file = std::fopen(descriptor_path.c_str(), "wb")) {
            const std::string descriptor = transport.describe();
            std::fwrite(descriptor.data(), 1, descriptor.size(), file);
            std::fclose(file);
        }
    }
    if (!token_path.empty()) {
        if (FILE* file = std::fopen(token_path.c_str(), "wb")) {
            std::fwrite(token.data(), 1, token.size(), file);
            std::fclose(file);
        }
    }

    int exit_code = 0;
    if (self_check) {
        int steps = 0;
        int check_result = -1;
        std::thread checker([&] {
            check_result = run_self_check(transport.port(), token, steps);
            // Quit the message loop from the checker thread.
            if (HWND hwnd = static_cast<HWND>(platform->native_handle(window))) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
        });
        platform->run();
        checker.join();
        exit_code = check_result == 0 ? 0 : check_result;
        if (check_result != 0) {
            std::fprintf(stderr, "self-check failed with code %d\n", check_result);
        }
    } else {
        exit_code = platform->run();
    }

    transport.stop();
    runtime.shutdown(platform->monotonic_seconds());
    return exit_code;
}
