// Runtime behavior: envelope validation, registry rules, queueing and
// deadlines, operations, waits, events, sessions and the wake pump.
// Everything runs against a fake clock with manual drains; no I/O.

#include "tabengine/automation/runtime.h"
#include "tabengine/automation/values.h"

#include "check.h"

#include <cstdint>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

void run_automation_values_tests();

namespace automation = tabengine::automation;
using automation::AutomationEvent;
using automation::AutomationRuntime;
using automation::InstanceIdentity;
using automation::JsonValue;
using automation::MethodDescriptor;
using automation::MethodHandler;
using automation::MethodOutcome;
using automation::MethodRegistration;
using automation::RequestContext;
using automation::RuntimeBudgets;
using automation::id_from_wire;

namespace {

struct Clock {
    double now_value = 100.0;
    double operator()() const { return now_value; }
};

struct Responses {
    // transport_request → {connection, payload}
    std::map<std::uint64_t, std::pair<std::uint64_t, std::string>> by_request;

    void install(AutomationRuntime& runtime) {
        runtime.set_response_sink([this](std::uint64_t connection, std::uint64_t request,
                                         std::string payload) {
            by_request[request] = {connection, std::move(payload)};
        });
    }
    [[nodiscard]] bool has(std::uint64_t request) const {
        return by_request.count(request) != 0;
    }
    [[nodiscard]] const std::string& payload(std::uint64_t request) const {
        return by_request.at(request).second;
    }
    [[nodiscard]] JsonValue parsed(std::uint64_t request) const {
        JsonValue value;
        const auto result = automation::parse_json_strict(by_request.at(request).second, 65536,
                                                          value);
        CHECK(result.status == automation::JsonParseStatus::Ok);
        return value;
    }
    [[nodiscard]] std::int64_t error_code(std::uint64_t request) const {
        const JsonValue value = parsed(request);
        const JsonValue* error = value.find("error");
        CHECK(error != nullptr);
        const JsonValue* code = error->find("code");
        CHECK(code != nullptr && code->is_integer());
        return code->as_integer();
    }
    [[nodiscard]] bool has_result(std::uint64_t request) const {
        return parsed(request).find("result") != nullptr;
    }
};

void receive(AutomationRuntime& runtime, std::uint64_t request, const std::string& json) {
    runtime.receive(/*connection=*/7, request, json);
}

MethodDescriptor simple_method(const char* name, const char* summary) {
    MethodDescriptor descriptor;
    descriptor.name = name;
    descriptor.summary = summary;
    JsonValue params = JsonValue::object();
    params.set("type", "object");
    JsonValue properties = JsonValue::object();
    params.set("properties", std::move(properties));
    descriptor.params = std::move(params);
    descriptor.result = JsonValue::object();
    descriptor.result.set("type", "object");
    return descriptor;
}

void test_app_method_round_trip() {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0", "demo"}, std::ref(clock)};
    Responses responses;
    responses.install(runtime);

    bool called = false;
    std::vector<MethodDescriptor> methods;
    methods.push_back(simple_method("app.demo.ping", "answers pong"));
    MethodRegistration registration = runtime.register_methods(
        "app.demo", std::move(methods),
        [&](const RequestContext&, automation::CompletionSink sink) {
            called = true;
            JsonValue result = JsonValue::object();
            result.set("pong", true);
            sink(MethodOutcome::success(std::move(result)));
        });
    CHECK(registration.valid());

    receive(runtime, 1, R"({"jsonrpc":"2.0","id":"r1","method":"app.demo.ping"})");
    CHECK(!responses.has(1));
    runtime.drain(clock.now_value);
    CHECK(called);
    CHECK(responses.has(1));
    const JsonValue response = responses.parsed(1);
    CHECK(response.find("id")->as_string() == "r1");
    CHECK(response.find("result")->find("pong")->as_boolean());
}

void test_envelope_rejections() {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0"}, std::ref(clock)};
    Responses responses;
    responses.install(runtime);

    struct Case {
        std::uint64_t request;
        const char* json;
        std::int64_t code;
    };
    const Case cases[] = {
        {10, "not json", -32700},
        {11, R"([{"jsonrpc":"2.0","id":1,"method":"System.getInfo"}])", -32600},
        {12, R"({"jsonrpc":"1.0","id":1,"method":"System.getInfo"})", -32600},
        {13, R"({"jsonrpc":"2.0","method":"System.getInfo"})", -32600},
        {14, R"({"jsonrpc":"2.0","id":null,"method":"System.getInfo"})", -32600},
        {15, R"({"jsonrpc":"2.0","id":1.5,"method":"System.getInfo"})", -32600},
        {16, R"({"jsonrpc":"2.0","id":1,"method":"System"})", -32600},
        {17, R"({"jsonrpc":"2.0","id":1,"method":"System.getInfo","params":[1]})", -32600},
        {18, R"({"jsonrpc":"2.0","id":1,"method":"System.getInfo","extra":true})", -32600},
        {19, R"({"jsonrpc":"2.0","id":1,"method":"no.such.method"})", -32601},
    };
    for (const Case& probe : cases) {
        receive(runtime, probe.request, probe.json);
        CHECK(responses.has(probe.request));
        CHECK(responses.error_code(probe.request) == probe.code);
    }
}

void test_param_validation_and_grants() {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0"}, std::ref(clock)};
    Responses responses;
    responses.install(runtime);

    // Method that requires an integer count >= 1.
    MethodDescriptor descriptor;
    descriptor.name = "app.demo.count";
    descriptor.summary = "counts";
    JsonValue params = JsonValue::object();
    params.set("type", "object");
    JsonValue properties = JsonValue::object();
    JsonValue count = JsonValue::object();
    count.set("type", "integer");
    count.set("minimum", static_cast<std::int64_t>(1));
    properties.set("count", std::move(count));
    params.set("properties", std::move(properties));
    JsonValue required = JsonValue::array();
    required.push_back(JsonValue("count"));
    params.set("required", std::move(required));
    descriptor.params = std::move(params);
    std::vector<MethodDescriptor> methods;
    methods.push_back(std::move(descriptor));
    MethodRegistration registration = runtime.register_methods(
        "app.demo", std::move(methods),
        [](const RequestContext&, automation::CompletionSink sink) {
            sink(MethodOutcome::success(JsonValue::object()));
        });
    CHECK(registration.valid());

    receive(runtime, 20, R"({"jsonrpc":"2.0","id":1,"method":"app.demo.count","params":{}})");
    runtime.drain(clock.now_value);
    CHECK(responses.error_code(20) == -32602);

    receive(runtime, 21,
            R"({"jsonrpc":"2.0","id":2,"method":"app.demo.count","params":{"count":0}})");
    runtime.drain(clock.now_value);
    CHECK(responses.error_code(21) == -32602);

    receive(runtime, 22,
            R"({"jsonrpc":"2.0","id":3,"method":"app.demo.count","params":{"count":2}})");
    runtime.drain(clock.now_value);
    CHECK(responses.has_result(22));
}

void test_grant_enforcement() {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0"}, std::ref(clock)};
    Responses responses;
    responses.install(runtime);

    std::vector<MethodDescriptor> methods;
    methods.push_back(simple_method("Sample.protected", "needs a special grant"));
    methods.back().grants.push_back("engine.testHooks");
    MethodRegistration registration = runtime.register_engine_methods(
        "Sample", std::move(methods),
        [](const RequestContext&, automation::CompletionSink sink) {
            sink(MethodOutcome::success(JsonValue::object()));
        });
    CHECK(registration.valid());

    receive(runtime, 30, R"({"jsonrpc":"2.0","id":1,"method":"Sample.protected"})");
    runtime.drain(clock.now_value);
    CHECK(responses.error_code(30) == 1001);   // PermissionDenied

    // A policy that grants the missing name lets the same request through.
    struct Policy : automation::IAutomationPolicy {
        std::vector<std::string> grants(const automation::SessionInfo&) override {
            return {std::string(automation::kGrantObserve), std::string(automation::kGrantInput),
                    std::string(automation::kGrantWindowTab), "engine.testHooks"};
        }
    } policy;
    runtime.set_policy(&policy);
    receive(runtime, 31, R"({"jsonrpc":"2.0","id":2,"method":"Sample.protected"})");
    runtime.drain(clock.now_value);
    CHECK(responses.has_result(31));

    // A connection-level refusal keeps every method denied.
    struct Refusing : automation::IAutomationPolicy {
        bool authorize_session(const automation::SessionInfo&) override { return false; }
    } refusing;
    runtime.set_policy(&refusing);
    runtime.receive(8, 32, R"({"jsonrpc":"2.0","id":3,"method":"System.getInfo"})");
    runtime.drain(clock.now_value);
    CHECK(responses.error_code(32) == 1001);
    runtime.set_policy(nullptr);
}

void test_domain_rules() {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0"}, std::ref(clock)};
    std::vector<MethodDescriptor> methods;
    methods.push_back(simple_method("Window.list", "impersonating the engine"));
    MethodRegistration application = runtime.register_methods(
        "Window", std::move(methods), [](const RequestContext&, automation::CompletionSink) {});
    CHECK(!application.valid());

    std::vector<MethodDescriptor> shell;
    shell.push_back(simple_method("app.demo.ok", "fine"));
    MethodRegistration valid = runtime.register_methods(
        "app.demo", std::move(shell), [](const RequestContext&, automation::CompletionSink) {});
    CHECK(valid.valid());

    // Duplicate method name on the engine path is rejected.
    std::vector<MethodDescriptor> again;
    again.push_back(simple_method("app.demo.ok", "duplicate"));
    MethodRegistration duplicate = runtime.register_methods(
        "app.demo", std::move(again), [](const RequestContext&, automation::CompletionSink) {});
    CHECK(!duplicate.valid());

    // Engine path must not claim application namespaces.
    std::vector<MethodDescriptor> engine;
    engine.push_back(simple_method("app.engine.sneaky", "wrong side"));
    MethodRegistration wrong_side = runtime.register_engine_methods(
        "app.engine", std::move(engine), [](const RequestContext&, automation::CompletionSink) {});
    CHECK(!wrong_side.valid());

    CHECK(runtime.has_method("app.demo.ok"));
    CHECK(runtime.has_method("System.getInfo"));
    CHECK(!runtime.has_method("Window.list"));
}

void test_unregistered_provider_returns_unavailable() {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0"}, std::ref(clock)};
    Responses responses;
    responses.install(runtime);
    {
        std::vector<MethodDescriptor> methods;
        methods.push_back(simple_method("app.gone.work", "vanishes"));
        MethodRegistration registration = runtime.register_methods(
            "app.gone", std::move(methods),
            [](const RequestContext&, automation::CompletionSink sink) {
                sink(MethodOutcome::success(JsonValue::object()));
            });
        CHECK(registration.valid());
        receive(runtime, 40, R"({"jsonrpc":"2.0","id":1,"method":"app.gone.work"})");
        // Registration destroyed here, before the request is drained.
    }
    runtime.drain(clock.now_value);
    CHECK(responses.has(40));
    CHECK(responses.error_code(40) == 1008);   // ProviderUnavailable
}

void test_queued_deadline_never_executes() {
    Clock clock;
    RuntimeBudgets budgets;
    AutomationRuntime runtime{budgets, {"1.0", "0.1.0"}, std::ref(clock)};
    Responses responses;
    responses.install(runtime);

    bool executed = false;
    std::vector<MethodDescriptor> methods;
    MethodDescriptor descriptor = simple_method("app.slow.work", "times out queued");
    descriptor.timeout_s = 0.05;
    methods.push_back(std::move(descriptor));
    MethodRegistration registration = runtime.register_methods(
        "app.slow", std::move(methods),
        [&](const RequestContext&, automation::CompletionSink sink) {
            executed = true;
            sink(MethodOutcome::success(JsonValue::object()));
        });
    CHECK(registration.valid());

    receive(runtime, 50, R"({"jsonrpc":"2.0","id":1,"method":"app.slow.work"})");
    clock.now_value += 1.0;   // far past the 0.05s deadline
    runtime.drain(clock.now_value);
    CHECK(!executed);
    CHECK(responses.has(50));
    CHECK(responses.error_code(50) == 1006);   // DeadlineExceeded
}

void test_executing_deadline_and_deferred_success() {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0"}, std::ref(clock)};
    Responses responses;
    responses.install(runtime);

    automation::CompletionSink captured;
    std::vector<MethodDescriptor> methods;
    methods.push_back(simple_method("app.async.work", "defers"));
    MethodRegistration registration = runtime.register_methods(
        "app.async", std::move(methods),
        [&](const RequestContext&, automation::CompletionSink sink) {
            captured = std::move(sink);   // defer settlement
        });
    CHECK(registration.valid());

    receive(runtime, 60, R"({"jsonrpc":"2.0","id":1,"method":"app.async.work"})");
    runtime.drain(clock.now_value);
    CHECK(!responses.has(60));
    CHECK(captured != nullptr);

    // Deferred success is delivered without another drain.
    JsonValue result = JsonValue::object();
    result.set("done", true);
    captured(MethodOutcome::success(result));
    CHECK(responses.has(60));
    CHECK(responses.parsed(60).find("result")->find("done")->as_boolean());

    // Executing timeout: defer again and let the deadline pass.
    receive(runtime, 61, R"({"jsonrpc":"2.0","id":2,"method":"app.async.work"})");
    runtime.drain(clock.now_value);
    clock.now_value += 11.0;   // default deadline is 10s
    runtime.drain(clock.now_value);
    CHECK(responses.has(61));
    CHECK(responses.error_code(61) == 1006);
    (void)registration;
}

void test_operations() {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0"}, std::ref(clock)};
    Responses responses;
    responses.install(runtime);

    std::uint64_t operation = 0;
    std::vector<MethodDescriptor> methods;
    methods.push_back(simple_method("app.job.start", "starts an operation"));
    MethodRegistration registration = runtime.register_methods(
        "app.job", std::move(methods),
        [&](const RequestContext& context, automation::CompletionSink sink) {
            operation = runtime.start_operation("app.job.start", context);
            JsonValue result = JsonValue::object();
            result.set("operationId", automation::id_to_wire(operation));
            sink(MethodOutcome::success(std::move(result)));
        });
    CHECK(registration.valid());

    receive(runtime, 70, R"({"jsonrpc":"2.0","id":1,"method":"app.job.start"})");
    runtime.drain(clock.now_value);
    CHECK(operation != 0);
    const JsonValue start_response = responses.parsed(70);
    CHECK(start_response.find("result")->find("operationId")->is_string());

    // Running state is queryable.
    receive(runtime, 71,
            (R"({"jsonrpc":"2.0","id":2,"method":"Operation.get","params":{"operationId":")" +
             automation::id_to_wire(operation) + R"("}})")
                .c_str());
    runtime.drain(clock.now_value);
    CHECK(responses.parsed(71).find("result")->find("state")->as_string() == "running");

    // Wait on a running operation defers.
    receive(runtime, 72,
            (R"({"jsonrpc":"2.0","id":3,"method":"Operation.wait","params":{"operationId":")" +
             automation::id_to_wire(operation) + R"("}})")
                .c_str());
    runtime.drain(clock.now_value);
    CHECK(!responses.has(72));

    // Complete the operation; the waiter settles on the next drain.
    runtime.finish_operation(operation, automation::OperationState::Completed,
                             JsonValue(std::string("payload")), {});
    clock.now_value += 0.05;   // past the wait's poll interval
    runtime.drain(clock.now_value);
    CHECK(responses.has(72));
    CHECK(responses.parsed(72).find("result")->find("condition")->as_string() ==
          "operationWait");

    // Terminal state stays queryable.
    receive(runtime, 73,
            (R"({"jsonrpc":"2.0","id":4,"method":"Operation.get","params":{"operationId":")" +
             automation::id_to_wire(operation) + R"("}})")
                .c_str());
    runtime.drain(clock.now_value);
    CHECK(responses.parsed(73).find("result")->find("state")->as_string() == "completed");

    // Cancelling a terminal operation is an error.
    receive(runtime, 74,
            (R"({"jsonrpc":"2.0","id":5,"method":"Operation.cancel","params":{"operationId":")" +
             automation::id_to_wire(operation) + R"("}})")
                .c_str());
    runtime.drain(clock.now_value);
    CHECK(responses.error_code(74) == 1013);   // InvalidOperationState

    // Unknown operation id.
    receive(runtime, 75,
            R"({"jsonrpc":"2.0","id":6,"method":"Operation.get","params":{"operationId":"999"}})");
    runtime.drain(clock.now_value);
    CHECK(responses.error_code(75) == 1012);   // OperationNotFound
}

