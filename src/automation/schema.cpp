#include "tabengine/automation/schema.h"

#include <cmath>
#include <string>

namespace tabengine::automation {
namespace {

bool type_matches(std::string_view declared, const JsonValue& value) {
    if (declared == "any") return true;
    if (declared == "object") return value.is_object();
    if (declared == "array") return value.is_array();
    if (declared == "string") return value.is_string();
    if (declared == "boolean") return value.is_boolean();
    if (declared == "null") return value.is_null();
    if (declared == "integer") return value.is_integer();
    if (declared == "number") return value.is_number();
    return false;
}

SchemaValidationResult fail(std::string_view path, std::string message) {
    SchemaValidationResult result;
    result.ok = false;
    result.error = std::string(path) + ": " + std::move(message);
    return result;
}

SchemaValidationResult validate(const JsonValue& instance, const JsonValue& schema,
                                std::string_view path);

SchemaValidationResult validate_object(const JsonValue& instance, const JsonValue& schema,
                                       std::string_view path) {
    const JsonValue* properties = schema.find("properties");
    for (const auto& [key, member] : instance.members()) {
        const JsonValue* property_schema = properties ? properties->find(key) : nullptr;
        if (!property_schema) {
            return fail(path, "unknown property '" + key + "'");
        }
        const std::string child_path = std::string(path) + "." + key;
        if (const SchemaValidationResult result = validate(member, *property_schema, child_path);
            !result.ok)
            return result;
    }
    const JsonValue* required = schema.find("required");
    if (required && required->is_array()) {
        for (const JsonValue& name : required->items()) {
            if (!name.is_string()) continue;
            if (!instance.find(name.as_string())) {
                return fail(path, "missing required property '" + name.as_string() + "'");
            }
        }
    }
    SchemaValidationResult ok;
    ok.ok = true;
    return ok;
}

SchemaValidationResult validate_array(const JsonValue& instance, const JsonValue& schema,
                                      std::string_view path) {
    const JsonValue* items = schema.find("items");
    const auto read_size = [&schema](std::string_view key, std::size_t& target) {
        const JsonValue* member = schema.find(key);
        if (!member || !member->is_integer() || member->as_integer() < 0) return false;
        target = static_cast<std::size_t>(member->as_integer());
        return true;
    };
    std::size_t min_items = 0;
    std::size_t max_items = 0;
    const bool has_min = read_size("minItems", min_items);
    const bool has_max = read_size("maxItems", max_items);
    if (has_min && instance.size() < min_items)
        return fail(path, "expected at least " + std::to_string(min_items) + " items");
    if (has_max && instance.size() > max_items)
        return fail(path, "expected at most " + std::to_string(max_items) + " items");
    if (items) {
        for (std::size_t i = 0; i < instance.size(); ++i) {
            const std::string child_path =
                std::string(path) + "[" + std::to_string(i) + "]";
            if (const SchemaValidationResult result =
                    validate(instance.at(i), *items, child_path);
                !result.ok)
                return result;
        }
    }
    SchemaValidationResult ok;
    ok.ok = true;
    return ok;
}

SchemaValidationResult validate(const JsonValue& instance, const JsonValue& schema,
                                std::string_view path) {
    if (!schema.is_object()) {
        SchemaValidationResult ok;
        ok.ok = true;
        return ok;
    }
    const JsonValue* type = schema.find("type");
    if (type && type->is_string() && !type_matches(type->as_string(), instance)) {
        return fail(path, "expected " + type->as_string() + ", got " +
                              (instance.is_null() ? "null" : "value of another type"));
    }
    const JsonValue* enum_values = schema.find("enum");
    if (enum_values && enum_values->is_array()) {
        bool matched = false;
        for (const JsonValue& candidate : enum_values->items()) {
            if (candidate.serialize() == instance.serialize()) {
                matched = true;
                break;
            }
        }
        if (!matched) return fail(path, "value is not one of the allowed enum entries");
    }
    const JsonValue* minimum = schema.find("minimum");
    if (minimum && minimum->is_number() && instance.is_number() &&
        instance.as_number() < minimum->as_number())
        return fail(path, "value below the allowed minimum");
    const JsonValue* maximum = schema.find("maximum");
    if (maximum && maximum->is_number() && instance.is_number() &&
        instance.as_number() > maximum->as_number())
        return fail(path, "value above the allowed maximum");
    const JsonValue* min_length = schema.find("minLength");
    if (min_length && min_length->is_integer() && instance.is_string() &&
        instance.as_string().size() < static_cast<std::size_t>(min_length->as_integer()))
        return fail(path, "string shorter than the allowed minimum");
    const JsonValue* max_length = schema.find("maxLength");
    if (max_length && max_length->is_integer() && instance.is_string() &&
        instance.as_string().size() > static_cast<std::size_t>(max_length->as_integer()))
        return fail(path, "string longer than the allowed maximum");
    if (instance.is_object()) return validate_object(instance, schema, path);
    if (instance.is_array()) return validate_array(instance, schema, path);
    SchemaValidationResult ok;
    ok.ok = true;
    return ok;
}

} // namespace

SchemaValidationResult validate_against_schema(const JsonValue& instance, const JsonValue& schema,
                                               std::string_view path) {
    return validate(instance, schema, path);
}

} // namespace tabengine::automation
