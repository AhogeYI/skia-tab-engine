// AutomationRuntime implementation: method registry, bounded request queue
// with deadlines, event ring, operations, wait conditions, and the engine's
// own System/Event/Operation/Wait/Log domains. Worker threads only touch
// state guarded by Impl::state_mutex; everything else is UI-thread by
// contract.

#include "tabengine/automation/runtime.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <random>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tabengine::automation {
namespace {

constexpr std::size_t kMaxLogItems = 512;
constexpr std::size_t kMaxLogBytes = 256 * 1024;
constexpr std::size_t kMaxLogBatch = 256;
constexpr double kWaitPollSeconds = 0.033;

bool valid_method_name(std::string_view name) {
    if (name.empty()) return false;
    std::size_t labels = 0;
    std::size_t i = 0;
    while (i <= name.size()) {
        std::size_t end = name.find('.', i);
        if (end == std::string_view::npos) end = name.size();
        if (end == i) return false;   // empty label
        for (std::size_t k = i; k < end; ++k) {
            const char c = name[k];
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '_';
            if (!ok) return false;
        }
        ++labels;
        if (end == name.size()) break;
        i = end + 1;
    }
    return labels >= 2;   // always "Domain.action"
}

bool valid_domain_name(std::string_view name) {
    if (name.empty() || name.starts_with('.') || name.ends_with('.')) return false;
    for (const char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}

// ---- JSON-RPC v1 restricted profile: envelope parsing and payloads ----

bool parse_envelope(const JsonValue& doc, JsonValue& id_echo, bool& has_id,
                    std::string& method, JsonValue& params, Error& error) {
    if (doc.is_array()) {
        error = Error::make(ErrorCode::InvalidRequest,
                            "batch requests are not supported in the v1 profile");
        return false;
    }
    if (!doc.is_object()) {
        error = Error::make(ErrorCode::InvalidRequest, "request must be a JSON object");
        return false;
    }
    const JsonValue* version = doc.find("jsonrpc");
    if (!version || !version->is_string() || version->as_string() != "2.0") {
        error = Error::make(ErrorCode::InvalidRequest, "jsonrpc must be the string \"2.0\"");
        return false;
    }
    const JsonValue* id = doc.find("id");
    if (!id) {
        error = Error::make(ErrorCode::InvalidRequest,
                            "id is required; notifications are not supported");
        return false;
    }
    if (id->is_null() || id->is_boolean() || (id->is_number() && !id->is_integer())) {
        error = Error::make(ErrorCode::InvalidRequest,
                            "id must be a string or an integer in the v1 profile");
        return false;
    }
    id_echo = *id;
    has_id = true;
    const JsonValue* method_value = doc.find("method");
    if (!method_value || !method_value->is_string() ||
        !valid_method_name(method_value->as_string())) {
        error = Error::make(ErrorCode::InvalidRequest, "method must be a dotted name");
        return false;
    }
    method = method_value->as_string();
    const JsonValue* params_value = doc.find("params");
    if (params_value) {
        if (!params_value->is_object()) {
            error = Error::make(ErrorCode::InvalidRequest,
                                "params must be an object in the v1 profile");
            return false;
        }
        params = *params_value;
    } else {
        params = JsonValue::object();
    }
    for (const auto& [key, member] : doc.members()) {
        (void)member;
        if (key != "jsonrpc" && key != "id" && key != "method" && key != "params") {
            error = Error::make(ErrorCode::InvalidRequest,
                                "unknown request member '" + key + "'");
            return false;
        }
    }
    return true;
}

std::string build_error_payload(const JsonValue& id_echo, bool has_id, const Error& error) {
    JsonValue response = JsonValue::object();
    response.set("jsonrpc", "2.0");
    response.set("id", has_id ? id_echo : JsonValue{});
    JsonValue error_body = JsonValue::object();
    error_body.set("code", static_cast<std::int64_t>(error.code));
    error_body.set("message", error.message);
    if (error.data.is_object() && !error.data.members().empty()) {
        error_body.set("data", error.data);
    }
    response.set("error", std::move(error_body));
    return response.serialize();
}

std::string build_result_payload(const JsonValue& id_echo, const JsonValue& result) {
    JsonValue response = JsonValue::object();
    response.set("jsonrpc", "2.0");
    response.set("id", id_echo);
    response.set("result", result);
    return response.serialize();
}

struct LogEntry {
    std::uint64_t sequence = 0;
    double at = 0.0;
    std::string level;
    std::string source;
    std::string message;
    std::string request_id;
    std::uint64_t operation = 0;
};

JsonValue event_to_wire(const AutomationEvent& event) {
    JsonValue value = JsonValue::object();
    value.set("sequence", static_cast<std::int64_t>(event.sequence));
    value.set("name", event.name);
    value.set("at", event.at);
    if (event.window) value.set("windowId", id_to_wire(event.window));
    if (event.tab) value.set("tabId", id_to_wire(event.tab));
    if (event.content) value.set("contentId", id_to_wire(event.content));
    if (event.generation) value.set("generation", static_cast<std::int64_t>(event.generation));
    if (!event.request_id.empty()) value.set("requestId", event.request_id);
    if (event.operation) value.set("operationId", id_to_wire(event.operation));
    if (!event.details.is_null()) value.set("details", event.details);
    return value;
}

JsonValue log_to_wire(const LogEntry& entry) {
    JsonValue value = JsonValue::object();
    value.set("sequence", static_cast<std::int64_t>(entry.sequence));
    value.set("at", entry.at);
    value.set("level", entry.level);
    value.set("source", entry.source);
    value.set("message", entry.message);
    if (!entry.request_id.empty()) value.set("requestId", entry.request_id);
    if (entry.operation) value.set("operationId", id_to_wire(entry.operation));
    return value;
}

JsonValue operation_to_wire(const OperationInfo& info) {
    JsonValue value = JsonValue::object();
    value.set("operationId", id_to_wire(info.id));
    value.set("method", info.method);
    switch (info.state) {
    case OperationState::Running: value.set("state", "running"); break;
    case OperationState::Completed: value.set("state", "completed"); break;
    case OperationState::Failed: value.set("state", "failed"); break;
    case OperationState::Cancelled: value.set("state", "cancelled"); break;
    }
    value.set("started", info.started);
    if (info.state != OperationState::Running) value.set("finished", info.finished);
    if (info.state == OperationState::Completed) value.set("result", info.result);
    if (info.state == OperationState::Failed || info.state == OperationState::Cancelled) {
        JsonValue error_body = JsonValue::object();
        error_body.set("code", static_cast<std::int64_t>(info.error.code));
        error_body.set("message", info.error.message);
        value.set("error", std::move(error_body));
    }
    return value;
}

std::string make_instance_id() {
    std::random_device device;
    const std::uint64_t a = (static_cast<std::uint64_t>(device()) << 32) | device();
    const std::uint64_t b = (static_cast<std::uint64_t>(device()) << 32) | device();
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%016llx%016llx",
                  static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    return buffer;
}

std::string request_id_of(const JsonValue& id_echo) {
    if (id_echo.is_string()) return id_echo.as_string();
    if (id_echo.is_integer()) return std::to_string(id_echo.as_integer());
    return {};
}

// Schema helpers for the internal domains' descriptors.
JsonValue schema_object(std::vector<std::pair<std::string, JsonValue>> properties,
                        std::vector<std::string> required = {}) {
    JsonValue schema = JsonValue::object();
    schema.set("type", "object");
    JsonValue props = JsonValue::object();
    for (auto& [name, property] : properties) props.set(std::move(name), std::move(property));
    schema.set("properties", std::move(props));
    if (!required.empty()) {
        JsonValue list = JsonValue::array();
        for (std::string& name : required) list.push_back(JsonValue(std::move(name)));
        schema.set("required", std::move(list));
    }
    return schema;
}

JsonValue string_property(std::string description) {
    JsonValue property = JsonValue::object();
    property.set("type", "string");
    property.set("description", std::move(description));
    return property;
}

JsonValue integer_property(std::string description, std::int64_t minimum = -1,
                           std::int64_t maximum = -1) {
    JsonValue property = JsonValue::object();
    property.set("type", "integer");
    property.set("description", std::move(description));
    if (minimum >= 0) property.set("minimum", minimum);
    if (maximum >= 0) property.set("maximum", maximum);
    return property;
}

JsonValue number_property(std::string description, double minimum = -1.0) {
    JsonValue property = JsonValue::object();
    property.set("type", "number");
    property.set("description", std::move(description));
    if (minimum >= 0.0) property.set("minimum", minimum);
    return property;
}

JsonValue enum_property(std::string description, std::vector<std::string> values) {
    JsonValue property = JsonValue::object();
    property.set("type", "string");
    property.set("description", std::move(description));
    JsonValue list = JsonValue::array();
    for (std::string& value : values) list.push_back(JsonValue(std::move(value)));
    property.set("enum", std::move(list));
    return property;
}

// ---- runtime-internal records ----

struct PendingRequest {
    std::uint64_t connection = 0;
    std::uint64_t transport_request = 0;
    JsonValue id_echo{};
    bool has_id = false;
    std::string method;
    JsonValue params;
    double submitted = 0.0;
    double deadline = 0.0;
    std::uint64_t slot = 0;
    std::uint64_t operation = 0;
    std::uint64_t domain_token = 0;
    std::shared_ptr<std::atomic<bool>> cancel_flag;
};

struct OperationRecord {
    OperationInfo info;
    std::shared_ptr<std::atomic<bool>> cancel_flag;
    std::vector<std::function<void(const OperationInfo&)>> waiters;
};

struct ConditionRecord {
    std::uint64_t slot = 0;
    std::string type;
    double submitted = 0.0;
    double next_poll = 0.0;
    double dwell = 0.0;
    double satisfied_since = -1.0;
    WindowId window = 0;
    std::string scope;
    std::string stable_key;
    std::uint64_t operation = 0;
    std::uint64_t frame_target = 0;
    std::function<bool(std::string& describe)> check;
};

struct DomainEntry {
    std::string domain;
    std::vector<MethodDescriptor> methods;
    MethodHandler handler;
};

struct SessionState {
    bool authorized = false;
    bool denied = false;
    std::vector<std::string> grants;
};

} // namespace

struct AutomationRuntime::Impl {
    Impl(RuntimeBudgets budget_values, InstanceIdentity identity_value,
         std::function<double()> clock_value)
        : budgets(budget_values), identity(std::move(identity_value)),
          clock(clock_value ? std::move(clock_value)
                            : std::function<double()>{}) {
        if (identity.instance_id.empty()) identity.instance_id = make_instance_id();
        if (!clock) clock = [] { return 0.0; };
    }