void test_activity_wait() {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0"}, std::ref(clock)};
    Responses responses;
    responses.install(runtime);

    struct Provider : automation::IActivityProvider {
        int pending = 1;
        bool activity(std::string_view scope, automation::ActivityReport& out) override {
            if (scope != "tasks") return false;
            out.generation = 1;
            out.pending = pending;
            out.describe = "fixture";
            return true;
        }
        std::vector<std::string> scopes() const override { return {"tasks"}; }
    } provider;
    const auto registration = runtime.register_activity_provider(&provider);
    CHECK(registration.valid());

    // No pending yet: wait defers.
    receive(runtime, 80,
            R"({"jsonrpc":"2.0","id":1,"method":"Wait.condition","params":{"condition":"activityQuiet","scope":"tasks"}})");
    runtime.drain(clock.now_value);
    CHECK(!responses.has(80));

    // Provider reports quiet: the next drain satisfies the wait.
    provider.pending = 0;
    runtime.drain(clock.now_value + 1.0);
    CHECK(responses.has(80));
    CHECK(responses.parsed(80).find("result")->find("satisfied")->as_boolean());

    // Unknown scope reports Unsupported immediately.
    receive(runtime, 81,
            R"({"jsonrpc":"2.0","id":2,"method":"Wait.condition","params":{"condition":"activityQuiet","scope":"nope"}})");
    runtime.drain(clock.now_value);
    CHECK(responses.error_code(81) == 1000);   // Unsupported
}

