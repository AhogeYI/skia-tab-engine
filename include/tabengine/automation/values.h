#pragma once

// Protocol value model for TabEngine Automation. This is a self-contained
// JSON value type: the wire parser lives in the implementation, and no
// third-party JSON type ever appears in a public header. Wire identifiers
// (WindowId, TabId, ContentId, node refs, sequence numbers) travel as
// decimal strings so JavaScript consumers never lose 64-bit precision.

#include "tabengine/types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tabengine::automation {

class JsonValue {
public:
    enum class Type { Null, Boolean, Integer, Number, String, Array, Object };

    JsonValue() = default;
    JsonValue(bool value) : type_(Type::Boolean), boolean_(value) {}
    JsonValue(std::int64_t value) : type_(Type::Integer), integer_(value) {}
    JsonValue(int value) : type_(Type::Integer), integer_(value) {}
    JsonValue(double value) : type_(Type::Number), number_(value) {}
    JsonValue(std::string value) : type_(Type::String), string_(std::move(value)) {}
    JsonValue(const char* value) : type_(Type::String), string_(value) {}

    [[nodiscard]] static JsonValue array() { return JsonValue(Type::Array); }
    [[nodiscard]] static JsonValue object() { return JsonValue(Type::Object); }

    [[nodiscard]] Type type() const { return type_; }
    [[nodiscard]] bool is_null() const { return type_ == Type::Null; }
    [[nodiscard]] bool is_boolean() const { return type_ == Type::Boolean; }
    [[nodiscard]] bool is_integer() const { return type_ == Type::Integer; }
    [[nodiscard]] bool is_number() const { return type_ == Type::Integer || type_ == Type::Number; }
    [[nodiscard]] bool is_string() const { return type_ == Type::String; }
    [[nodiscard]] bool is_array() const { return type_ == Type::Array; }
    [[nodiscard]] bool is_object() const { return type_ == Type::Object; }

    // Typed accessors return the stored value; call sites must check the
    // type first. Wrong-type reads return zero values rather than throwing,
    // so protocol code stays exception-free.
    [[nodiscard]] bool as_boolean() const { return boolean_; }
    [[nodiscard]] std::int64_t as_integer() const { return integer_; }
    [[nodiscard]] double as_number() const {
        return type_ == Type::Integer ? static_cast<double>(integer_) : number_;
    }
    [[nodiscard]] const std::string& as_string() const { return string_; }

    // Array interface; ignored on non-arrays.
    void push_back(JsonValue value);
    [[nodiscard]] std::size_t size() const {
        return type_ == Type::Array ? array_.size() :
               type_ == Type::Object ? object_.size() : 0;
    }
    [[nodiscard]] const JsonValue& at(std::size_t index) const;
    [[nodiscard]] const std::vector<JsonValue>& items() const { return array_; }
    [[nodiscard]] std::vector<JsonValue>& items() { return array_; }

    // Object interface. Member order is preserved for deterministic output.
    // set() replaces an existing member of the same name in place.
    void set(std::string key, JsonValue value);
    [[nodiscard]] const JsonValue* find(std::string_view key) const;
    [[nodiscard]] const std::vector<std::pair<std::string, JsonValue>>& members() const {
        return object_;
    }
    [[nodiscard]] std::vector<std::pair<std::string, JsonValue>>& members() { return object_; }

    // Compact serialization: UTF-8 output, control characters escaped,
    // object member order preserved. Number formatting is round-trip safe.
    [[nodiscard]] std::string serialize() const;

    // Recursively validate that every string in the tree is well-formed
    // UTF-8 (no lone surrogates, no overlong encodings, no out-of-range
    // code points). The parser guarantees this on the way in; providers
    // that build values from external text validate before exposing it.
    [[nodiscard]] bool is_valid_utf8() const;

private:
    explicit JsonValue(Type type) : type_(type) {}

    Type type_ = Type::Null;
    bool boolean_ = false;
    std::int64_t integer_ = 0;
    double number_ = 0.0;
    std::string string_;
    std::vector<JsonValue> array_;
    std::vector<std::pair<std::string, JsonValue>> object_;
};

// Strict parse outcomes. Everything the wire contract calls an error is
// reported here: malformed grammar, duplicate object keys, nesting beyond
// the limit, integers outside int64, non-finite or out-of-range floats,
// invalid UTF-8, and documents above the byte cap.
enum class JsonParseStatus {
    Ok,
    SyntaxError,
    DuplicateKey,
    DepthExceeded,
    NumberOutOfRange,
    DocumentTooLarge,
    InvalidUtf8,
};

struct JsonParseResult {
    JsonParseStatus status = JsonParseStatus::Ok;
    std::size_t offset = 0;      // byte offset of the failure when status != Ok
    std::string detail;          // English diagnostic, safe to log
};

// Parse one complete JSON document. Trailing non-whitespace bytes after the
// document are a syntax error. `max_bytes` bounds the input length.
JsonParseResult parse_json_strict(std::string_view text, std::size_t max_bytes, JsonValue& out);

// Decimal wire-id helpers. Strict: digits only, no sign, no leading '+',
// value must fit uint64. Used for every id field on the wire.
[[nodiscard]] std::string id_to_wire(std::uint64_t id);
[[nodiscard]] bool id_from_wire(std::string_view text, std::uint64_t& id);

// Client-space rectangle on the wire: {"x","y","width","height"} integers.
[[nodiscard]] JsonValue rect_to_wire(Rect rect);
[[nodiscard]] bool rect_from_wire(const JsonValue& value, Rect& rect);

} // namespace tabengine::automation
