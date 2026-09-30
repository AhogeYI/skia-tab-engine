#pragma once

// Error contract for TabEngine Automation. JSON-RPC reserves the negative
// codes; engine and provider errors live in a separate positive space so a
// transport-level failure is never mistaken for a business result.

#include "tabengine/automation/values.h"

#include <string>
#include <string_view>

namespace tabengine::automation {

enum class ErrorCode {
    // JSON-RPC 2.0 reserved codes (transport/envelope level).
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,

    // Engine and provider business codes. Names map 1:1 to error.data.type.
    Unsupported = 1000,
    PermissionDenied = 1001,
    TargetNotFound = 1002,
    StaleNode = 1003,
    NotInteractable = 1004,
    AmbiguousTarget = 1005,
    DeadlineExceeded = 1006,
    Cancelled = 1007,
    ProviderUnavailable = 1008,
    Overloaded = 1009,
    SurfaceUnavailable = 1010,
    DeviceLost = 1011,
    OperationNotFound = 1012,
    InvalidOperationState = 1013,
    BudgetExceeded = 1014,
    InvalidArgument = 1016,
    SessionConflict = 1017,
    InvalidSession = 1018,
};

// Stable wire name of an error code, e.g. "StaleNode" or "InvalidParams".
// Used for error.data.type and for the schema's error catalogue.
[[nodiscard]] std::string_view error_type_name(ErrorCode code);

struct Error {
    ErrorCode code = ErrorCode::InternalError;
    std::string message;   // English, operator-readable; never a raw exception
    JsonValue data;        // object: type, target fields, requestId, operationId,
                           // recoverable flag and hint. No stacks, no pointers.

    [[nodiscard]] static Error make(ErrorCode code, std::string message);
    [[nodiscard]] static Error unsupported(std::string what);
    [[nodiscard]] static Error invalid_params(std::string message);
    [[nodiscard]] static Error target_not_found(std::string what);
    [[nodiscard]] static Error stale_node(std::string what);
};

// Attach the standard data envelope fields. Safe to call repeatedly.
void error_set_data(Error& error, std::string_view type_override = {});

} // namespace tabengine::automation