void test_wait_unsupported_without_provider() {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0"}, std::ref(clock)};
    Responses responses;
    responses.install(runtime);
    receive(runtime, 90,
            R"({"jsonrpc":"2.0","id":1,"method":"Wait.condition","params":{"condition":"activityQuiet","scope":"tasks"}})");
    runtime.drain(clock.now_value);
    CHECK(responses.error_code(90) == 1000);
    receive(runtime, 91,
            R"({"jsonrpc":"2.0","id":2,"method":"Wait.condition","params":{"condition":"animationSettled","windowId":"1"}})");
    runtime.drain(clock.now_value);
    CHECK(responses.error_code(91) == 1000);
}

void test_events_and_gap() {
    RuntimeBudgets budgets;
    budgets.event_items = 3;
    Clock clock;
    AutomationRuntime runtime{budgets, {"1.0", "0.1.0"}, std::ref(clock)};
    Responses responses;
    responses.install(runtime);

    for (int i = 0; i < 2; ++i) {
        AutomationEvent event;
        event.name = "tab.added";
        event.window = i + 1;
        runtime.emit_event(std::move(event));
    }
    receive(runtime, 100, R"({"jsonrpc":"2.0","id":1,"method":"Event.poll","params":{}})");
    runtime.drain(clock.now_value);
    const JsonValue first = responses.parsed(100);
    CHECK(first.find("result")->find("events")->size() == 2);
    CHECK(first.find("result")->find("gap")->as_boolean() == false);
    const std::int64_t next = first.find("result")->find("next")->as_integer();

    // Overflow the ring: a since-cursor from before the trim reports a gap.
    for (int i = 0; i < 5; ++i) {
        AutomationEvent event;
        event.name = "tab.added";
        event.window = 9;
        runtime.emit_event(std::move(event));
    }
    receive(runtime, 101,
            (R"({"jsonrpc":"2.0","id":2,"method":"Event.poll","params":{"since":)" +
             std::to_string(next - 1) + R"(,"windowId":"9"}})")
                .c_str());
    runtime.drain(clock.now_value);
    const JsonValue second = responses.parsed(101);
    CHECK(second.find("result")->find("gap")->as_boolean() == true);
    CHECK(second.find("result")->find("windowId") == nullptr ||
          second.find("result")->find("events")->size() <= 3);
}

