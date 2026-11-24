#include "mcp/schema.hpp"

#include <cmath>

namespace wowee::mcp {

namespace schema {

json text(const char* what, int maxLength) {
    return {{"type", "string"}, {"description", what}, {"maxLength", maxLength}};
}

json choice(const char* what, std::initializer_list<const char*> values) {
    json options = json::array();
    for (const char* v : values) options.push_back(v);
    return {{"type", "string"}, {"description", what}, {"enum", std::move(options)}};
}

json number(const char* what, double lo, double hi) {
    return {{"type", "number"}, {"description", what}, {"minimum", lo}, {"maximum", hi}};
}

json integer(const char* what, long long lo, long long hi) {
    return {{"type", "integer"}, {"description", what}, {"minimum", lo}, {"maximum", hi}};
}

json flag(const char* what) {
    return {{"type", "boolean"}, {"description", what}};
}

json list(const char* what, json item, int maxItems) {
    return {{"type", "array"}, {"description", what}, {"items", std::move(item)}, {"maxItems", maxItems}};
}

json guid(const char* what) {
    return {{"type", "string"}, {"description", what}, {"pattern", "^0x[0-9a-fA-F]{1,16}$"}, {"maxLength", 18}};
}

Property required(const char* name, json schema) {
    return {.name = name, .schema = std::move(schema), .required = true};
}

Property optional(const char* name, json schema) {
    return {.name = name, .schema = std::move(schema), .required = false};
}

json object(std::vector<Property> properties) {
    json out = {{"type", "object"}, {"additionalProperties", false}, {"properties", json::object()}};
    json wanted = json::array();
    for (auto& p : properties) {
        if (p.required) wanted.push_back(p.name);
        out["properties"][p.name] = std::move(p.schema);
    }
    if (!wanted.empty()) out["required"] = std::move(wanted);
    return out;
}

}  // namespace schema

namespace {

/// The one pattern these schemas have (schema::guid), without a regex engine
/// to feed untrusted text to.
bool looksLikeGuid(const std::string& s) {
    if (s.size() < 3 || s.size() > 18 || s[0] != '0' || s[1] != 'x') return false;
    for (size_t i = 2; i < s.size(); ++i) {
        const char c = s[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

std::string check(const json& schema, const json& value, const std::string& where) {
    const std::string type = schema.value("type", "object");
    if (type == "object") {
        if (!value.is_object()) return where + ": an object is wanted";
        const json& properties = schema.contains("properties") ? schema["properties"] : json::object();
        if (schema.contains("required")) {
            for (const auto& name : schema["required"]) {
                if (!value.contains(name.get<std::string>())) return "missing: " + name.get<std::string>();
            }
        }
        for (const auto& [name, item] : value.items()) {
            if (!properties.contains(name)) return "unknown argument: " + name.substr(0, 64);
            if (std::string bad = check(properties[name], item, name); !bad.empty()) return bad;
        }
        return {};
    }
    if (type == "string") {
        if (!value.is_string()) return where + ": a string is wanted";
        const auto& s = value.get_ref<const std::string&>();
        if (schema.contains("maxLength") && s.size() > schema["maxLength"].get<size_t>()) {
            return where + ": longer than " + std::to_string(schema["maxLength"].get<size_t>());
        }
        if (s.find('\0') != std::string::npos) return where + ": a NUL in it";
        if (schema.contains("pattern") && !looksLikeGuid(s)) return where + ": not a guid (0x...)";
        if (schema.contains("enum")) {
            std::string options;
            for (const auto& v : schema["enum"]) {
                if (v == value) return {};
                options += (options.empty() ? "" : ", ") + v.get<std::string>();
            }
            return where + ": one of " + options;
        }
        return {};
    }
    if (type == "boolean") return value.is_boolean() ? std::string() : where + ": true or false is wanted";
    if (type == "number" || type == "integer") {
        if (!value.is_number()) return where + ": a number is wanted";
        const double v = value.get<double>();
        if (!std::isfinite(v)) return where + ": a finite number is wanted";
        if (type == "integer" && std::floor(v) != v) return where + ": a whole number is wanted";
        if (schema.contains("minimum") && v < schema["minimum"].get<double>()) {
            return where + ": below " + schema["minimum"].dump();
        }
        if (schema.contains("maximum") && v > schema["maximum"].get<double>()) {
            return where + ": above " + schema["maximum"].dump();
        }
        return {};
    }
    if (type == "array") {
        if (!value.is_array()) return where + ": an array is wanted";
        if (schema.contains("maxItems") && value.size() > schema["maxItems"].get<size_t>()) {
            return where + ": more than " + std::to_string(schema["maxItems"].get<size_t>()) + " items";
        }
        if (!schema.contains("items")) return {};
        for (const auto& item : value) {
            if (std::string bad = check(schema["items"], item, where); !bad.empty()) return bad;
        }
        return {};
    }
    return where + ": no such type";
}

}  // namespace

std::string validate(const json& schema, const json& args) {
    return check(schema, args, "arguments");
}

}  // namespace wowee::mcp
