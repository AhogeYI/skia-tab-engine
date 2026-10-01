// Protocol value model: strict parsing, serialization, wire ids, schema
// validation. Platform-neutral; no Skia.

#include "tabengine/automation/schema.h"
#include "tabengine/automation/values.h"

#include "check.h"

#include <cstdint>
#include <string>

using tabengine::automation::id_from_wire;
using tabengine::automation::id_to_wire;
using tabengine::automation::JsonParseResult;
using tabengine::automation::JsonParseStatus;
using tabengine::automation::JsonValue;
using tabengine::automation::parse_json_strict;
using tabengine::automation::rect_from_wire;
using tabengine::automation::rect_to_wire;
using tabengine::automation::validate_against_schema;

static void test_parse_ok() {
    JsonValue value;
    const JsonParseResult result =
        parse_json_strict(R"({"a": 1, "b": [true, null, "x"], "c": 1.5})", 4096, value);
    CHECK(result.status == JsonParseStatus::Ok);
    CHECK(value.is_object());
    CHECK(value.find("a")->is_integer() && value.find("a")->as_integer() == 1);
    CHECK(value.find("b")->is_array() && value.find("b")->size() == 3);
    CHECK(value.find("b")->at(0).is_boolean() && value.find("b")->at(0).as_boolean());
    CHECK(value.find("b")->at(1).is_null());
    CHECK(value.find("b")->at(2).as_string() == "x");
    CHECK(value.find("c")->is_number() && !value.find("c")->is_integer());
}

static void test_parse_errors() {
    JsonValue value;
    struct Case {
        const char* text;
        JsonParseStatus expected;
    };
    const Case cases[] = {
        {"{", JsonParseStatus::SyntaxError},
        {"{} trailing", JsonParseStatus::SyntaxError},
        {R"({"a":1,"a":2})", JsonParseStatus::DuplicateKey},
        {R"([1, {"b":2, "b":3}])", JsonParseStatus::DuplicateKey},
        {"99999999999999999999", JsonParseStatus::NumberOutOfRange},
        {"-9223372036854775809", JsonParseStatus::NumberOutOfRange},
        {"1e999", JsonParseStatus::NumberOutOfRange},
        {"\"\xff\xfe\"", JsonParseStatus::InvalidUtf8},
        {R"("\ud800")", JsonParseStatus::InvalidUtf8},   // lone surrogate escape
        {R"("\ud800\ud800")", JsonParseStatus::InvalidUtf8},
    };
    for (const Case& probe : cases) {
        const JsonParseResult result = parse_json_strict(probe.text, 4096, value);
        if (result.status != probe.expected) {
            std::fprintf(stderr, "probe got %d want %d detail: %s\n",
                         static_cast<int>(result.status), static_cast<int>(probe.expected),
                         result.detail.c_str());
        }
        CHECK(result.status == probe.expected);
    }
    // Nesting beyond the limit.
    std::string deep;
    for (int i = 0; i < 200; ++i) deep += "[";
    const JsonParseResult result = parse_json_strict(deep, 4096, value);
    CHECK(result.status == JsonParseStatus::DepthExceeded);
    // Document cap.
    const JsonParseResult capped = parse_json_strict(std::string(100, ' '), 16, value);
    CHECK(capped.status == JsonParseStatus::DocumentTooLarge);
}

static void test_parse_valid_surrogate_pair() {
    JsonValue value;
    const JsonParseResult result = parse_json_strict(R"("\ud83d\ude00")", 64, value);
    CHECK(result.status == JsonParseStatus::Ok);
    CHECK(value.as_string() == "\xF0\x9F\x98\x80");   // U+1F600
}

static void test_serialize_round_trip() {
    JsonValue value = JsonValue::object();
    value.set("s", std::string("quote\" backslash\\ newline\n tab\t ctrl\x01"));
    value.set("int", static_cast<std::int64_t>(-42));
    value.set("num", 0.5);
    JsonValue inner = JsonValue::array();
    inner.push_back(JsonValue("ok"));
    value.set("arr", std::move(inner));
    value.set("empty", JsonValue{});
    const std::string text = value.serialize();
    JsonValue parsed;
    const JsonParseResult result = parse_json_strict(text, 4096, parsed);
    CHECK(result.status == JsonParseStatus::Ok);
    CHECK(parsed.find("s")->as_string() == value.find("s")->as_string());
    CHECK(parsed.find("int")->as_integer() == -42);
    CHECK(parsed.find("num")->as_number() == 0.5);
    CHECK(parsed.find("arr")->at(0).as_string() == "ok");
    CHECK(parsed.find("empty")->is_null());
    // Member order is preserved.
    CHECK(text.find("\"s\"") < text.find("\"int\""));
    CHECK(text.find("\"int\"") < text.find("\"num\""));
}

static void test_number_precision() {
    // 0.1 must round-trip instead of printing a truncated value.
    JsonValue value;
    CHECK(parse_json_strict("0.1", 16, value).status == JsonParseStatus::Ok);
    const std::string text = value.serialize();
    JsonValue parsed;
    CHECK(parse_json_strict(text, 64, parsed).status == JsonParseStatus::Ok);
    CHECK(parsed.as_number() == 0.1);
    // Integers stay integers on the wire.
    CHECK(parse_json_strict("2", 4, value).status == JsonParseStatus::Ok);
    CHECK(value.serialize() == "2");
}