void test_connection_close_and_shutdown() {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0"}, std::ref(clock)};
    Responses responses;
    responses.install(runtime);

    automation::CompletionSink captured;
    std::vector<MethodDescriptor> methods;
    methods.push_back(simple_method("app.hang.work", "defers forever"));
    MethodRegistration registration = runtime.register_methods(
        "app.hang", std::move(methods),
        [&](const RequestContext&, automation::CompletionSink sink) {
            captured = std::move(sink);
        });
    CHECK(registration.valid());

    receive(runtime, 110, R"({"jsonrpc":"2.0","id":1,"method":"app.hang.work"})");
    runtime.drain(clock.now_value);
    CHECK(!responses.has(110));
    runtime.connection_closed(7);
    CHECK(responses.has(110));
    CHECK(responses.error_code(110) == 1007);   // Cancelled

    // After shutdown new requests are rejected outright.
    runtime.shutdown(clock.now_value);
    receive(runtime, 111, R"({"jsonrpc":"2.0","id":2,"method":"System.getInfo"})");
    CHECK(responses.has(111));
    CHECK(responses.error_code(111) == 1007);
    (void)registration;
}

void test_schema_document_lists_methods() {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0", "demo"}, std::ref(clock)};
    std::vector<MethodDescriptor> methods;
    methods.push_back(simple_method("app.schema.probe", "listed in the schema"));
    const auto registration = runtime.register_methods(
        "app.schema", std::move(methods), [](const RequestContext&, automation::CompletionSink) {});
    CHECK(registration.valid());
    const JsonValue document = runtime.build_schema_document();
    CHECK(document.find("protocolVersion")->as_string() == "1.0");
    CHECK(document.find("instanceId")->is_string());
    bool found = false;
    for (const JsonValue& method : document.find("methods")->items()) {
        if (method.find("name")->as_string() == "app.schema.probe") found = true;
    }
    CHECK(found);
    CHECK(document.find("errors")->size() > 0);
    CHECK(document.find("events")->size() > 0);
}

