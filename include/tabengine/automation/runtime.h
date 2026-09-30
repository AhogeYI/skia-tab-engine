#pragma once

// AutomationRuntime: the in-process control runtime. It owns the method
// registry, the bounded request queue with deadlines, the event ring,
// operations, wait conditions, and the engine's own domains (System, Event,
// Operation, Wait, Log). The Windows shell bridge adds Window/Tab/Input/UI/
// Capture/Trace through register_engine_methods; applications add methods
// under "app.<owner>." / "test.<owner>." and register observation providers.
//
// Threading: transport-facing entry points (receive, connection_closed,
// set_response_sink, set_wake) are thread-safe and never touch UI objects.
// drain(), shutdown(), registration and provider calls are UI-thread. The
// runtime never installs platform handlers itself - hosts compose wakes
// through UiTaskPump and call drain() from their own loop.

#include "tabengine/automation/activity.h"
#include "tabengine/automation/artifacts.h"
#include "tabengine/automation/diagnostics.h"
#include "tabengine/automation/errors.h"
#include "tabengine/automation/events.h"
#include "tabengine/automation/method.h"
#include "tabengine/automation/policy.h"
#include "tabengine/automation/schema.h"
#include "tabengine/automation/semantic.h"
#include "tabengine/automation/values.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// IPlatform is used by value in the pump's bind entry point.
#include "tabengine/platform.h"

namespace tabengine::automation {

struct RuntimeBudgets {
    // Request/queue limits. Hosts may lower these at construction; clients
    // cannot raise them. Exceeding a limit yields BudgetExceeded/Overloaded.
    std::size_t request_bytes = 256 * 1024;       // max JSON-RPC payload
    std::size_t queue_limit = 64;                 // pending requests, all sessions
    std::size_t inflight_per_session = 16;        // queued+executing per connection
    double request_timeout_s = 10.0;              // default execution deadline
    std::size_t event_items = 2048;               // event ring entries
    std::size_t event_bytes = 4 * 1024 * 1024;    // event ring serialized budget
    std::size_t poll_batch = 256;                 // max events per Event.poll
    std::size_t screenshot_max_pixels = 16'000'000;
    std::size_t inflight_captures = 2;
    std::size_t artifact_bytes = 256 * 1024 * 1024;   // artifact + pixels memory cap
    double artifact_ttl_s = 60.0;
    double trace_duration_s = 30.0;
    // UI dispatch pacing.
    double drain_budget_ms = 4.0;                 // wall-clock budget per drain()
    std::size_t drain_batch = 32;                 // max requests started per drain()
};

struct InstanceIdentity {
    std::string protocol_version = "1.0";
    std::string engine_version;       // TabEngine library version
    std::string application;          // host display name (may be empty)
    std::string app_version;
    std::string sdk_build;            // build identifier (may be empty)
    std::string instance_id;          // generated when left empty
};

enum class OperationState { Running, Completed, Failed, Cancelled };

struct OperationInfo {
    std::uint64_t id = 0;
    std::string method;
    std::uint64_t connection = 0;
    OperationState state = OperationState::Running;
    JsonValue result;        // completed: result; failed/cancelled: error payload
    Error error;             // failed/cancelled only
    double started = 0.0;
    double finished = 0.0;
};

// Engine-side predicates the wait domain evaluates. The shell bridge fills
// them at bind time; absent hooks make the corresponding conditions report
// Unsupported instead of pretending to be satisfied.
struct RuntimeEnvironmentHooks {
    // True while the engine still runs finite animations for the window.
    std::function<bool(WindowId)> animation_active;
    // Serial of the last presented frame for the window (0 = none yet).
    std::function<std::uint64_t(WindowId)> frame_serial;
    // True when a node with this stable business identity is in the
    // window's current semantic tree.
    std::function<bool(WindowId, std::string_view)> node_exists;
};

// Coalesced-wake dispatcher. The pump is the single owner of the platform's
// wake handler; each consumer (the host's own background work, the
// automation runtime, tests) subscribes and drains its own state. Wakes
// coalesce by design: subscribers must drain, not count.
class UiTaskPump {
public:
    UiTaskPump() = default;
    ~UiTaskPump();
    UiTaskPump(const UiTaskPump&) = delete;
    UiTaskPump& operator=(const UiTaskPump&) = delete;

    // Installs the pump as the platform's wake handler. Call once, on the
    // UI thread, before subscribing. The pump must not outlive the platform
    // it bound.
    void bind_platform(IPlatform& platform);

    class Subscription {
    public:
        Subscription() = default;
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        ~Subscription();

    private:
        friend class UiTaskPump;
        struct Releaser;
        explicit Subscription(std::shared_ptr<Releaser> releaser);
        std::shared_ptr<Releaser> releaser_;
    };

    // Register a drain callback. Destroy the subscription to detach.
    [[nodiscard]] Subscription subscribe(std::function<void()> drain);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

class AutomationRuntime {
public:
    // `clock` supplies monotonic seconds (the bridge passes
    // IPlatform::monotonic_seconds; tests pass a fake). Null defaults to a
    // zero clock for value-only tests.
    AutomationRuntime(RuntimeBudgets budgets, InstanceIdentity identity,
                      std::function<double()> clock);
    ~AutomationRuntime();
    AutomationRuntime(const AutomationRuntime&) = delete;
    AutomationRuntime& operator=(const AutomationRuntime&) = delete;

