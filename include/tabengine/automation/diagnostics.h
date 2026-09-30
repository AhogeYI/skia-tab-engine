#pragma once

// Diagnostics contract: paged logs and a free-form diagnostic snapshot.
// The engine records its own bounded log (method/target/status/duration,
// correlated by request and operation); an application provider can expose
// its existing log through the same read path. Sensitive content (tokens,
// document bodies, terminal output, file paths) is the provider's
// responsibility to keep out; the engine never logs request payloads.

#include "tabengine/automation/values.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tabengine::automation {

struct LogRecord {
    std::uint64_t sequence = 0;   // source-assigned read cursor; read_logs
                                  // returns records with sequence > after_sequence
    double at = 0.0;          // monotonic seconds
    std::string level;        // "debug" | "info" | "warning" | "error"
    std::string source;       // "engine" or provider-chosen source name
    std::string message;      // single line, pre-sanitized
    std::string request_id;   // correlation when applicable
    std::uint64_t operation = 0;
};

// A stable cursor: records are addressable by the source-assigned sequence.
// read_logs returns records with sequence strictly greater than
// `after_sequence` in ascending order, at most `max` of them.
class IDiagnosticProvider {
public:
    virtual ~IDiagnosticProvider() = default;
    virtual bool read_logs(std::uint64_t after_sequence, std::size_t max,
                           std::vector<LogRecord>& out) = 0;
    // Structured snapshot for support tooling (queue depths, counters).
    // Providers must not include secrets; the engine merges this into
    // Log.read's response unchanged.
    [[nodiscard]] virtual JsonValue diagnostic_snapshot() { return JsonValue::object(); }
};

} // namespace tabengine::automation