void test_wake_pump_two_subscribers() {
    struct PumpPlatform : tabengine::IPlatform {
        std::function<void(const tabengine::Event&)> handler;
        std::function<bool(tabengine::WindowId, tabengine::Point)> caption;
        std::function<void()> wake_handler;
        int wakes = 0;
        void set_event_handler(std::function<void(const tabengine::Event&)>) override {}
        void set_caption_hit_handler(
            std::function<bool(tabengine::WindowId, tabengine::Point)>) override {}
        void set_wake_handler(std::function<void()> handler_fn) override {
            wake_handler = std::move(handler_fn);
        }
        void wake() override { ++wakes; }
        bool create(tabengine::WindowId, tabengine::Rect, std::string_view, bool) override {
            return true;
        }
        void show(tabengine::WindowId) override {}
        void destroy(tabengine::WindowId) override {}
        void invalidate(tabengine::WindowId) override {}
        double monotonic_seconds() const override { return 0.0; }
        void request_animation_frame(tabengine::WindowId) override {}
        void capture_pointer(tabengine::WindowId) override {}
        void release_pointer() override {}
        void minimize(tabengine::WindowId) override {}
        void toggle_maximize(tabengine::WindowId) override {}
        tabengine::Size client_size(tabengine::WindowId) const override { return {}; }
        tabengine::Point client_origin(tabengine::WindowId) const override { return {}; }
        float scale(tabengine::WindowId) const override { return 1.0f; }
        void* native_handle(tabengine::WindowId) const override { return nullptr; }
        tabengine::WindowId window_at(tabengine::Point, tabengine::WindowId) const override {
            return 0;
        }
        bool supports_native_move_loop() const override { return true; }
        void set_client_origin(tabengine::WindowId, tabengine::Point) override {}
        tabengine::MoveLoopResult run_native_move_loop(tabengine::WindowId) override {
            return tabengine::MoveLoopResult::Unsupported;
        }
        void end_native_move_loop(tabengine::WindowId) override {}
        int run() override { return 0; }
    } platform;

    automation::UiTaskPump pump;
    pump.bind_platform(platform);
    CHECK(platform.wake_handler != nullptr);

    int first_drains = 0;
    int second_drains = 0;
    automation::UiTaskPump::Subscription first =
        pump.subscribe([&] { ++first_drains; });
    {
        automation::UiTaskPump::Subscription second =
            pump.subscribe([&] { ++second_drains; });
        platform.wake_handler();
        CHECK(first_drains == 1);
        CHECK(second_drains == 1);
    }
    // The destroyed subscription no longer drains.
    platform.wake_handler();
    CHECK(first_drains == 2);
    CHECK(second_drains == 1);
    // Destroying the pump detaches the platform handler. Binding a second
    // pump replaces the wake handler, so only its subscriber fires.
    {
        automation::UiTaskPump scoped;
        scoped.bind_platform(platform);
        automation::UiTaskPump::Subscription third = scoped.subscribe([&] { ++first_drains; });
        CHECK(platform.wake_handler != nullptr);
        platform.wake_handler();
        CHECK(first_drains == 3);
    }
    CHECK(platform.wake_handler == nullptr);
}

} // namespace

