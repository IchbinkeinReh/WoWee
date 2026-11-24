#pragma once

// What a tool takes, said once: the JSON schema tools/list hands out is the
// one the arguments of a call are held to.
//
// Of JSON Schema only what these tools use - objects with typed properties,
// required ones, ranges, lengths, choices and arrays of one type - and nothing
// beside: a property a schema does not name is refused, since an argument that
// is quietly not read is a call that does something else than was asked.

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace wowee::mcp {

using json = nlohmann::json;

namespace schema {

/// One property: its name, its schema, and whether it must be given.
struct Property {
    std::string name;
    json schema;
    bool required = false;
};

json text(const char* what, int maxLength = 256);
json choice(const char* what, std::initializer_list<const char*> values);
json number(const char* what, double lo, double hi);
json integer(const char* what, long long lo, long long hi);
json flag(const char* what);
/// An array of up to maxItems of `item`.
json list(const char* what, json item, int maxItems);
/// A guid as this server writes them: 0x and up to 16 hex digits.
json guid(const char* what);
Property required(const char* name, json schema);
Property optional(const char* name, json schema);
json object(std::vector<Property> properties);

}  // namespace schema

/// The first thing wrong with `args`, or an empty string. Never throws.
std::string validate(const json& schema, const json& args);

}  // namespace wowee::mcp
