#pragma once

// Method schema subset. Every method declares its parameters and result as
// a JSON schema restricted to this vocabulary; the same declaration drives
// dispatch-time validation and the /automation/v1/schema document, so a
// method cannot be registered without a machine-readable contract.
//
// Supported keywords: type ("object" | "array" | "string" | "integer" |
// "number" | "boolean" | "null" | "any"), properties, required (per
// property), items, enum, minimum, maximum, minLength, maxLength, minItems,
// maxItems, description. Objects reject undeclared properties by default
// (additionalProperties: false is implied and cannot be relaxed in v1).

#include "tabengine/automation/values.h"

#include <string>

namespace tabengine::automation {

struct SchemaValidationResult {
    bool ok = false;
    std::string error;   // path-qualified, e.g. "params.windowId: expected string"
};

SchemaValidationResult validate_against_schema(const JsonValue& instance, const JsonValue& schema,
                                               std::string_view path = "params");

} // namespace tabengine::automation
