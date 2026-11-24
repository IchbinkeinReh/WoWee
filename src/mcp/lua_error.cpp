#include "mcp/lua_error.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace wowee::mcp {

namespace {

bool isDigit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

bool startsWith(std::string_view text, std::string_view with) {
    return text.substr(0, with.size()) == with;
}

/// Where ":LINE:" begins at or after `from`; npos for nowhere.
size_t lineMark(std::string_view text, size_t from) {
    for (size_t at = text.find(':', from); at != std::string_view::npos; at = text.find(':', at + 1)) {
        size_t end = at + 1;
        while (end < text.size() && isDigit(text[end])) ++end;
        if (end > at + 1 && end < text.size() && text[end] == ':') return at;
    }
    return std::string_view::npos;
}

}  // namespace

LuaError parseLuaError(std::string_view text) {
    LuaError out;
    constexpr std::string_view kAt = "\n      at ";
    if (size_t at = text.find(kAt); at != std::string_view::npos) {
        std::string_view stack = text.substr(at);
        text = text.substr(0, at);
        while (!stack.empty()) {
            stack.remove_prefix(kAt.size());
            at = std::min(stack.find(kAt), stack.size());
            out.stack.emplace_back(stack.substr(0, at));
            stack.remove_prefix(at);
        }
    }
    // A chunk's own text may have any character in it, a ":12:" too; it ends
    // at the quote and bracket it was opened with.
    size_t from = 0;
    if (startsWith(text, "[string \"")) {
        const size_t end = text.find("\"]:");
        if (end != std::string_view::npos) from = end + 2;
    }
    const size_t mark = lineMark(text, from);
    // A path is at the front, before any sentence: one with a space before
    // its first colon is a message with a time of day in it.
    const std::string_view source = mark == std::string_view::npos ? std::string_view() : text.substr(0, mark);
    const bool place = !source.empty() && (from > 0 || source.find(": ") == std::string_view::npos);
    if (!place) {
        out.message = std::string(text);
        return out;
    }
    out.source = std::string(source);
    out.line = std::atoi(std::string(text.substr(mark + 1, 9)).c_str());
    size_t rest = text.find(':', mark + 1) + 1;
    while (rest < text.size() && text[rest] == ' ') ++rest;
    out.message = std::string(text.substr(rest));
    return out;
}

std::optional<LuaError> luaErrorOfLogLine(std::string_view line) {
    if (startsWith(line, "LuaEngine: ")) line.remove_prefix(11);
    else if (startsWith(line, "[Lua] ")) line.remove_prefix(6);
    else return std::nullopt;
    constexpr std::string_view kError = " error: ";
    const size_t at = line.find(kError);
    if (at == std::string_view::npos) return std::nullopt;
    LuaError out = parseLuaError(line.substr(at + kError.size()));
    out.handler = std::string(line.substr(0, at));
    return out;
}

nlohmann::json luaErrorJson(const LuaError& error) {
    nlohmann::json out = {{"message", error.message}};
    if (!error.handler.empty()) out["handler"] = error.handler;
    if (!error.source.empty()) out["source"] = error.source;
    if (error.line > 0) out["line"] = error.line;
    if (!error.stack.empty()) out["stack"] = error.stack;
    return out;
}

}  // namespace wowee::mcp
