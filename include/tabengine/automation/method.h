#pragma once

// Method contract: descriptors, registration, request context and the
// completion token. Providers are application objects; the runtime owns all
// queueing, validation and settlement bookkeeping. Handler code runs on the
// UI thread; the completion sink may be invoked from any thread.

#include "tabengine/automation/errors.h"
#include "tabengine/automation/values.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tabengine::automation {

// Grant names used by the engine's own domains. Applications may register
// additional grant names; a session only executes a method when the session
// policy grants every name in the descriptor.
inline constexpr std::string_view kGrantObserve = "engine.observe";
inline constexpr std::string_view kGrantInput = "engine.input";
inline constexpr std::string_view kGrantWindowTab = "engine.windowTab";
inline constexpr std::string_view kGrantTestHooks = "engine.testHooks";

// Reserved engine domains. Application registrations must use the
// "app.<owner>." or "test.<owner>." prefix; anything else is rejected.
[[nodiscard]] bool is_reserved_domain(std::string_view domain);
// Valid application domain: "app.<owner>." / "test.<owner>." plus at least
// one more label of [A-Za-z0-9_].
[[nodiscard]] bool is_valid_app_domain(std::string_view method);

struct MethodDescriptor {
    std::string name;            // full wire name, e.g. "Tab.select" or "app.example.navigate"
    std::string summary;         // one-line English summary for tools and docs
    std::vector<std::string> grants;
    bool mutating = false;       // read/write classification surfaced in the schema
    bool idempotent = false;     // safe to retry with the same arguments
    double timeout_s = 10.0;     // execution deadline once dequeued
    JsonValue params;            // schema subset (object schema); empty object = no parameters
    JsonValue result;            // schema subset; empty object = null result
};

// One settled result delivered to the runtime. Exactly one of the two is
// meaningful; ok=false carries a protocol Error.
struct MethodOutcome {
    bool ok = false;
    JsonValue result;
    Error error;
    [[nodiscard]] static MethodOutcome success(JsonValue result);
    [[nodiscard]] static MethodOutcome failure(Error error);
};

using CompletionSink = std::function<void(MethodOutcome)>;

// Execution context handed to a method handler. The handler runs on the UI
// thread. Call the sink exactly once - synchronously for fast methods, or
// later (from any thread) for deferred work; the runtime rejects a second
// settlement. `cancel_flag` is cooperative: long work polls it and fails
// with Cancelled. No socket, platform, or window types appear here.
struct RequestContext {
    std::string request_id;      // JSON-RPC id rendered for correlation
    std::string method;
    JsonValue params;
    std::uint64_t connection = 0;
    std::uint64_t slot = 0;      // runtime-internal request slot (correlation)
    double submitted = 0.0;      // monotonic seconds at enqueue
    double deadline = 0.0;       // monotonic seconds; enforced by the runtime
    std::uint64_t operation_id = 0;   // non-zero when promoted to an operation
    std::shared_ptr<std::atomic<bool>> cancel_flag;   // never null while executing
};

// A method handler runs on the UI thread with the request context and a
// single-shot completion sink. The handler either invokes the sink before
// returning (fast methods) or defers it (long work); a deferred sink may
// fire from any thread. Never invoke a sink twice.
using MethodHandler = std::function<void(const RequestContext&, CompletionSink)>;

// A method registration. Destroying it unregisters the methods; after
// unregistration, requests targeting them fail with ProviderUnavailable and
// late sink calls are dropped. Move-only.
class MethodRegistration {
public:
    MethodRegistration() = default;
    MethodRegistration(MethodRegistration&& other) noexcept;
    MethodRegistration& operator=(MethodRegistration&& other) noexcept;
    MethodRegistration(const MethodRegistration&) = delete;
    MethodRegistration& operator=(const MethodRegistration&) = delete;
    ~MethodRegistration();

    [[nodiscard]] bool valid() const { return token_ != 0; }
    [[nodiscard]] std::uint64_t token() const { return token_; }

private:
    friend class AutomationRuntime;
    struct Releaser;   // implemented in the runtime translation unit
    MethodRegistration(std::shared_ptr<Releaser> releaser, std::uint64_t token);
    std::shared_ptr<Releaser> releaser_;
    std::uint64_t token_ = 0;
};

} // namespace tabengine::automation
