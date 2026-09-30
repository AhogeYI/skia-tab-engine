// Protocol JSON value model: strict parsing over the pinned nlohmann/json
// release (vendored under third_party/json) and compact serialization. The
// third-party header stays in this translation unit; public headers never
// expose it.

#include "tabengine/automation/values.h"

#include <json.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <unordered_set>

namespace tabengine::automation {
namespace {

constexpr std::size_t kMaxDepth = 64;

// ---- strict UTF-8 validation (shared by parser and serializer checks) ----

bool valid_utf8_code_point(std::string_view text, std::size_t& i) {
    const auto byte_at = [&text](std::size_t index) -> unsigned char {
        return static_cast<unsigned char>(text[index]);
    };
    const unsigned char lead = byte_at(i);
    std::size_t length = 0;
    std::uint32_t code_point = 0;
    if (lead < 0x80) {
        ++i;
        return true;
    }
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        code_point = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        code_point = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        code_point = lead & 0x07u;
    } else {
        return false;
    }
    if (i + length > text.size()) return false;
    for (std::size_t k = 1; k < length; ++k) {
        const unsigned char continuation = byte_at(i + k);
        if ((continuation & 0xC0) != 0x80) return false;
        code_point = (code_point << 6) | (continuation & 0x3Fu);
    }
    // Reject overlong encodings, surrogates, and out-of-range code points.
    if (length == 2 && code_point < 0x80) return false;
    if (length == 3 && code_point < 0x800) return false;
    if (length == 4 && code_point < 0x10000) return false;
    if (code_point >= 0xD800 && code_point <= 0xDFFF) return false;
    if (code_point > 0x10FFFF) return false;
    i += length;
    return true;
}

bool is_strict_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        if (!valid_utf8_code_point(text, i)) return false;
    }
    return true;
}

// ---- number pre-scan: reject integer overflow and non-finite floats ----
// nlohmann silently degrades out-of-range integers to double; the protocol
// forbids silent precision loss, so every number token is checked against
// its declared shape before SAX parsing.

bool scan_number_token(std::string_view text, std::size_t& i, std::string& error) {
    const std::size_t start = i;
    if (text[i] == '-') ++i;
    bool digits = false;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        ++i;
        digits = true;
    }
    bool is_float = false;
    if (i < text.size() && text[i] == '.') {
        is_float = true;
        ++i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        is_float = true;
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
    }
    const std::string token(text.substr(start, i - start));
    if (!digits) {
        error = "malformed number token '" + token + "'";
        return false;
    }
    if (!is_float) {
        // Integer: must fit int64 exactly (leading '-' allowed for the
        // int64 minimum).
        errno = 0;
        char* end = nullptr;
        const long long value = std::strtoll(token.c_str(), &end, 10);
        if (errno == ERANGE || end != token.c_str() + token.size()) {
            error = "integer out of int64 range: '" + token + "'";
            return false;
        }
        (void)value;
        return true;
    }
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (errno == ERANGE || end != token.c_str() + token.size() || !std::isfinite(value)) {
        error = "float out of range: '" + token + "'";
        return false;
    }
    return true;
}

bool precheck_numbers(std::string_view text, std::string& error) {
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            if (!scan_number_token(text, i, error)) return false;
        }
    }
    return true;
}

// ---- SAX handler building the value tree ----

struct Frame {
    JsonValue* value = nullptr;
    bool is_object = false;
    std::string pending_key;
    std::unordered_set<std::string> keys;
};

class BuilderSax {
public:
    using number_integer_t = std::int64_t;
    using number_unsigned_t = std::uint64_t;
    using number_float_t = double;
    using string_t = std::string;
    using binary_t = nlohmann::json::binary_t;

    explicit BuilderSax(std::string& error) : error_(error) {}