    // ---- registration (UI thread) ----
    // Application path: the domain must be "app.<owner>." or
    // "test.<owner>."; anything else (including engine domains) is rejected
    // and an invalid registration is returned.
    [[nodiscard]] MethodRegistration register_methods(
        std::string domain, std::vector<MethodDescriptor> methods, MethodHandler handler);
    // Engine path (shell bridge, tests): may register reserved domains.
    // Still rejects duplicates and malformed names.
    [[nodiscard]] MethodRegistration register_engine_methods(
        std::string domain, std::vector<MethodDescriptor> methods, MethodHandler handler);

    [[nodiscard]] ProviderRegistration register_semantic_provider(ISemanticProvider* provider);
    [[nodiscard]] ProviderRegistration register_activity_provider(IActivityProvider* provider);
    [[nodiscard]] ProviderRegistration register_diagnostics_provider(IDiagnosticProvider* provider);
    // Registered providers in registration order (UI thread). The engine
    // bridge reads these to compose merged semantic snapshots.
    [[nodiscard]] const std::vector<ISemanticProvider*>& semantic_providers() const;
    [[nodiscard]] const std::vector<IActivityProvider*>& activity_providers() const;
    // Non-owning; may be null to restore the default allow policy.
    void set_policy(IAutomationPolicy* policy);
    // Engine-side wait predicates; may be reset with a default-constructed
    // value. UI thread.
    void set_environment_hooks(RuntimeEnvironmentHooks hooks);
    // Capability block surfaced by System.getInfo/getCapabilities. Replaces
    // (not merges) on each call. UI thread.
    void set_instance_capabilities(JsonValue capabilities);

    // ---- transport-facing (thread-safe) ----
    using ResponseSink = std::function<void(std::uint64_t connection,
                                            std::uint64_t transport_request,
                                            std::string payload)>;
    // The sink is invoked with exactly one response per accepted request,
    // from an arbitrary thread. Set before serving; clear with null.
    void set_response_sink(ResponseSink sink);
    // Submit one JSON-RPC payload. Always produces exactly one response for
    // a parseable request (result or error), or an error response with a
    // null id when the payload cannot be parsed at all.
    void receive(std::uint64_t connection, std::uint64_t transport_request, std::string payload);
    // Drop one connection's session state: queued requests fail Cancelled,
    // deferred completions settle Cancelled, wait conditions detach.
    void connection_closed(std::uint64_t connection);
    // Immediate cross-thread wake when worker-side work needs a drain.
    void set_wake(std::function<void()> wake);
    // Bounded-delay wake used for wait polling and deadlines. The host
    // schedules one UI-thread drain() after the delay (may coalesce);
    // without it, waits only progress when other activity drains.
    void set_deferred_wake(std::function<void(double delay_seconds)> wake);

    // ---- UI thread ----
    // Execute queued work within the configured budget.
    void drain(double now);
    // Stop serving: fail queued and executing requests with Cancelled,
    // reject future ones. Irreversible. Registrations stay valid.
    void shutdown(double now);
    [[nodiscard]] bool accepting() const { return !stopped_.load(std::memory_order_acquire); }

    // ---- observation (UI thread) ----
    // Emit an application event ("app.<owner>.<event>" naming convention).
    void emit_event(AutomationEvent event);
    // Promote executing deferred work to a queryable operation. The request
    // may complete immediately with the returned id; the operation finishes
    // later through finish_operation.
    [[nodiscard]] std::uint64_t start_operation(std::string method,
                                                const RequestContext& context);
    // Finish an operation. `result` is used on success; `error` on failure.
    void finish_operation(std::uint64_t id, OperationState state, JsonValue result, Error error);
    [[nodiscard]] bool get_operation(std::uint64_t id, OperationInfo& out) const;
    // Engine log ring entry (bounded, correlation fields optional).
    void log(std::string_view level, std::string message, std::string request_id = {},
             std::uint64_t operation = 0);

    // ---- queries ----
    [[nodiscard]] const RuntimeBudgets& budgets() const;
    [[nodiscard]] const InstanceIdentity& identity() const;
    // Current monotonic time on the runtime's clock (the same base used for
    // deadlines, operations, artifacts and events). UI thread.
    [[nodiscard]] double now() const;
    [[nodiscard]] bool has_method(std::string_view name) const;
    // Bounded binary artifact store (screenshots, traces). Bytes are served
    // by the transport's artifact route, never written to disk by the
    // engine.
    [[nodiscard]] ArtifactStore& artifacts() { return *artifacts_; }
    [[nodiscard]] const ArtifactStore& artifacts() const { return *artifacts_; }
    // Schema document for GET /automation/v1/schema. UI thread.
    [[nodiscard]] JsonValue build_schema_document() const;
    [[nodiscard]] const JsonValue& instance_capabilities() const;

public:
    // Opaque implementation state. Defined in the runtime translation unit;
    // public only so registration releasers can hold weak references.
    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
    std::shared_ptr<ArtifactStore> artifacts_;
    std::atomic<bool> stopped_{false};

    [[nodiscard]] MethodRegistration wrap_token(std::uint64_t token);
    [[nodiscard]] static MethodDescriptor make_descriptor(
        std::string name, std::string summary, std::vector<std::string> grants, bool mutating,
        bool idempotent, double timeout_s, JsonValue params, JsonValue result);
    void register_internal_domains();
};

} // namespace tabengine::automation