void test_event_gap_when_ring_fully_trimmed() {
    RuntimeBudgets budgets;
    budgets.event_items = 2;
    budgets.event_bytes = 1;   // forces the ring to trim to empty
    Clock clock;
    AutomationRuntime runtime{budgets, {"1.0", "0.1.0"}, std::ref(clock)};
    Responses responses;
    responses.install(runtime);
    for (int i = 0; i < 3; ++i) {
        AutomationEvent event;
        event.name = "tab.added";
        event.window = i + 1;
        runtime.emit_event(std::move(event));
    }
    // The ring is empty but the caller's cursor predates the discarded
    // events: an empty list must still report a gap, never "synchronized".
    receive(runtime, 110,
            R"({"jsonrpc":"2.0","id":1,"method":"Event.poll","params":{"since":1}})");
    runtime.drain(clock.now_value);
    const JsonValue stale = responses.parsed(110);
    CHECK(stale.find("result")->find("gap")->as_boolean() == true);
    CHECK(stale.find("result")->find("events")->size() == 0);

    // A cursor at the newest sequence is genuinely synchronized.
    receive(runtime, 111,
            R"({"jsonrpc":"2.0","id":2,"method":"Event.poll","params":{"since":3}})");
    runtime.drain(clock.now_value);
    const JsonValue fresh = responses.parsed(111);
    CHECK(fresh.find("result")->find("gap")->as_boolean() == false);
}

