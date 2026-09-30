#pragma once

// Bounded in-memory artifact store for binary results (screenshots today,
// traces later). Entries carry size, SHA-256, media type, capture target
// metadata and a TTL; the store evicts oldest-first at the byte budget so a
// client can never grow engine memory without bound, and the transport
// serves bytes back by id. Clients choose where to save them - the engine
// never writes to a client-supplied path.

#include "tabengine/automation/values.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tabengine::automation {

class ArtifactStore {
public:
    struct Entry {
        std::string id;
        std::string media_type;
        std::vector<std::uint8_t> bytes;
        std::string sha256;      // lowercase hex
        double created = 0.0;    // monotonic seconds
        double ttl = 0.0;        // seconds; <= 0 means "no expiry while resident"
        JsonValue target;        // free-form target metadata (window, bounds, backend)
    };

    ArtifactStore(std::size_t max_bytes, double ttl_seconds);

    // Reconfigure at construction time only (host budgets).
    void configure(std::size_t max_bytes, double ttl_seconds);

    // Stores one artifact; returns its id, or an empty string when the
    // payload alone exceeds the byte budget. Evicts expired and then
    // oldest entries to make room.
    std::string store(std::vector<std::uint8_t> bytes, std::string media_type, JsonValue target,
                      double now);

    // Copies the entry out when it exists and has not expired.
    [[nodiscard]] bool get(std::string_view id, Entry& out) const;
    // Metadata-only view for Artifact.get responses.
    [[nodiscard]] bool metadata(std::string_view id, JsonValue& out) const;
    // Drops expired entries; called before every store and by the runtime
    // drain so a TTL always takes effect without external traffic.
    void expire(double now);

    [[nodiscard]] std::size_t bytes_used() const { return bytes_used_; }
    [[nodiscard]] std::size_t count() const { return entries_.size(); }

private:
    void evict_oldest();
    void expire_locked(double now);   // caller holds mutex_

    mutable std::mutex mutex_;
    std::size_t max_bytes_;
    double ttl_seconds_;
    std::size_t bytes_used_ = 0;
    std::uint64_t next_id_ = 1;
    std::unordered_map<std::string, Entry> entries_;
    std::deque<std::string> order_;
};

// Lowercase hex SHA-256 of a byte range.
[[nodiscard]] std::string sha256_hex(const std::uint8_t* data, std::size_t size);

} // namespace tabengine::automation