    RuntimeBudgets budgets;
    InstanceIdentity identity;
    std::function<double()> clock;

    // ---- guarded by state_mutex (worker + UI) ----
    mutable std::mutex state_mutex;
    std::deque<PendingRequest> queue;
    std::unordered_map<std::uint64_t, PendingRequest> executing;
    std::unordered_map<std::uint64_t, SessionState> sessions;
    ResponseSink response_sink;
    std::function<void()> wake_callback;
    std::function<void(double)> deferred_wake_callback;
    std::uint64_t next_slot = 1;

    // ---- guarded by registry_mutex ----
    mutable std::mutex registry_mutex;
    std::unordered_map<std::uint64_t, DomainEntry> domains;
    std::unordered_map<std::string, std::uint64_t> method_domain;
    std::uint64_t next_domain_token = 1;

    // ---- UI thread only ----
    ISemanticProvider* semantic_provider = nullptr;
    std::uint64_t semantic_token = 0;
    IActivityProvider* activity_provider = nullptr;
    std::uint64_t activity_token = 0;
    IDiagnosticProvider* diagnostics_provider = nullptr;
    std::uint64_t diagnostics_token = 0;
    IAutomationPolicy* policy = nullptr;
    RuntimeEnvironmentHooks env;

    std::mutex event_mutex;
    std::deque<AutomationEvent> events;
    std::size_t event_bytes = 0;
    std::uint64_t next_sequence = 1;
    std::uint64_t oldest_sequence = 0;

    std::vector<ConditionRecord> conditions;
    std::deque<LogEntry> log_ring;
    std::size_t log_bytes = 0;
    std::uint64_t next_log_sequence = 1;
    std::uint64_t next_operation = 1;
    std::unordered_map<std::uint64_t, OperationRecord> operations;
    // Terminal operations stay queryable for Operation.get, bounded by age.
    std::deque<std::uint64_t> terminal_operations;
    static constexpr std::size_t kTerminalOperationKeep = 256;

    JsonValue capabilities = JsonValue::object();

    // Registrations capture a weak reference so they never extend the
    // runtime's lifetime.
    std::weak_ptr<Impl> self_weak;

    [[nodiscard]] double now() const { return clock(); }

    // ---- response plumbing ----

    void respond_now(std::uint64_t connection, std::uint64_t transport_request,
                     const JsonValue& id_echo, bool has_id, MethodOutcome outcome) {
        ResponseSink sink;
        {
            std::lock_guard<std::mutex> guard(state_mutex);
            sink = response_sink;
        }
        if (!sink) return;
        const std::string payload = outcome.ok
            ? build_result_payload(id_echo, outcome.result)
            : build_error_payload(id_echo, has_id, outcome.error);
        sink(connection, transport_request, std::move(payload));
    }

    // Settle one executing request. Safe from any thread; idempotent.
    void settle(std::uint64_t slot, MethodOutcome outcome) {
        PendingRequest entry;
        ResponseSink sink;
        {
            std::lock_guard<std::mutex> guard(state_mutex);
            const auto it = executing.find(slot);
            if (it == executing.end()) return;
            entry = std::move(it->second);
            executing.erase(it);
            sink = response_sink;
        }
        std::erase_if(conditions, [slot](const ConditionRecord& record) {
            return record.slot == slot;
        });
        if (outcome.ok) {
            log("debug", "completed " + entry.method, request_id_of(entry.id_echo),
                entry.operation);
        } else {
            log("info",
                "failed " + entry.method + ": " + std::string(error_type_name(outcome.error.code)),
                request_id_of(entry.id_echo), entry.operation);
        }
        if (!sink) return;
        const std::string payload = outcome.ok
            ? build_result_payload(entry.id_echo, outcome.result)
            : build_error_payload(entry.id_echo, entry.has_id, outcome.error);
        sink(entry.connection, entry.transport_request, std::move(payload));
    }

    void settle_deadline_or_cancel(std::uint64_t slot) {
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> guard(state_mutex);
            const auto it = executing.find(slot);
            if (it == executing.end()) return;
            cancelled = it->second.cancel_flag && it->second.cancel_flag->load();
        }
        if (cancelled) {
            settle(slot, MethodOutcome::failure(
                             Error::make(ErrorCode::Cancelled, "request was cancelled")));
        } else {
            settle(slot, MethodOutcome::failure(
                             Error::make(ErrorCode::DeadlineExceeded,
                                         "request exceeded its execution deadline")));
        }
    }

    void log(std::string_view level, std::string message, std::string request_id = {},
             std::uint64_t operation = 0) {
        LogEntry entry;
        entry.sequence = next_log_sequence++;
        entry.at = clock();
        entry.level = std::string(level);
        entry.source = "engine";
        entry.message = std::move(message);
        entry.request_id = std::move(request_id);
        entry.operation = operation;
        const std::size_t bytes = entry.message.size() + 96;
        log_bytes += bytes;
        log_ring.push_back(std::move(entry));
        while (log_ring.size() > kMaxLogItems || log_bytes > kMaxLogBytes) {
            if (log_ring.empty()) break;
            log_bytes -= log_ring.front().message.size() + 96;
            log_ring.pop_front();
        }
    }

    void append_event(AutomationEvent event) {
        event.sequence = next_sequence++;
        event.at = clock();
        const std::size_t bytes = event.name.size() + 160;
        std::lock_guard<std::mutex> guard(event_mutex);
        events.push_back(std::move(event));
        event_bytes += bytes;
        while (events.size() > budgets.event_items || event_bytes > budgets.event_bytes) {
            if (events.empty()) break;
            event_bytes -= events.front().name.size() + 160;
            events.pop_front();
        }
        oldest_sequence = events.empty() ? next_sequence - 1 : events.front().sequence;
    }

    // ---- registration (returns 0 on rejection) ----

    std::uint64_t install_domain(const std::string& domain,
                                 std::vector<MethodDescriptor> methods, MethodHandler handler,
                                 bool engine_path) {
        if (!handler || !valid_domain_name(domain)) return 0;
        if (engine_path && (domain.starts_with("app.") || domain.starts_with("test."))) return 0;
        if (!engine_path && !domain.starts_with("app.") && !domain.starts_with("test.")) return 0;
        std::unordered_set<std::string> seen;
        for (const MethodDescriptor& method : methods) {
            if (!method.name.starts_with(domain + ".") ||
                method.name.size() <= domain.size() + 1) {
                return 0;
            }
            if (!seen.insert(method.name).second) return 0;
        }
        std::lock_guard<std::mutex> guard(registry_mutex);
        for (const MethodDescriptor& method : methods) {
            if (method_domain.count(method.name) != 0) return 0;
        }
        const std::uint64_t token = next_domain_token++;
        DomainEntry entry;
        entry.domain = domain;
        entry.methods = std::move(methods);
        entry.handler = std::move(handler);
        for (const MethodDescriptor& method : entry.methods) {
            method_domain.emplace(method.name, token);
        }
        domains.emplace(token, std::move(entry));
        return token;
    }