static void test_wire_ids() {
    CHECK(id_to_wire(0) == "0");
    CHECK(id_to_wire(18446744073709551615ULL) == "18446744073709551615");
    std::uint64_t id = 0;
    CHECK(id_from_wire("17", id) && id == 17);
    CHECK(id_from_wire("18446744073709551615", id) && id == 18446744073709551615ULL);
    CHECK(!id_from_wire("-1", id));
    CHECK(!id_from_wire("+1", id));
    CHECK(!id_from_wire("0x10", id));
    CHECK(!id_from_wire("18446744073709551616", id));
    CHECK(!id_from_wire("1 ", id));
    CHECK(!id_from_wire("", id));
}

static void test_rect_wire() {
    const JsonValue wire = rect_to_wire({1, 2, 30, 40});
    tabengine::Rect rect;
    CHECK(rect_from_wire(wire, rect) && rect.x == 1 && rect.height == 40);
    JsonValue broken = JsonValue::object();
    broken.set("x", JsonValue("nope"));
    CHECK(!rect_from_wire(broken, rect));
    JsonValue partial = JsonValue::object();
    partial.set("x", JsonValue(static_cast<std::int64_t>(1)));
    CHECK(!rect_from_wire(partial, rect));
}

static void test_schema_validation() {
    JsonValue schema = JsonValue::object();
    schema.set("type", "object");
    JsonValue properties = JsonValue::object();
    JsonValue window_prop = JsonValue::object();
    window_prop.set("type", "string");
    window_prop.set("description", "window id");
    properties.set("windowId", std::move(window_prop));
    JsonValue count_prop = JsonValue::object();
    count_prop.set("type", "integer");
    count_prop.set("minimum", static_cast<std::int64_t>(1));
    properties.set("count", std::move(count_prop));
    JsonValue mode_prop = JsonValue::object();
    mode_prop.set("type", "string");
    JsonValue modes = JsonValue::array();
    modes.push_back(JsonValue("left"));
    modes.push_back(JsonValue("right"));
    mode_prop.set("enum", std::move(modes));
    properties.set("mode", std::move(mode_prop));
    schema.set("properties", std::move(properties));
    JsonValue required = JsonValue::array();
    required.push_back(JsonValue("windowId"));
    schema.set("required", std::move(required));

    JsonValue params = JsonValue::object();
    params.set("windowId", JsonValue("1"));
    CHECK(validate_against_schema(params, schema).ok);

    JsonValue no_id = JsonValue::object();
    const auto missing = validate_against_schema(no_id, schema);
    CHECK(!missing.ok && missing.error.find("windowId") != std::string::npos);

    JsonValue extra = JsonValue::object();
    extra.set("windowId", JsonValue("1"));
    extra.set("bogus", JsonValue(static_cast<std::int64_t>(1)));
    const auto unknown = validate_against_schema(extra, schema);
    CHECK(!unknown.ok && unknown.error.find("bogus") != std::string::npos);

    JsonValue bad_type = JsonValue::object();
    bad_type.set("windowId", JsonValue(static_cast<std::int64_t>(1)));
    CHECK(!validate_against_schema(bad_type, schema).ok);

    JsonValue bad_enum = JsonValue::object();
    bad_enum.set("windowId", JsonValue("1"));
    bad_enum.set("mode", JsonValue("middle"));
    const auto enum_error = validate_against_schema(bad_enum, schema);
    CHECK(!enum_error.ok && enum_error.error.find("enum") != std::string::npos);

    JsonValue bad_min = JsonValue::object();
    bad_min.set("windowId", JsonValue("1"));
    bad_min.set("count", JsonValue(static_cast<std::int64_t>(0)));
    const auto min_error = validate_against_schema(bad_min, schema);
    CHECK(!min_error.ok && min_error.error.find("minimum") != std::string::npos);

    // Array validation with per-item schemas and bounds.
    JsonValue list_schema = JsonValue::object();
    list_schema.set("type", "array");
    JsonValue items = JsonValue::object();
    items.set("type", "string");
    list_schema.set("items", std::move(items));
    list_schema.set("minItems", static_cast<std::int64_t>(1));
    list_schema.set("maxItems", static_cast<std::int64_t>(2));
    JsonValue empty_list = JsonValue::array();
    CHECK(!validate_against_schema(empty_list, list_schema).ok);
    JsonValue one = JsonValue::array();
    one.push_back(JsonValue("a"));
    CHECK(validate_against_schema(one, list_schema).ok);
    JsonValue three = JsonValue::array();
    three.push_back(JsonValue("a"));
    three.push_back(JsonValue("b"));
    three.push_back(JsonValue("c"));
    CHECK(!validate_against_schema(three, list_schema).ok);
    JsonValue wrong_item = JsonValue::array();
    wrong_item.push_back(JsonValue(static_cast<std::int64_t>(7)));
    const auto item_error = validate_against_schema(wrong_item, list_schema, "params");
    CHECK(!item_error.ok && item_error.error.find("params[0]") != std::string::npos);
}

static void test_utf8_validation() {
    JsonValue value = JsonValue::object();
    value.set("ok", std::string("plain ascii"));
    CHECK(value.is_valid_utf8());
    JsonValue bad = JsonValue::object();
    bad.set("x", std::string("bad \xC3 sequence"));
    CHECK(!bad.is_valid_utf8());
}

void run_automation_values_tests() {
    test_parse_ok();
    test_parse_errors();
    test_parse_valid_surrogate_pair();
    test_serialize_round_trip();
    test_number_precision();
    test_wire_ids();
    test_rect_wire();
    test_schema_validation();
    test_utf8_validation();
}