// Adversarial concurrency: workers settle deferred requests (the transport
// response path) while the UI thread drains, which iterates and purges
// wait conditions. Exercises the retired-slot handoff; before that fix a
// worker's erase raced the UI iteration of the same vector.
void test_concurrent_settle_against_draining_waits() {
    Clock clock;
    RuntimeBudgets budgets;
    budgets.inflight_per_session = 64;   // room for the waits + workers below
    budgets.queue_limit = 512;
    AutomationRuntime runtime{budgets, {"1.0", "0.1.0"}, std::ref(clock)};

    std::mutex responses_mutex;
    std::map<std::uint64_t, std::string> responses;
    runtime.set_response_sink(
        [&](std::uint64_t, std::uint64_t request, std::string payload) {
            std::lock_guard<std::mutex> guard(responses_mutex);
            responses.emplace(request, std::move(payload));
        });

    struct Provider : automation::IActivityProvider {
        std::atomic<int> pending{1};
        bool activity(std::string_view scope, automation::ActivityReport& out) override {
            if (scope != "tasks") return false;
            out.generation = 1;
            out.pending = pending.load();
            return true;
        }
        std::vector<std::string> scopes() const override { return {"tasks"}; }
    } provider;
    const auto provider_registration = runtime.register_activity_provider(&provider);
    CHECK(provider_registration.valid());

    std::mutex sinks_mutex;
    std::vector<automation::CompletionSink> deferred;
    std::vector<MethodDescriptor> methods;
    MethodDescriptor descriptor = simple_method("app.race.work", "defers");
    descriptor.timeout_s = 60.0;
    methods.push_back(descriptor);
    MethodRegistration registration = runtime.register_methods(
        "app.race", std::move(methods),
        [&](const RequestContext&, automation::CompletionSink sink) {
            std::lock_guard<std::mutex> guard(sinks_mutex);
            deferred.push_back(std::move(sink));
        });
    CHECK(registration.valid());

    // Long-lived waits whose conditions the UI thread re-evaluates on every
    // drain while the workers below settle concurrently.
    for (int i = 0; i < 16; ++i) {
        runtime.receive(7, 10'000 + static_cast<std::uint64_t>(i),
                        R"({"jsonrpc":"2.0","id":"w)" + std::to_string(i) +
                            R"(","method":"Wait.condition","params":{"condition":"activityQuiet","scope":"tasks","timeoutS":60}})");
    }
    runtime.drain(clock.now_value);

    std::uint64_t next_request = 20'000;
    for (int round = 0; round < 150; ++round) {
        clock.now_value += 0.05;   // past each wait's poll interval
        for (int i = 0; i < 4; ++i) {
            runtime.receive(7, next_request++,
                            R"({"jsonrpc":"2.0","id":"r","method":"app.race.work"})");
        }
        runtime.drain(clock.now_value);   // handlers capture sinks
        std::vector<automation::CompletionSink> to_settle;
        {
            std::lock_guard<std::mutex> guard(sinks_mutex);
            to_settle = std::move(deferred);
            deferred.clear();
        }
        // The scope goes quiet mid-run so waits start settling through the
        // same drains the workers race against.
        if (round == 40) provider.pending.store(0);
        std::thread worker([&to_settle] {
            for (automation::CompletionSink& sink : to_settle) {
                sink(MethodOutcome::success(JsonValue::object()));
            }
        });
        runtime.drain(clock.now_value);
        worker.join();
    }
    // Every wait settles satisfied and every work request got a response.
    for (int attempt = 0; attempt < 200; ++attempt) {
        clock.now_value += 0.05;
        runtime.drain(clock.now_value);
        std::lock_guard<std::mutex> guard(responses_mutex);
        if (responses.size() >= 16 + 150 * 4) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    int satisfied = 0;
    int completed = 0;
    {
        std::lock_guard<std::mutex> guard(responses_mutex);
        for (const auto& [request, payload] : responses) {
            (void)request;
            if (payload.find("\"satisfied\":true") != std::string::npos) ++satisfied;
            if (payload.find("\"result\":{}") != std::string::npos) ++completed;
        }
        if (responses.size() != 16 + 150 * 4) {
            std::fprintf(stderr, "responses=%zu satisfied=%d completed=%d\n",
                         responses.size(), satisfied, completed);
        }
        CHECK(responses.size() == 16 + 150 * 4);
    }
    CHECK(satisfied == 16);
    CHECK(completed == 150 * 4);
}