    // ---- schema document ----

    JsonValue build_schema_document() {
        JsonValue document = JsonValue::object();
        document.set("protocolVersion", identity.protocol_version);
        document.set("engineVersion", identity.engine_version);
        document.set("instanceId", identity.instance_id);
        JsonValue profile = JsonValue::object();
        profile.set("transport", "loopback-http jsonrpc-2.0 single-request");
        profile.set("batch", false);
        profile.set("notifications", false);
        profile.set("idType", "string or integer");
        document.set("profile", std::move(profile));

        JsonValue methods = JsonValue::array();
        std::vector<std::string> event_names_list;
        {
            std::lock_guard<std::mutex> guard(registry_mutex);
            for (const auto& [token, entry] : domains) {
                (void)token;
                for (const MethodDescriptor& method : entry.methods) {
                    JsonValue wire = JsonValue::object();
                    wire.set("name", method.name);
                    wire.set("summary", method.summary);
                    wire.set("mutating", method.mutating);
                    wire.set("idempotent", method.idempotent);
                    wire.set("timeoutS", method.timeout_s);
                    JsonValue grants = JsonValue::array();
                    for (const std::string& grant : method.grants) grants.push_back(grant);
                    wire.set("grants", std::move(grants));
                    wire.set("params", method.params);
                    wire.set("result", method.result);
                    methods.push_back(std::move(wire));
                }
            }
        }
        document.set("methods", std::move(methods));

        for (const std::string_view name : {
                 event_names::kWindowAdded, event_names::kWindowRemoved,
                 event_names::kWindowActivated, event_names::kWindowDeactivated,
                 event_names::kWindowBoundsChanged, event_names::kTabAdded,
                 event_names::kTabRemoved, event_names::kTabMoved, event_names::kTabSelected,
                 event_names::kTabUpdated, event_names::kTabTransferred,
                 event_names::kInputDispatched, event_names::kAnimationSettled,
                 event_names::kFramePresented, event_names::kCaptureCompleted,
                 event_names::kOperationUpdated}) {
            event_names_list.emplace_back(name);
        }
        JsonValue events_value = JsonValue::array();
        for (std::string& name : event_names_list) events_value.push_back(name);
        document.set("events", std::move(events_value));

        JsonValue errors = JsonValue::array();
        for (const ErrorCode code : {
                 ErrorCode::Unsupported, ErrorCode::PermissionDenied,
                 ErrorCode::TargetNotFound, ErrorCode::StaleNode, ErrorCode::NotInteractable,
                 ErrorCode::AmbiguousTarget, ErrorCode::DeadlineExceeded, ErrorCode::Cancelled,
                 ErrorCode::ProviderUnavailable, ErrorCode::Overloaded,
                 ErrorCode::SurfaceUnavailable, ErrorCode::DeviceLost,
                 ErrorCode::OperationNotFound, ErrorCode::InvalidOperationState,
                 ErrorCode::BudgetExceeded, ErrorCode::InvalidArgument, ErrorCode::SessionConflict,
                 ErrorCode::InvalidSession}) {
            JsonValue entry = JsonValue::object();
            entry.set("code", static_cast<std::int64_t>(code));
            entry.set("type", std::string(error_type_name(code)));
            errors.push_back(std::move(entry));
        }
        document.set("errors", std::move(errors));

        if (activity_provider) {
            JsonValue scopes = JsonValue::array();
            for (std::string& scope : activity_provider->scopes()) scopes.push_back(scope);
            document.set("activityScopes", std::move(scopes));
        }
        JsonValue budgets_value = JsonValue::object();
        budgets_value.set("requestBytes", static_cast<std::int64_t>(budgets.request_bytes));
        budgets_value.set("queueLimit", static_cast<std::int64_t>(budgets.queue_limit));
        budgets_value.set("inflightPerSession",
                          static_cast<std::int64_t>(budgets.inflight_per_session));
        budgets_value.set("requestTimeoutS", budgets.request_timeout_s);
        budgets_value.set("eventItems", static_cast<std::int64_t>(budgets.event_items));
        budgets_value.set("pollBatch", static_cast<std::int64_t>(budgets.poll_batch));
        budgets_value.set("screenshotMaxPixels",
                          static_cast<std::int64_t>(budgets.screenshot_max_pixels));
        budgets_value.set("inflightCaptures", static_cast<std::int64_t>(budgets.inflight_captures));
        budgets_value.set("artifactBytes", static_cast<std::int64_t>(budgets.artifact_bytes));
        budgets_value.set("artifactTtlS", budgets.artifact_ttl_s);
        budgets_value.set("traceDurationS", budgets.trace_duration_s);
        document.set("budgets", std::move(budgets_value));
        return document;
    }

    // ---- wait conditions ----

    void check_conditions(double now_value) {
        // Settles are deferred to the end of the pass: settle() erases from
        // `conditions` and must not run while iterating it.
        std::vector<std::pair<std::uint64_t, MethodOutcome>> settled;
        std::vector<std::uint64_t> finished;
        for (ConditionRecord& record : conditions) {
            bool slot_alive = false;
            {
                std::lock_guard<std::mutex> guard(state_mutex);
                slot_alive = executing.count(record.slot) != 0;
            }
            if (!slot_alive) {
                finished.push_back(record.slot);
                continue;
            }
            if (now_value < record.next_poll) continue;
            std::string describe;
            if (record.check(describe)) {
                if (record.dwell > 0.0) {
                    if (record.satisfied_since < 0.0) record.satisfied_since = now_value;
                    if (now_value - record.satisfied_since < record.dwell) {
                        record.next_poll = now_value + kWaitPollSeconds;
                        continue;
                    }
                }
                JsonValue result = JsonValue::object();
                result.set("condition", record.type);
                result.set("describe", describe);
                result.set("satisfied", true);
                result.set("waitedS", now_value - record.submitted);
                settled.emplace_back(record.slot, MethodOutcome::success(std::move(result)));
                finished.push_back(record.slot);
            } else {
                record.next_poll = now_value + kWaitPollSeconds;
            }
        }
        if (!finished.empty()) {
            std::erase_if(conditions, [&](const ConditionRecord& candidate) {
                return std::find(finished.begin(), finished.end(), candidate.slot) !=
                       finished.end();
            });
        }
        for (auto& [slot, outcome] : settled) settle(slot, std::move(outcome));
    }

    void schedule_wait_wake(double now_value) {
        double earliest = 0.0;
        for (const ConditionRecord& record : conditions) {
            earliest = earliest == 0.0 ? record.next_poll : std::min(earliest, record.next_poll);
        }
        if (earliest <= 0.0) return;
        const double delay = std::clamp(earliest - now_value, 0.001, 5.0);
        std::function<void(double)> wake;
        {
            std::lock_guard<std::mutex> guard(state_mutex);
            wake = deferred_wake_callback;
        }
        if (wake) wake(delay);
    }
};

// ---- pump ----

struct UiTaskPump::Impl {
    IPlatform* platform = nullptr;
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, std::function<void()>>> subscribers;
    std::uint64_t next_token = 1;

    void fire() {
        std::vector<std::function<void()>> drains;
        {
            std::lock_guard<std::mutex> guard(mutex);
            drains.reserve(subscribers.size());
            for (const auto& [token, drain] : subscribers) {
                (void)token;
                drains.push_back(drain);
            }
        }
        for (auto& drain : drains) drain();
    }
};

// ---- registration value types ----

struct MethodRegistration::Releaser {
    std::weak_ptr<AutomationRuntime::Impl> owner;
    std::uint64_t token = 0;
    ~Releaser() {
        const std::shared_ptr<AutomationRuntime::Impl> impl = owner.lock();
        if (!impl) return;
        std::lock_guard<std::mutex> guard(impl->registry_mutex);
        const auto domain_it = impl->domains.find(token);
        if (domain_it == impl->domains.end()) return;
        for (const MethodDescriptor& method : domain_it->second.methods) {
            impl->method_domain.erase(method.name);
        }
        impl->domains.erase(domain_it);
    }
};

