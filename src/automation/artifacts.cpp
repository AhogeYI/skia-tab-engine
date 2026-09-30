#include "tabengine/automation/artifacts.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

namespace tabengine::automation {
namespace {

// Compact SHA-256 (FIPS 180-4): artifact integrity metadata only, not a
// security boundary.
struct Sha256 {
    std::uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::uint64_t bit_count = 0;
    std::uint8_t buffer[64] = {};
    std::size_t buffered = 0;

    static std::uint32_t rotate_right(std::uint32_t value, int bits) {
        return (value >> bits) | (value << (32 - bits));
    }

    void transform(const std::uint8_t* block) {
        static const std::uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
            0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
            0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
            0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
            0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
            0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18) ^
                                     (w[i - 15] >> 3);
            const std::uint32_t s1 = rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19) ^
                                     (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    void update(const std::uint8_t* data, std::size_t size) {
        bit_count += static_cast<std::uint64_t>(size) * 8;
        while (size > 0) {
            const std::size_t take = std::min<std::size_t>(64 - buffered, size);
            std::memcpy(buffer + buffered, data, take);
            buffered += take;
            data += take;
            size -= take;
            if (buffered == 64) {
                transform(buffer);
                buffered = 0;
            }
        }
    }

    void finalize(std::uint8_t out[32]) {
        const std::uint64_t bits = bit_count;
        const std::uint8_t pad = 0x80;
        update(&pad, 1);
        const std::uint8_t zero = 0;
        while (buffered != 56) update(&zero, 1);
        std::uint8_t length[8];
        for (int i = 0; i < 8; ++i)
            length[i] = static_cast<std::uint8_t>(bits >> (56 - i * 8));
        // update() counts these bytes; the length field bypasses counting by
        // writing the buffer directly.
        std::memcpy(buffer + buffered, length, 8);
        transform(buffer);
        buffered = 0;
        for (int i = 0; i < 8; ++i) {
            out[i * 4] = static_cast<std::uint8_t>(state[i] >> 24);
            out[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16);
            out[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8);
            out[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
        }
    }
};

} // namespace

std::string sha256_hex(const std::uint8_t* data, std::size_t size) {
    Sha256 digest;
    digest.update(data, size);
    std::uint8_t bytes[32];
    digest.finalize(bytes);
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const std::uint8_t byte : bytes) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

ArtifactStore::ArtifactStore(std::size_t max_bytes, double ttl_seconds)
    : max_bytes_(max_bytes), ttl_seconds_(ttl_seconds) {}

void ArtifactStore::configure(std::size_t max_bytes, double ttl_seconds) {
    std::lock_guard<std::mutex> guard(mutex_);
    max_bytes_ = max_bytes;
    ttl_seconds_ = ttl_seconds;
    while (bytes_used_ > max_bytes_ && !entries_.empty()) evict_oldest();
}

void ArtifactStore::expire(double now) {
    std::lock_guard<std::mutex> guard(mutex_);
    expire_locked(now);
}

void ArtifactStore::expire_locked(double now) {
    for (auto it = order_.begin(); it != order_.end();) {
        const auto entry = entries_.find(*it);
        if (entry != entries_.end() && entry->second.ttl > 0.0 &&
            now - entry->second.created > entry->second.ttl) {
            bytes_used_ -= entry->second.bytes.size();
            entries_.erase(entry);
            it = order_.erase(it);
        } else {
            ++it;
        }
    }
}

void ArtifactStore::evict_oldest() {
    while (!order_.empty()) {
        const std::string id = order_.front();
        order_.pop_front();
        const auto entry = entries_.find(id);
        if (entry != entries_.end()) {
            bytes_used_ -= entry->second.bytes.size();
            entries_.erase(entry);
            return;
        }
    }
}

std::string ArtifactStore::store(std::vector<std::uint8_t> bytes, std::string media_type,
                                 JsonValue target, double now) {
    if (bytes.empty()) return {};
    const std::size_t size = bytes.size();
    std::lock_guard<std::mutex> guard(mutex_);
    if (size > max_bytes_) return {};
    expire_locked(now);
    while (bytes_used_ + size > max_bytes_ && !entries_.empty()) evict_oldest();

    Entry entry;
    entry.id = "a" + std::to_string(next_id_++);
    entry.media_type = std::move(media_type);
    entry.sha256 = sha256_hex(bytes.data(), bytes.size());
    entry.created = now;
    entry.ttl = ttl_seconds_;
    entry.target = std::move(target);
    entry.bytes = std::move(bytes);
    const std::string id = entry.id;
    bytes_used_ += size;
    entries_.emplace(id, std::move(entry));
    order_.push_back(id);
    return id;
}

bool ArtifactStore::get(std::string_view id, Entry& out) const {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = entries_.find(std::string(id));
    if (it == entries_.end()) return false;
    out = it->second;
    return true;
}

bool ArtifactStore::metadata(std::string_view id, JsonValue& out) const {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = entries_.find(std::string(id));
    if (it == entries_.end()) return false;
    const Entry& entry = it->second;
    JsonValue value = JsonValue::object();
    value.set("artifactId", entry.id);
    value.set("mediaType", entry.media_type);
    value.set("size", static_cast<std::int64_t>(entry.bytes.size()));
    value.set("sha256", entry.sha256);
    value.set("createdAt", entry.created);
    value.set("ttlS", entry.ttl);
    if (!entry.target.is_null()) value.set("target", entry.target);
    out = std::move(value);
    return true;
}

} // namespace tabengine::automation