    bool null() { return attach(JsonValue{}); }
    bool boolean(bool val) { return attach(JsonValue(val)); }
    bool number_integer(number_integer_t val) { return attach(JsonValue(static_cast<std::int64_t>(val))); }
    bool number_unsigned(number_unsigned_t val) {
        // Pre-scan already confined integers to int64; unsigned values can
        // only appear within that range.
        if (val > static_cast<number_unsigned_t>(std::numeric_limits<std::int64_t>::max())) {
            error_ = "integer out of int64 range";
            return false;
        }
        return attach(JsonValue(static_cast<std::int64_t>(val)));
    }
    bool number_float(number_float_t val, const string_t&) {
        if (!std::isfinite(val)) {
            error_ = "float out of range";
            return false;
        }
        return attach(JsonValue(static_cast<double>(val)));
    }
    bool string(string_t& val) {
        if (!is_strict_utf8(val)) {
            error_ = "invalid UTF-8 in string";
            return false;
        }
        return attach(JsonValue(std::move(val)));
    }
    bool binary(binary_t&) {
        error_ = "binary values are not part of the protocol";
        return false;
    }
    bool start_object(std::size_t) {
        if (frames_.size() >= kMaxDepth) {
            error_ = "nesting depth limit exceeded";
            return false;
        }
        JsonValue value = JsonValue::object();
        if (!attach(value)) return false;
        Frame frame;
        frame.value = current_;
        frame.is_object = true;
        frames_.push_back(std::move(frame));
        return true;
    }
    bool key(string_t& val) {
        if (frames_.empty() || !frames_.back().is_object) return false;
        auto& frame = frames_.back();
        if (!frame.keys.insert(val).second) {
            error_ = "duplicate object key '" + val + "'";
            return false;
        }
        frame.pending_key = std::move(val);
        return true;
    }
    bool end_object() {
        if (frames_.empty() || !frames_.back().is_object) return false;
        frames_.pop_back();
        return true;
    }
    bool start_array(std::size_t) {
        if (frames_.size() >= kMaxDepth) {
            error_ = "nesting depth limit exceeded";
            return false;
        }
        JsonValue value = JsonValue::array();
        if (!attach(value)) return false;
        Frame frame;
        frame.value = current_;
        frame.is_object = false;
        frames_.push_back(std::move(frame));
        return true;
    }
    bool end_array() {
        if (frames_.empty() || frames_.back().is_object) return false;
        frames_.pop_back();
        return true;
    }
    bool parse_error(std::size_t position, const std::string& token,
                     const nlohmann::detail::exception& ex) {
        error_ = std::string(ex.what());
        (void)token;
        failure_offset_ = position;
        return false;
    }

    [[nodiscard]] JsonValue take_root() { return std::move(root_); }
    [[nodiscard]] const std::string& error() const { return error_; }
    [[nodiscard]] std::size_t failure_offset() const { return failure_offset_; }

private:
    bool attach(JsonValue value) {
        if (frames_.empty()) {
            if (has_root_) {
                error_ = "multiple root values";
                return false;
            }
            root_ = std::move(value);
            has_root_ = true;
            current_ = &root_;
            return true;
        }
        Frame& frame = frames_.back();
        if (frame.is_object) {
            auto& members = frame.value->members();
            members.emplace_back(std::move(frame.pending_key), std::move(value));
            current_ = &members.back().second;
            frame.pending_key.clear();
        } else {
            auto& items = frame.value->items();
            items.push_back(std::move(value));
            current_ = &items.back();
        }
        return true;
    }

    std::string& error_;
    JsonValue root_;
    JsonValue* current_ = nullptr;
    std::vector<Frame> frames_;
    bool has_root_ = false;
    std::size_t failure_offset_ = 0;
};

// ---- serialization ----

void write_escaped(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

void write_number(std::string& out, double value) {
    char buffer[64];
    // Shortest representation that round-trips: 15, then 16, then 17 digits.
    for (int precision : {15, 16, 17}) {
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
        if (std::strtod(buffer, nullptr) == value) break;
    }
    out += buffer;
    if (!std::strchr(buffer, '.') && !std::strchr(buffer, 'e') && !std::strchr(buffer, 'E'))
        out += ".0";   // keep the value recognizable as a float
}

void serialize_value(std::string& out, const JsonValue& value) {
    switch (value.type()) {
    case JsonValue::Type::Null: out += "null"; break;
    case JsonValue::Type::Boolean: out += value.as_boolean() ? "true" : "false"; break;
    case JsonValue::Type::Integer: out += std::to_string(value.as_integer()); break;
    case JsonValue::Type::Number: write_number(out, value.as_number()); break;
    case JsonValue::Type::String: write_escaped(out, value.as_string()); break;
    case JsonValue::Type::Array: {
        out.push_back('[');
        bool first = true;
        for (const JsonValue& item : value.items()) {
            if (!first) out.push_back(',');
            first = false;
            serialize_value(out, item);
        }
        out.push_back(']');
        break;
    }
    case JsonValue::Type::Object: {
        out.push_back('{');
        bool first = true;
        for (const auto& [key, member] : value.members()) {
            if (!first) out.push_back(',');
            first = false;
            write_escaped(out, key);
            out.push_back(':');
            serialize_value(out, member);
        }
        out.push_back('}');
        break;
    }
    }
}

} // namespace