struct ProviderRegistration::Releaser {
    std::weak_ptr<AutomationRuntime::Impl> owner;
    std::uint64_t token = 0;
    std::uint64_t slot_id = 0;   // 0 semantic, 1 activity, 2 diagnostics
    ~Releaser() {
        const std::shared_ptr<AutomationRuntime::Impl> impl = owner.lock();
        if (!impl) return;
        switch (slot_id) {
        case 0:
            if (impl->semantic_token == token) {
                impl->semantic_provider = nullptr;
                impl->semantic_token = 0;
            }
            break;
        case 1:
            if (impl->activity_token == token) {
                impl->activity_provider = nullptr;
                impl->activity_token = 0;
            }
            break;
        default:
            if (impl->diagnostics_token == token) {
                impl->diagnostics_provider = nullptr;
                impl->diagnostics_token = 0;
            }
            break;
        }
    }
};

struct UiTaskPump::Subscription::Releaser {
    std::weak_ptr<UiTaskPump::Impl> owner;
    std::uint64_t token = 0;
    ~Releaser() {
        const std::shared_ptr<UiTaskPump::Impl> impl = owner.lock();
        if (!impl) return;
        std::lock_guard<std::mutex> guard(impl->mutex);
        std::erase_if(impl->subscribers, [token = token](const auto& entry) {
            return entry.first == token;
        });
    }
};

MethodRegistration::MethodRegistration(std::shared_ptr<Releaser> releaser, std::uint64_t token)
    : releaser_(std::move(releaser)), token_(token) {}

MethodRegistration::MethodRegistration(MethodRegistration&& other) noexcept = default;

MethodRegistration& MethodRegistration::operator=(MethodRegistration&& other) noexcept {
    if (this != &other) {
        releaser_ = std::move(other.releaser_);
        token_ = std::exchange(other.token_, 0);
    }
    return *this;
}

MethodRegistration::~MethodRegistration() = default;

ProviderRegistration::ProviderRegistration(std::shared_ptr<Releaser> releaser)
    : releaser_(std::move(releaser)) {}

ProviderRegistration::ProviderRegistration(ProviderRegistration&& other) noexcept = default;

ProviderRegistration& ProviderRegistration::operator=(ProviderRegistration&& other) noexcept {
    if (this != &other) {
        releaser_ = std::move(other.releaser_);
    }
    return *this;
}

ProviderRegistration::~ProviderRegistration() = default;

void UiTaskPump::bind_platform(IPlatform& platform) {
    if (!impl_) impl_ = std::make_shared<Impl>();
    impl_->platform = &platform;
    platform.set_wake_handler([impl = impl_] { impl->fire(); });
}

UiTaskPump::Subscription UiTaskPump::subscribe(std::function<void()> drain) {
    if (!impl_) impl_ = std::make_shared<Impl>();
    std::uint64_t token = 0;
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        token = impl_->next_token++;
        impl_->subscribers.emplace_back(token, std::move(drain));
    }
    auto releaser = std::make_shared<Subscription::Releaser>();
    releaser->owner = impl_;
    releaser->token = token;
    return Subscription(std::move(releaser));
}

UiTaskPump::Subscription::Subscription(std::shared_ptr<Releaser> releaser)
    : releaser_(std::move(releaser)) {}

UiTaskPump::Subscription::Subscription(Subscription&& other) noexcept = default;

UiTaskPump::Subscription& UiTaskPump::Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        releaser_ = std::move(other.releaser_);
    }
    return *this;
}

UiTaskPump::Subscription::~Subscription() = default;

UiTaskPump::~UiTaskPump() {
    if (impl_ && impl_->platform) {
        impl_->platform->set_wake_handler({});
        impl_->platform = nullptr;
    }
}

// ---- runtime lifecycle ----

AutomationRuntime::AutomationRuntime(RuntimeBudgets budgets, InstanceIdentity identity,
                                     std::function<double()> clock)
    : impl_(std::make_shared<Impl>(budgets, std::move(identity), std::move(clock))) {
    impl_->self_weak = impl_;
    register_internal_domains();
}

AutomationRuntime::~AutomationRuntime() = default;

const RuntimeBudgets& AutomationRuntime::budgets() const { return impl_->budgets; }

const InstanceIdentity& AutomationRuntime::identity() const { return impl_->identity; }

const JsonValue& AutomationRuntime::instance_capabilities() const {
    return impl_->capabilities;
}

void AutomationRuntime::set_instance_capabilities(JsonValue capabilities) {
    impl_->capabilities = std::move(capabilities);
}

void AutomationRuntime::set_environment_hooks(RuntimeEnvironmentHooks hooks) {
    impl_->env = std::move(hooks);
}

bool AutomationRuntime::has_method(std::string_view name) const {
    std::lock_guard<std::mutex> guard(impl_->registry_mutex);
    return impl_->method_domain.count(std::string(name)) != 0;
}

JsonValue AutomationRuntime::build_schema_document() const {
    return impl_->build_schema_document();
}

MethodRegistration AutomationRuntime::wrap_token(std::uint64_t token) {
    if (token == 0) return {};
    auto releaser = std::make_shared<MethodRegistration::Releaser>();
    releaser->owner = impl_->self_weak;
    releaser->token = token;
    return MethodRegistration(std::move(releaser), token);
}

MethodRegistration AutomationRuntime::register_methods(
    std::string domain, std::vector<MethodDescriptor> methods, MethodHandler handler) {
    return wrap_token(impl_->install_domain(domain, std::move(methods), std::move(handler), false));
}

MethodRegistration AutomationRuntime::register_engine_methods(
    std::string domain, std::vector<MethodDescriptor> methods, MethodHandler handler) {
    return wrap_token(impl_->install_domain(domain, std::move(methods), std::move(handler), true));
}

ProviderRegistration AutomationRuntime::register_semantic_provider(ISemanticProvider* provider) {
    if (!provider) return {};
    impl_->semantic_provider = provider;
    impl_->semantic_token = impl_->next_domain_token++;
    auto releaser = std::make_shared<ProviderRegistration::Releaser>();
    releaser->owner = impl_->self_weak;
    releaser->token = impl_->semantic_token;
    releaser->slot_id = 0;
    return ProviderRegistration(std::move(releaser));
}

ProviderRegistration AutomationRuntime::register_activity_provider(IActivityProvider* provider) {
    if (!provider) return {};
    impl_->activity_provider = provider;
    impl_->activity_token = impl_->next_domain_token++;
    auto releaser = std::make_shared<ProviderRegistration::Releaser>();
    releaser->owner = impl_->self_weak;
    releaser->token = impl_->activity_token;
    releaser->slot_id = 1;
    return ProviderRegistration(std::move(releaser));
}

ProviderRegistration AutomationRuntime::register_diagnostics_provider(
    IDiagnosticProvider* provider) {
    if (!provider) return {};
    impl_->diagnostics_provider = provider;
    impl_->diagnostics_token = impl_->next_domain_token++;
    auto releaser = std::make_shared<ProviderRegistration::Releaser>();
    releaser->owner = impl_->self_weak;
    releaser->token = impl_->diagnostics_token;
    releaser->slot_id = 2;
    return ProviderRegistration(std::move(releaser));
}

void AutomationRuntime::set_policy(IAutomationPolicy* policy) { impl_->policy = policy; }

void AutomationRuntime::set_response_sink(ResponseSink sink) {
    std::lock_guard<std::mutex> guard(impl_->state_mutex);
    impl_->response_sink = std::move(sink);
}

void AutomationRuntime::set_wake(std::function<void()> wake) {
    std::lock_guard<std::mutex> guard(impl_->state_mutex);
    impl_->wake_callback = std::move(wake);
}

void AutomationRuntime::set_deferred_wake(std::function<void(double)> wake) {
    std::lock_guard<std::mutex> guard(impl_->state_mutex);
    impl_->deferred_wake_callback = std::move(wake);
}