// Regression: the per-session and queue overload rejections must answer
// with Overloaded instead of throwing from receive() (a self-deadlock in
// the MSVC debug mutex used to terminate the calling worker thread, which
// a client could trigger with 17 outstanding requests).
void test_overload_answers_instead_of_terminating() {
    Clock clock;
    AutomationRuntime runtime{{}, {"1.0", "0.1.0"}, std::ref(clock)};
    Responses responses;
    responses.install(runtime);

    std::vector<MethodDescriptor> methods;
    methods.push_back(simple_method("app.hold.work", "defers forever"));
    MethodRegistration registration = runtime.register_methods(
        "app.hold", std::move(methods),
        [](const RequestContext&, automation::CompletionSink) {
            // never settles: the requests stay in flight
        });
    CHECK(registration.valid());

    constexpr std::uint64_t kInflight = 16;
    for (std::uint64_t i = 0; i < kInflight; ++i) {
        receive(runtime, 500 + i,
                R"({"jsonrpc":"2.0","id":"hold","method":"app.hold.work"})");
    }
    runtime.drain(clock.now_value);
    for (std::uint64_t i = 0; i < kInflight; ++i) CHECK(!responses.has(500 + i));

    receive(runtime, 599, R"({"jsonrpc":"2.0","id":"over","method":"app.hold.work"})");
    CHECK(responses.has(599));   // answered synchronously, no crash
    CHECK(responses.error_code(599) == 1009);   // Overloaded

    // Queue limit: with the executor busy, more receives hit the cap and
    // still answer cleanly. (`small` is a macro in the Windows RPC headers,
    // so the budget object gets a neutral name.)
    RuntimeBudgets tight;
    tight.queue_limit = 2;
    tight.inflight_per_session = 8;
    AutomationRuntime limited{tight, {"1.0", "0.1.0"}, std::ref(clock)};
    Responses limited_responses;
    limited_responses.install(limited);
    std::vector<MethodDescriptor> queued;
    queued.push_back(simple_method("app.hold.work", "defers forever"));
    MethodRegistration queued_registration = limited.register_methods(
        "app.hold", std::move(queued),
        [](const RequestContext&, automation::CompletionSink) {});
    CHECK(queued_registration.valid());
    for (std::uint64_t request = 0; request < 4; ++request) {
        limited.receive(3, 700 + request,
                        R"({"jsonrpc":"2.0","id":"q","method":"app.hold.work"})");
    }
    CHECK(limited_responses.has(702));   // the third sits at the queue cap
    CHECK(limited_responses.error_code(702) == 1009 ||
          limited_responses.has(703));
}

int main() {
    run_automation_values_tests();
    test_app_method_round_trip();
    test_envelope_rejections();
    test_param_validation_and_grants();
    test_grant_enforcement();
    test_domain_rules();
    test_unregistered_provider_returns_unavailable();
    test_queued_deadline_never_executes();
    test_executing_deadline_and_deferred_success();
    test_operations();
    test_activity_wait();
    test_wait_unsupported_without_provider();
    test_events_and_gap();
    test_connection_close_and_shutdown();
    test_schema_document_lists_methods();
    test_wake_pump_two_subscribers();
    test_event_gap_when_ring_fully_trimmed();
    test_concurrent_settle_against_draining_waits();
    test_overload_answers_instead_of_terminating();
    return 0;
}
