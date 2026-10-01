#include "tabengine/automation/errors.h"

#include "tabengine/automation/method.h"

#include <utility>

namespace tabengine::automation {

std::string_view error_type_name(ErrorCode code) {
    switch (code) {
    case ErrorCode::ParseError: return "ParseError";
    case ErrorCode::InvalidRequest: return "InvalidRequest";
    case ErrorCode::MethodNotFound: return "MethodNotFound";
    case ErrorCode::InvalidParams: return "InvalidParams";
    case ErrorCode::InternalError: return "InternalError";
    case ErrorCode::Unsupported: return "Unsupported";
    case ErrorCode::PermissionDenied: return "PermissionDenied";
    case ErrorCode::TargetNotFound: return "TargetNotFound";
    case ErrorCode::StaleNode: return "StaleNode";
    case ErrorCode::NotInteractable: return "NotInteractable";
    case ErrorCode::AmbiguousTarget: return "AmbiguousTarget";
    case ErrorCode::DeadlineExceeded: return "DeadlineExceeded";
    case ErrorCode::Cancelled: return "Cancelled";
    case ErrorCode::ProviderUnavailable: return "ProviderUnavailable";
    case ErrorCode::Overloaded: return "Overloaded";
    case ErrorCode::SurfaceUnavailable: return "SurfaceUnavailable";
    case ErrorCode::DeviceLost: return "DeviceLost";
    case ErrorCode::OperationNotFound: return "OperationNotFound";
    case ErrorCode::InvalidOperationState: return "InvalidOperationState";
    case ErrorCode::BudgetExceeded: return "BudgetExceeded";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::SessionConflict: return "SessionConflict";
    case ErrorCode::InvalidSession: return "InvalidSession";
    }
    return "InternalError";
}

Error Error::make(ErrorCode code, std::string message) {
    Error error;
    error.code = code;
    error.message = std::move(message);
    error_set_data(error);
    return error;
}

Error Error::unsupported(std::string what) {
    return make(ErrorCode::Unsupported, std::move(what));
}

Error Error::invalid_params(std::string message) {
    return make(ErrorCode::InvalidParams, std::move(message));
}

Error Error::target_not_found(std::string what) {
    return make(ErrorCode::TargetNotFound, std::move(what));
}

Error Error::stale_node(std::string what) {
    return make(ErrorCode::StaleNode, std::move(what));
}

void error_set_data(Error& error, std::string_view type_override) {
    if (!error.data.is_object()) error.data = JsonValue::object();
    error.data.set("type", std::string(type_override.empty()
                                           ? error_type_name(error.code)
                                           : type_override));
}

MethodOutcome MethodOutcome::success(JsonValue result) {
    MethodOutcome outcome;
    outcome.ok = true;
    outcome.result = std::move(result);
    return outcome;
}

MethodOutcome MethodOutcome::failure(Error error) {
    MethodOutcome outcome;
    outcome.ok = false;
    outcome.error = std::move(error);
    return outcome;
}

} // namespace tabengine::automation