void AutomationRuntime::receive(std::uint64_t connection, std::uint64_t transport_request,
                                std::string payload) {
    Impl& impl = *impl_;
    JsonValue doc;
    const JsonParseResult parsed = parse_json_strict(payload, impl.budgets.request_bytes, doc);
    if (parsed.status != JsonParseStatus::Ok) {
        Error error = Error::make(ErrorCode::ParseError,
                                  "payload is not valid JSON: " + parsed.detail);
        error.data.set("detail", parsed.detail);
        impl.respond_now(connection, transport_request, JsonValue{}, false,
                         MethodOutcome::failure(std::move(error)));
        return;
    }
    JsonValue id_echo{};
    bool has_id = false;
    std::string method;
    JsonValue params;
    Error error;
    if (!parse_envelope(doc, id_echo, has_id, method, params, error)) {
        impl.respond_now(connection, transport_request, id_echo, has_id,
                         MethodOutcome::failure(std::move(error)));
        return;
    }
    if (stopped_.load(std::memory_order_acquire)) {
        impl.respond_now(connection, transport_request, id_echo, has_id,
                         MethodOutcome::failure(
                             Error::make(ErrorCode::Cancelled, "runtime is shutting down")));
        return;
    }
    // Registry lookup: method must exist; snapshot the descriptor.
    MethodDescriptor descriptor;
    std::uint64_t domain_token = 0;
    {
        std::lock_guard<std::mutex> guard(impl.registry_mutex);
        const auto name_it = impl.method_domain.find(method);
        if (name_it == impl.method_domain.end()) {
            impl.respond_now(connection, transport_request, id_echo, has_id,
                             MethodOutcome::failure(Error::make(
                                 ErrorCode::MethodNotFound,
                                 "method '" + method + "' is not registered")));
            return;
        }
        domain_token = name_it->second;
        const auto domain_it = impl.domains.find(domain_token);
        if (domain_it != impl.domains.end()) {
            for (const MethodDescriptor& candidate : domain_it->second.methods) {
                if (candidate.name == method) {
                    descriptor = candidate;
                    break;
                }
            }
        }
    }
    {
        std::lock_guard<std::mutex> guard(impl.state_mutex);
        if (impl.queue.size() >= impl.budgets.queue_limit) {
            impl.respond_now(connection, transport_request, id_echo, has_id,
                             MethodOutcome::failure(Error::make(
                                 ErrorCode::Overloaded,
                                 "runtime queue is at its limit; retry after a delay")));
            return;
        }
        std::size_t inflight = 0;
        for (const PendingRequest& pending : impl.queue) {
            if (pending.connection == connection) ++inflight;
        }
        for (const auto& [slot, pending] : impl.executing) {
            (void)slot;
            if (pending.connection == connection) ++inflight;
        }
        if (inflight >= impl.budgets.inflight_per_session) {
            impl.respond_now(connection, transport_request, id_echo, has_id,
                             MethodOutcome::failure(Error::make(
                                 ErrorCode::Overloaded,
                                 "too many in-flight requests for this session")));
            return;
        }
        PendingRequest entry;
        entry.connection = connection;
        entry.transport_request = transport_request;
        entry.id_echo = id_echo;
        entry.has_id = has_id;
        entry.method = std::move(method);
        entry.params = std::move(params);
        entry.submitted = impl.clock();
        entry.deadline = entry.submitted + descriptor.timeout_s;
        entry.domain_token = domain_token;
        entry.slot = impl.next_slot++;
        entry.cancel_flag = std::make_shared<std::atomic<bool>>(false);
        impl.queue.push_back(std::move(entry));
    }
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> guard(impl.state_mutex);
        wake = impl.wake_callback;
    }
    if (wake) wake();
}

void AutomationRuntime::connection_closed(std::uint64_t connection) {
    Impl& impl = *impl_;
    std::vector<std::uint64_t> to_cancel;
    {
        std::lock_guard<std::mutex> guard(impl.state_mutex);
        impl.sessions.erase(connection);
        std::erase_if(impl.queue, [&](const PendingRequest& entry) {
            return entry.connection == connection;
        });
        for (const auto& [slot, entry] : impl.executing) {
            (void)entry;
            if (entry.connection == connection) to_cancel.push_back(slot);
        }
    }
    for (const std::uint64_t slot : to_cancel) {
        impl.settle(slot, MethodOutcome::failure(Error::make(
                              ErrorCode::Cancelled, "connection closed before completion")));
    }
}

void AutomationRuntime::shutdown(double) {
    Impl& impl = *impl_;
    stopped_.store(true, std::memory_order_release);
    std::vector<PendingRequest> drained;
    {
        std::lock_guard<std::mutex> guard(impl.state_mutex);
        while (!impl.queue.empty()) {
            drained.push_back(std::move(impl.queue.front()));
            impl.queue.pop_front();
        }
        for (auto& [slot, entry] : impl.executing) {
            (void)slot;
            drained.push_back(std::move(entry));
        }
        impl.executing.clear();
    }
    impl.conditions.clear();
    const Error cancelled = Error::make(ErrorCode::Cancelled, "runtime is shutting down");
    for (PendingRequest& entry : drained) {
        impl.respond_now(entry.connection, entry.transport_request, entry.id_echo, entry.has_id,
                         MethodOutcome::failure(cancelled));
    }
}

void AutomationRuntime::emit_event(AutomationEvent event) {
    impl_->append_event(std::move(event));
}

void AutomationRuntime::log(std::string_view level, std::string message, std::string request_id,
                            std::uint64_t operation) {
    impl_->log(level, std::move(message), std::move(request_id), operation);
}

std::uint64_t AutomationRuntime::start_operation(std::string method,
                                                 const RequestContext& context) {
    Impl& impl = *impl_;
    const std::uint64_t id = impl.next_operation++;
    OperationRecord record;
    record.info.id = id;
    record.info.method = std::move(method);
    record.info.connection = context.connection;
    record.info.started = impl.clock();
    record.cancel_flag = context.cancel_flag;
    impl.operations.emplace(id, std::move(record));
    AutomationEvent event;
    event.name = std::string(event_names::kOperationUpdated);
    event.operation = id;
    event.request_id = context.request_id;
    JsonValue details = JsonValue::object();
    details.set("state", "running");
    event.details = std::move(details);
    impl.append_event(std::move(event));
    return id;
}

void AutomationRuntime::finish_operation(std::uint64_t id, OperationState state, JsonValue result,
                                         Error error) {
    Impl& impl = *impl_;
    const auto it = impl.operations.find(id);
    if (it == impl.operations.end()) return;
    OperationRecord& record = it->second;
    if (record.info.state != OperationState::Running) return;
    record.info.state = state;
    record.info.finished = impl.clock();
    if (state == OperationState::Completed) record.info.result = std::move(result);
    if (state == OperationState::Failed || state == OperationState::Cancelled)
        record.info.error = std::move(error);
    AutomationEvent event;
    event.name = std::string(event_names::kOperationUpdated);
    event.operation = id;
    JsonValue details = JsonValue::object();
    switch (state) {
    case OperationState::Completed: details.set("state", "completed"); break;
    case OperationState::Failed: details.set("state", "failed"); break;
    case OperationState::Cancelled: details.set("state", "cancelled"); break;
    default: details.set("state", "running"); break;
    }
    event.details = std::move(details);
    impl.append_event(std::move(event));
    const auto waiters = std::move(record.waiters);
    record.waiters.clear();
    const OperationInfo snapshot = record.info;
    impl.terminal_operations.push_back(id);
    while (impl.terminal_operations.size() > impl.kTerminalOperationKeep) {
        impl.operations.erase(impl.terminal_operations.front());
        impl.terminal_operations.pop_front();
    }
    for (const auto& waiter : waiters) waiter(snapshot);
}

bool AutomationRuntime::get_operation(std::uint64_t id, OperationInfo& out) const {
    const auto it = impl_->operations.find(id);
    if (it == impl_->operations.end()) return false;
    out = it->second.info;
    return true;
}

// ---- drain ----