void JsonValue::push_back(JsonValue value) {
    if (type_ == Type::Array) array_.push_back(std::move(value));
}

const JsonValue& JsonValue::at(std::size_t index) const {
    static const JsonValue empty;
    return type_ == Type::Array && index < array_.size() ? array_[index] : empty;
}

void JsonValue::set(std::string key, JsonValue value) {
    if (type_ != Type::Object) return;
    for (auto& [existing_key, member] : object_) {
        if (existing_key == key) {
            member = std::move(value);
            return;
        }
    }
    object_.emplace_back(std::move(key), std::move(value));
}

const JsonValue* JsonValue::find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& [existing_key, member] : object_) {
        if (existing_key == key) return &member;
    }
    return nullptr;
}

std::string JsonValue::serialize() const {
    std::string out;
    serialize_value(out, *this);
    return out;
}

bool JsonValue::is_valid_utf8() const {
    switch (type_) {
    case Type::String: return is_strict_utf8(string_);
    case Type::Array:
        for (const JsonValue& item : array_) {
            if (!item.is_valid_utf8()) return false;
        }
        return true;
    case Type::Object:
        for (const auto& [key, member] : object_) {
            if (!is_strict_utf8(key) || !member.is_valid_utf8()) return false;
        }
        return true;
    default: return true;
    }
}

JsonParseResult parse_json_strict(std::string_view text, std::size_t max_bytes, JsonValue& out) {
    JsonParseResult result;
    if (text.size() > max_bytes) {
        result.status = JsonParseStatus::DocumentTooLarge;
        result.detail = "document exceeds the byte limit";
        return result;
    }
    std::string error;
    if (!precheck_numbers(text, error)) {
        result.status = JsonParseStatus::NumberOutOfRange;
        result.detail = std::move(error);
        return result;
    }
    BuilderSax sax(error);
    const bool ok = nlohmann::json::sax_parse(text.begin(), text.end(), &sax,
                                              nlohmann::json::input_format_t::json,
                                              /*strict=*/true);
    if (!ok || !error.empty()) {
        if (error.find("duplicate object key") != std::string::npos) {
            result.status = JsonParseStatus::DuplicateKey;
        } else if (error.find("depth limit") != std::string::npos ||
                   error.find("nesting depth") != std::string::npos) {
            result.status = JsonParseStatus::DepthExceeded;
        } else if (error.find("UTF-8") != std::string::npos ||
                   error.find("surrogate") != std::string::npos) {
            result.status = JsonParseStatus::InvalidUtf8;
        } else {
            result.status = JsonParseStatus::SyntaxError;
        }
        result.offset = sax.failure_offset();
        result.detail = error.empty() ? "parse failed" : error;
        return result;
    }
    out = sax.take_root();
    result.status = JsonParseStatus::Ok;
    return result;
}

std::string id_to_wire(std::uint64_t id) { return std::to_string(id); }

bool id_from_wire(std::string_view text, std::uint64_t& id) {
    if (text.empty() || text.size() > 20) return false;
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    id = value;
    return true;
}

JsonValue rect_to_wire(Rect rect) {
    JsonValue value = JsonValue::object();
    value.set("x", static_cast<std::int64_t>(rect.x));
    value.set("y", static_cast<std::int64_t>(rect.y));
    value.set("width", static_cast<std::int64_t>(rect.width));
    value.set("height", static_cast<std::int64_t>(rect.height));
    return value;
}

bool rect_from_wire(const JsonValue& value, Rect& rect) {
    const auto read_int = [&value](std::string_view key, int& target) {
        const JsonValue* member = value.find(key);
        if (!member || !member->is_integer()) return false;
        const std::int64_t number = member->as_integer();
        if (number < std::numeric_limits<int>::min() ||
            number > std::numeric_limits<int>::max())
            return false;
        target = static_cast<int>(number);
        return true;
    };
    Rect parsed;
    if (!read_int("x", parsed.x) || !read_int("y", parsed.y) ||
        !read_int("width", parsed.width) || !read_int("height", parsed.height))
        return false;
    rect = parsed;
    return true;
}

} // namespace tabengine::automation