void AutomationRuntime::drain(double now) {
    Impl& impl = *impl_;
    if (stopped_.load(std::memory_order_acquire)) return;

    // 1. Sweep executing requests for deadline and cancellation.
    std::vector<std::uint64_t> expired;
    {
        std::lock_guard<std::mutex> guard(impl.state_mutex);
        for (const auto& [slot, entry] : impl.executing) {
            if ((entry.cancel_flag && entry.cancel_flag->load()) || now >= entry.deadline) {
                expired.push_back(slot);
            }
        }
    }
    for (const std::uint64_t slot : expired) impl.settle_deadline_or_cancel(slot);

    // 2. Evaluate wait conditions.
    impl.check_conditions(now);

    // 3. Start queued requests within the drain budget.
    const double started_at = impl.clock();
    const double budget = impl.budgets.drain_budget_ms / 1000.0;
    std::size_t launched = 0;
    while (launched < impl.budgets.drain_batch) {
        if (launched > 0 && impl.clock() - started_at >= budget) break;
        PendingRequest entry;
        {
            std::lock_guard<std::mutex> guard(impl.state_mutex);
            if (impl.queue.empty()) break;
            entry = std::move(impl.queue.front());
            impl.queue.pop_front();
        }
        if (stopped_.load(std::memory_order_acquire)) {
            impl.respond_now(entry.connection, entry.transport_request, entry.id_echo,
                             entry.has_id,
                             MethodOutcome::failure(
                                 Error::make(ErrorCode::Cancelled, "runtime is shutting down")));
            continue;
        }
        if (now >= entry.deadline) {
            Error error = Error::make(ErrorCode::DeadlineExceeded,
                                      "request expired before execution");
            error.data.set("requestId", request_id_of(entry.id_echo));
            impl.respond_now(entry.connection, entry.transport_request, entry.id_echo,
                             entry.has_id, MethodOutcome::failure(std::move(error)));
            impl.log("info", "expired queued: " + entry.method, request_id_of(entry.id_echo));
            continue;
        }
        // Snapshot the descriptor and handler.
        MethodHandler handler;
        MethodDescriptor descriptor;
        {
            std::lock_guard<std::mutex> guard(impl.registry_mutex);
            const auto domain_it = impl.domains.find(entry.domain_token);
            if (domain_it == impl.domains.end()) {
                impl.respond_now(entry.connection, entry.transport_request, entry.id_echo,
                                 entry.has_id,
                                 MethodOutcome::failure(Error::make(
                                     ErrorCode::ProviderUnavailable,
                                     "method provider was unregistered before execution")));
                impl.log("info", "dropped (provider gone): " + entry.method,
                         request_id_of(entry.id_echo));
                continue;
            }
            handler = domain_it->second.handler;
            for (const MethodDescriptor& candidate : domain_it->second.methods) {
                if (candidate.name == entry.method) {
                    descriptor = candidate;
                    break;
                }
            }
        }
        // Session authorization and grants. The policy object belongs to
        // the UI thread and must not re-enter the runtime. The connection
        // decision caches; grants re-query every request so hosts can
        // adjust them while a session stays open.
        SessionState session;
        {
            std::lock_guard<std::mutex> guard(impl.state_mutex);
            SessionState& stored = impl.sessions[entry.connection];
            if (!stored.authorized) {
                SessionInfo info;
                info.connection = entry.connection;
                stored.authorized = true;
                stored.denied = impl.policy && !impl.policy->authorize_session(info);
            }
            if (!stored.denied) {
                SessionInfo info;
                info.connection = entry.connection;
                stored.grants = impl.policy
                                    ? impl.policy->grants(info)
                                    : std::vector<std::string>{std::string(kGrantObserve),
                                                               std::string(kGrantInput),
                                                               std::string(kGrantWindowTab)};
            }
            session = stored;
        }
        if (session.denied) {
            impl.respond_now(entry.connection, entry.transport_request, entry.id_echo,
                             entry.has_id,
                             MethodOutcome::failure(Error::make(
                                 ErrorCode::PermissionDenied,
                                 "the connection was not authorized by the host policy")));
            continue;
        }
        bool granted = true;
        for (const std::string& needed : descriptor.grants) {
            if (std::find(session.grants.begin(), session.grants.end(), needed) ==
                session.grants.end()) {
                granted = false;
                break;
            }
        }
        if (!granted) {
            Error error = Error::make(ErrorCode::PermissionDenied,
                                      "session lacks the grants required by " + entry.method);
            JsonValue grants_list = JsonValue::array();
            for (const std::string& needed : descriptor.grants) grants_list.push_back(needed);
            error.data.set("grants", std::move(grants_list));
            impl.respond_now(entry.connection, entry.transport_request, entry.id_echo,
                             entry.has_id, MethodOutcome::failure(std::move(error)));
            continue;
        }
        if (impl.policy) {
            SessionInfo info;
            info.connection = entry.connection;
            if (!impl.policy->allow_method(info, descriptor)) {
                impl.respond_now(entry.connection, entry.transport_request, entry.id_echo,
                                 entry.has_id,
                                 MethodOutcome::failure(Error::make(
                                     ErrorCode::PermissionDenied,
                                     "the host policy rejected " + entry.method)));
                continue;
            }
        }
        // Parameter validation against the declared schema.
        if (descriptor.params.is_object()) {
            const SchemaValidationResult valid =
                validate_against_schema(entry.params, descriptor.params, "params");
            if (!valid.ok) {
                Error error = Error::invalid_params(valid.error);
                error.data.set("method", entry.method);
                impl.respond_now(entry.connection, entry.transport_request, entry.id_echo,
                                 entry.has_id, MethodOutcome::failure(std::move(error)));
                continue;
            }
        }
        // Build the context, move the entry into executing, invoke.
        RequestContext context;
        context.request_id = request_id_of(entry.id_echo);
        context.method = entry.method;
        context.params = entry.params;
        context.connection = entry.connection;
        context.slot = entry.slot;
        context.submitted = entry.submitted;
        context.deadline = entry.deadline;
        context.operation_id = entry.operation;
        context.cancel_flag = entry.cancel_flag;
        const std::uint64_t slot = entry.slot;
        {
            std::lock_guard<std::mutex> guard(impl.state_mutex);
            impl.executing.emplace(slot, std::move(entry));
        }
        impl.log("debug", "executing " + context.method, context.request_id);
        handler(context, [impl_weak = impl_->self_weak, slot](MethodOutcome outcome) {
            if (const std::shared_ptr<Impl> target = impl_weak.lock()) {
                target->settle(slot, std::move(outcome));
            }
        });
        ++launched;
    }
    // Budget exhausted with work remaining: request another drain (wake
    // calls coalesce, so this cannot spin without progress).
    bool more = false;
    {
        std::lock_guard<std::mutex> guard(impl.state_mutex);
        more = !impl.queue.empty();
    }
    if (more) {
        std::function<void()> wake;
        {
            std::lock_guard<std::mutex> guard(impl.state_mutex);
            wake = impl.wake_callback;
        }
        if (wake) wake();
    }
    impl.schedule_wait_wake(now);
}

// ---- internal engine domains ----

MethodDescriptor AutomationRuntime::make_descriptor(
    std::string name, std::string summary, std::vector<std::string> grants, bool mutating,
    bool idempotent, double timeout_s, JsonValue params, JsonValue result) {
    MethodDescriptor descriptor;
    descriptor.name = std::move(name);
    descriptor.summary = std::move(summary);
    descriptor.grants = std::move(grants);
    descriptor.mutating = mutating;
    descriptor.idempotent = idempotent;
    descriptor.timeout_s = timeout_s;
    descriptor.params = std::move(params);
    descriptor.result = std::move(result);
    return descriptor;
}

void AutomationRuntime::register_internal_domains() {
    Impl& impl = *impl_;
    const std::string observe{std::string(kGrantObserve)};
    const std::string input{std::string(kGrantInput)};

    // System domain.
    {
        std::vector<MethodDescriptor> methods;
        methods.push_back(make_descriptor(
            "System.getInfo", "Instance identity, versions, renderer and budgets.", {observe},
            false, true, 10.0, schema_object({}), JsonValue::object()));
        methods.push_back(make_descriptor(
            "System.getCapabilities", "Capability block for this instance and its windows.",
            {observe}, false, true, 10.0, schema_object({}), JsonValue::object()));
        methods.push_back(make_descriptor(
            "System.getSchema", "Machine-readable method/event/error schema.", {observe}, false,
            true, 10.0, schema_object({}), JsonValue::object()));
        MethodHandler handler = [this](const RequestContext& context, CompletionSink sink) {
            if (context.method == "System.getInfo") {
                JsonValue result = JsonValue::object();
                result.set("protocolVersion", impl_->identity.protocol_version);
                result.set("engineVersion", impl_->identity.engine_version);
                result.set("sdkBuild", impl_->identity.sdk_build);
                result.set("instanceId", impl_->identity.instance_id);
                result.set("application", impl_->identity.application);
                result.set("appVersion", impl_->identity.app_version);
                result.set("capabilities", impl_->capabilities);
                sink(MethodOutcome::success(std::move(result)));
                return;
            }
            if (context.method == "System.getCapabilities") {
                sink(MethodOutcome::success(impl_->capabilities));
                return;
            }
            sink(MethodOutcome::success(impl_->build_schema_document()));
        };
        impl_->install_domain("System", std::move(methods), std::move(handler), true);
    }

    // Event.poll
    {
        std::vector<MethodDescriptor> methods;
        methods.push_back(make_descriptor(
            "Event.poll",
            "Poll observation events newer than a sequence number. A gap means re-synchronize.",
            {observe}, false, true, 10.0,
            schema_object({
                {"since", integer_property("Last sequence seen (0 for everything retained)", 0)},
                {"max", integer_property("Maximum events returned", 1, 4096)},
                {"windowId", string_property("Restrict to one window (decimal id string)")},
            }),
            JsonValue::object()));
        MethodHandler handler = [this](const RequestContext& context, CompletionSink sink) {
            std::uint64_t since = 0;
            std::size_t max = impl_->budgets.poll_batch;
            if (const JsonValue* value = context.params.find("since");
                value && value->is_integer()) {
                since = value->as_integer() > 0 ? static_cast<std::uint64_t>(value->as_integer())
                                                : 0;
            }
            if (const JsonValue* value = context.params.find("max");
                value && value->is_integer() && value->as_integer() > 0) {
                max = std::min<std::size_t>(static_cast<std::size_t>(value->as_integer()),
                                            impl_->budgets.poll_batch);
            }
            WindowId window = 0;
            if (const JsonValue* value = context.params.find("windowId");
                value && value->is_string()) {
                if (!id_from_wire(value->as_string(), window)) {
                    sink(MethodOutcome::failure(Error::invalid_params(
                        "params.windowId: not a decimal id string")));
                    return;
                }
            }
            JsonValue list = JsonValue::array();
            std::uint64_t next = since;
            bool gap = false;
            {
                std::lock_guard<std::mutex> guard(impl_->event_mutex);
                if (since > 0 && !impl_->events.empty() &&
                    impl_->oldest_sequence > since + 1) {
                    gap = true;
                }
                for (const AutomationEvent& event : impl_->events) {
                    if (event.sequence <= since) continue;
                    if (window != 0 && event.window != window) continue;
                    if (list.size() >= max) break;
                    list.push_back(event_to_wire(event));
                    next = event.sequence;
                }
            }
            JsonValue result = JsonValue::object();
            result.set("events", std::move(list));
            result.set("next", static_cast<std::int64_t>(next));
            result.set("gap", gap);
            std::uint64_t oldest = 0;
            {
                std::lock_guard<std::mutex> guard(impl_->event_mutex);
                oldest = impl_->events.empty() ? impl_->next_sequence : impl_->oldest_sequence;
            }
            result.set("oldestAvailable", static_cast<std::int64_t>(oldest));
            sink(MethodOutcome::success(std::move(result)));
        };
        impl_->install_domain("Event", std::move(methods), std::move(handler), true);
    }

    // Operation domain.
    {
        std::vector<MethodDescriptor> methods;
        methods.push_back(make_descriptor(
            "Operation.get", "Read one operation's status.", {observe}, false, true, 10.0,
            schema_object({{"operationId", string_property("Operation id (decimal string)")}},
                          {"operationId"}),
            JsonValue::object()));
        methods.push_back(make_descriptor(
            "Operation.wait", "Wait until one operation finishes; returns its final state.",
            {observe}, false, false, 60.0,
            schema_object({
                {"operationId", string_property("Operation id (decimal string)")},
                {"timeoutS", number_property("Maximum wait; capped by the request deadline")},
            },
                          {"operationId"}),
            JsonValue::object()));
        methods.push_back(make_descriptor(
            "Operation.cancel",
            "Cooperative cancel: sets the cancellation flag and marks the operation cancelled.",
            {observe}, true, false, 10.0,
            schema_object({{"operationId", string_property("Operation id (decimal string)")}},
                          {"operationId"}),
            JsonValue::object()));
        MethodHandler handler = [this](const RequestContext& context, CompletionSink sink) {
            const JsonValue* id_value = context.params.find("operationId");
            std::uint64_t id = 0;
            if (!id_value || !id_value->is_string() || !id_from_wire(id_value->as_string(), id)) {
                sink(MethodOutcome::failure(
                    Error::invalid_params("params.operationId: not a decimal id string")));
                return;
            }
            if (context.method == "Operation.get") {
                OperationInfo info;
                if (!get_operation(id, info)) {
                    sink(MethodOutcome::failure(
                        Error::make(ErrorCode::OperationNotFound,
                                    "no operation with id " + id_value->as_string())));
                    return;
                }
                sink(MethodOutcome::success(operation_to_wire(info)));
                return;
            }
            const auto it = impl_->operations.find(id);
            if (it == impl_->operations.end()) {
                sink(MethodOutcome::failure(
                    Error::make(ErrorCode::OperationNotFound,
                                "no operation with id " + id_value->as_string())));
                return;
            }
            if (context.method == "Operation.wait") {
                const OperationRecord& record = it->second;
                if (record.info.state != OperationState::Running) {
                    if (record.info.state == OperationState::Completed) {
                        sink(MethodOutcome::success(operation_to_wire(record.info)));
                    } else {
                        sink(MethodOutcome::failure(Error::make(
                            ErrorCode::InvalidOperationState,
                            "operation finished with state " +
                                std::string(record.info.state == OperationState::Failed
                                                ? "failed"
                                                : "cancelled"))));
                    }
                    return;
                }
                // Custom timeout: extend this request's deadline.
                double timeout_s = impl_->budgets.request_timeout_s;
                if (const JsonValue* value = context.params.find("timeoutS");
                    value && value->is_number() && value->as_number() > 0.0) {
                    timeout_s = std::min(value->as_number(), 3600.0);
                }
                {
                    std::lock_guard<std::mutex> guard(impl_->state_mutex);
                    const auto entry_it = impl_->executing.find(context.slot);
                    if (entry_it != impl_->executing.end()) {
                        entry_it->second.deadline = impl_->clock() + timeout_s;
                    }
                }
                // Handler-side timeout only works while drains happen; the
                // deferred wake keeps them coming.
                impl_->conditions.push_back(ConditionRecord{});
                ConditionRecord& record_ref = impl_->conditions.back();
                record_ref.slot = context.slot;
                record_ref.type = "operationWait";
                record_ref.submitted = impl_->clock();
                record_ref.next_poll = impl_->clock() + kWaitPollSeconds;
                record_ref.operation = id;
                record_ref.check = [this, id](std::string& describe) {
                    const auto op_it = impl_->operations.find(id);
                    if (op_it == impl_->operations.end()) {
                        // Terminal records are erased after finish; treat
                        // disappearance as completed (Operation.get can no
                        // longer see it either).
                        describe = "operation finished";
                        return true;
                    }
                    if (op_it->second.info.state != OperationState::Running) {
                        describe = "operation finished";
                        return true;
                    }
                    describe = "operation still running";
                    return false;
                };
                sink = CompletionSink{};   // deferred; never settled here
                // Guard against a captured-null sink misuse: mark deferred.
                return;
            }
            // Operation.cancel
            OperationRecord& record = it->second;
            if (record.info.state != OperationState::Running) {
                sink(MethodOutcome::failure(Error::make(
                    ErrorCode::InvalidOperationState,
                    "operation already finished; executed side effects are not rolled back")));
                return;
            }
            const std::string method = record.info.method;
            if (record.cancel_flag) record.cancel_flag->store(true);
            finish_operation(id, OperationState::Cancelled, JsonValue{},
                             Error::make(ErrorCode::Cancelled, "cancelled via Operation.cancel"));
            JsonValue result = JsonValue::object();
            result.set("operationId", id_to_wire(id));
            result.set("method", method);
            result.set("state", "cancelled");
            result.set("note", "cooperative cancel: running work observes the flag; side "
                               "effects already executed are not rolled back");
            sink(MethodOutcome::success(std::move(result)));
        };
        impl_->install_domain("Operation", std::move(methods), std::move(handler), true);
    }

    // Wait.condition
    {
        std::vector<MethodDescriptor> methods;
        methods.push_back(make_descriptor(
            "Wait.condition",
            "Wait for a typed condition: activityQuiet, nodeAppears, operationDone, "
            "animationSettled or frameStable. Non-blocking; checked on UI drains.",
            {observe}, false, false, 60.0,
            schema_object({
                {"condition",
                 enum_property("Condition kind", {"activityQuiet", "nodeAppears", "operationDone",
                                                  "animationSettled", "frameStable"})},
                {"windowId", string_property("Target window (decimal id string)")},
                {"scope", string_property("Activity scope for activityQuiet")},
                {"stableKey", string_property("Business identity for nodeAppears")},
                {"operationId", string_property("Operation id for operationDone (decimal)")},
                {"dwellS", number_property("Seconds the condition must hold (activityQuiet)")},
                {"frames", integer_property("Frames to pass for frameStable", 1)},
                {"timeoutS", number_property("Maximum wait; enforced as the request deadline")},
            },
                          {"condition"}),
            JsonValue::object()));
        MethodHandler handler = [this](const RequestContext& context, CompletionSink sink) {
            const std::string condition =
                context.params.find("condition") != nullptr &&
                        context.params.find("condition")->is_string()
                    ? context.params.find("condition")->as_string()
                    : std::string{};
            double timeout_s = impl_->budgets.request_timeout_s;
            if (const JsonValue* value = context.params.find("timeoutS");
                value && value->is_number() && value->as_number() > 0.0) {
                timeout_s = value->as_number();
            }
            double dwell = 0.0;
            if (const JsonValue* value = context.params.find("dwellS");
                value && value->is_number() && value->as_number() > 0.0) {
                dwell = std::min(value->as_number(), 60.0);
            }
            std::uint64_t frames = 1;
            if (const JsonValue* value = context.params.find("frames");
                value && value->is_integer() && value->as_integer() > 0) {
                frames = static_cast<std::uint64_t>(value->as_integer());
            }
            WindowId window = 0;
            if (const JsonValue* value = context.params.find("windowId");
                value && value->is_string()) {
                if (!id_from_wire(value->as_string(), window)) {
                    sink(MethodOutcome::failure(
                        Error::invalid_params("params.windowId: not a decimal id string")));
                    return;
                }
            }
            std::string scope;
            if (const JsonValue* value = context.params.find("scope");
                value && value->is_string()) {
                scope = value->as_string();
            }
            std::string stable_key;
            if (const JsonValue* value = context.params.find("stableKey");
                value && value->is_string()) {
                stable_key = value->as_string();
            }
            std::uint64_t operation = 0;
            if (const JsonValue* value = context.params.find("operationId");
                value && value->is_string()) {
                if (!id_from_wire(value->as_string(), operation)) {
                    sink(MethodOutcome::failure(Error::invalid_params(
                        "params.operationId: not a decimal id string")));
                    return;
                }
            }

            ConditionRecord record;
            record.slot = context.slot;
            record.type = condition;
            record.submitted = impl_->clock();
            record.next_poll = record.submitted + kWaitPollSeconds;
            record.dwell = dwell;
            record.window = window;
            record.scope = scope;
            record.stable_key = stable_key;
            record.operation = operation;

            if (condition == "activityQuiet") {
                if (!impl_->activity_provider) {
                    sink(MethodOutcome::failure(Error::unsupported(
                        "no activity provider is registered for this instance")));
                    return;
                }
                ActivityReport report;
                if (!impl_->activity_provider->activity(scope, report)) {
                    sink(MethodOutcome::failure(
                        Error::unsupported("unknown activity scope '" + scope + "'")));
                    return;
                }
                record.check = [this, scope](std::string& describe) {
                    ActivityReport current;
                    if (!impl_->activity_provider->activity(scope, current)) return false;
                    describe = "scope '" + scope + "' has " +
                               std::to_string(current.pending) + " pending tasks";
                    return current.pending == 0;
                };
            } else if (condition == "nodeAppears") {
                if (!impl_->env.node_exists) {
                    sink(MethodOutcome::failure(Error::unsupported(
                        "no semantic provider observes this instance")));
                    return;
                }
                record.check = [this, window, stable_key](std::string& describe) {
                    const bool exists = impl_->env.node_exists(window, stable_key);
                    describe = exists ? "node '" + stable_key + "' present"
                                      : "node '" + stable_key + "' not present yet";
                    return exists;
                };
            } else if (condition == "operationDone") {
                const auto op_it = impl_->operations.find(operation);
                if (op_it == impl_->operations.end()) {
                    sink(MethodOutcome::failure(Error::make(
                        ErrorCode::OperationNotFound,
                        "no operation with id " + id_to_wire(operation))));
                    return;
                }
                record.check = [this, operation](std::string& describe) {
                    const auto target = impl_->operations.find(operation);
                    if (target == impl_->operations.end() ||
                        target->second.info.state != OperationState::Running) {
                        describe = "operation finished";
                        return true;
                    }
                    describe = "operation still running";
                    return false;
                };
            } else if (condition == "animationSettled") {
                if (!impl_->env.animation_active) {
                    sink(MethodOutcome::failure(
                        Error::unsupported("no shell bridge reports animation state")));
                    return;
                }
                record.check = [this, window](std::string& describe) {
                    const bool active = impl_->env.animation_active(window);
                    describe = active ? "animations still running" : "animations settled";
                    return !active;
                };
            } else if (condition == "frameStable") {
                if (!impl_->env.frame_serial) {
                    sink(MethodOutcome::failure(
                        Error::unsupported("no shell bridge reports frame serials")));
                    return;
                }
                const std::uint64_t baseline = impl_->env.frame_serial(window);
                record.frame_target = baseline + frames;
                record.check = [this, window, target = record.frame_target](
                                   std::string& describe) {
                    const std::uint64_t serial = impl_->env.frame_serial(window);
                    describe = "frame " + std::to_string(serial) + " of target " +
                               std::to_string(target);
                    return serial >= target && target > 0;
                };
            } else {
                sink(MethodOutcome::failure(
                    Error::invalid_params("params.condition: unknown condition kind")));
                return;
            }
            // Extend this request's deadline to the requested timeout.
            {
                std::lock_guard<std::mutex> guard(impl_->state_mutex);
                const auto entry_it = impl_->executing.find(context.slot);
                if (entry_it != impl_->executing.end()) {
                    entry_it->second.deadline = impl_->clock() + timeout_s;
                }
            }
            impl_->conditions.push_back(std::move(record));
            // The request settles when check_conditions sees the condition
            // satisfied; `sink` stays uncalled here.
        };
        impl_->install_domain("Wait", std::move(methods), std::move(handler), true);
    }

    // Log.read
    {
        std::vector<MethodDescriptor> methods;
        methods.push_back(make_descriptor(
            "Log.read", "Paged engine and application diagnostics; sensitive fields are "
                        "excluded by the sources themselves.",
            {observe}, false, true, 10.0,
            schema_object({
                {"after", integer_property("Engine-log sequence cursor", 0)},
                {"appAfter", integer_property("Application-log sequence cursor", 0)},
                {"max", integer_property("Maximum records per source", 1, kMaxLogBatch)},
                {"source", enum_property("Restrict to one source", {"engine", "app"})},
            }),
            JsonValue::object()));
        MethodHandler handler = [this](const RequestContext& context, CompletionSink sink) {
            std::uint64_t after = 0;
            std::uint64_t app_after = 0;
            std::size_t max = 128;
            std::string source;
            if (const JsonValue* value = context.params.find("after");
                value && value->is_integer() && value->as_integer() > 0) {
                after = static_cast<std::uint64_t>(value->as_integer());
            }
            if (const JsonValue* value = context.params.find("appAfter");
                value && value->is_integer() && value->as_integer() > 0) {
                app_after = static_cast<std::uint64_t>(value->as_integer());
            }
            if (const JsonValue* value = context.params.find("max");
                value && value->is_integer() && value->as_integer() > 0) {
                max = std::min<std::size_t>(static_cast<std::size_t>(value->as_integer()),
                                            kMaxLogBatch);
            }
            if (const JsonValue* value = context.params.find("source");
                value && value->is_string()) {
                source = value->as_string();
            }
            JsonValue sources = JsonValue::object();
            if (source.empty() || source == "engine") {
                JsonValue list = JsonValue::array();
                std::uint64_t next = after;
                {
                    std::lock_guard<std::mutex> guard(impl_->event_mutex);
                    for (const LogEntry& entry : impl_->log_ring) {
                        if (entry.sequence <= after) continue;
                        if (list.size() >= max) break;
                        list.push_back(log_to_wire(entry));
                        next = entry.sequence;
                    }
                }
                JsonValue engine = JsonValue::object();
                engine.set("records", std::move(list));
                engine.set("next", static_cast<std::int64_t>(next));
                sources.set("engine", std::move(engine));
            }
            if (source.empty() || source == "app") {
                JsonValue list = JsonValue::array();
                std::uint64_t next = app_after;
                if (impl_->diagnostics_provider) {
                    std::vector<LogRecord> records;
                    if (impl_->diagnostics_provider->read_logs(app_after, max, records)) {
                        for (LogRecord& record : records) {
                            JsonValue wire = JsonValue::object();
                            wire.set("sequence", static_cast<std::int64_t>(record.sequence));
                            wire.set("at", record.at);
                            wire.set("level", record.level);
                            wire.set("source", record.source);
                            wire.set("message", record.message);
                            if (!record.request_id.empty())
                                wire.set("requestId", record.request_id);
                            if (record.operation)
                                wire.set("operationId", id_to_wire(record.operation));
                            list.push_back(std::move(wire));
                            next = std::max(next, record.sequence);
                        }
                    }
                }
                JsonValue app = JsonValue::object();
                app.set("records", std::move(list));
                app.set("next", static_cast<std::int64_t>(next));
                sources.set("app", std::move(app));
            }
            JsonValue result = JsonValue::object();
            result.set("sources", std::move(sources));
            sink(MethodOutcome::success(std::move(result)));
        };
        impl_->install_domain("Log", std::move(methods), std::move(handler), true);
    }
}

// Explicit instantiation points kept out of headers; keeps the linker happy
// for the registration value types defined here.
static_assert(std::is_nothrow_move_constructible_v<MethodRegistration>);
static_assert(std::is_nothrow_move_constructible_v<ProviderRegistration>);

} // namespace tabengine::automation
