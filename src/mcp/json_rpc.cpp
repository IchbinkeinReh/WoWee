#include "mcp/json_rpc.hpp"

#include <algorithm>
#include <exception>

#include "mcp/schema.hpp"

namespace wowee::mcp {

namespace {

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;

/// Deeper than any call here, and shallow enough to write out again.
constexpr size_t kMostNesting = 32;
constexpr size_t kMostBatch = 16;

json error(const json& id, int code, const std::string& message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

json resultOf(const json& id, json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json initialize(const json& params, const RpcTarget& target) {
    const auto& versions = protocolVersions();
    std::string version = versions.front();
    if (params.is_object() && params.contains("protocolVersion") && params["protocolVersion"].is_string()) {
        const std::string asked = params["protocolVersion"].get<std::string>();
        if (std::find(versions.begin(), versions.end(), asked) != versions.end()) version = asked;
    }
    json out = {{"protocolVersion", version},
                {"capabilities", {{"tools", {{"listChanged", false}}}}},
                {"serverInfo", {{"name", target.serverName}, {"version", target.serverVersion}}}};
    if (!target.instructions.empty()) out["instructions"] = target.instructions;
    return out;
}

json listTools(const RpcTarget& target) {
    json tools = json::array();
    for (const auto& t : target.tools()) {
        tools.push_back({{"name", t.name},
                         {"description", t.description},
                         {"inputSchema", t.inputSchema},
                         {"annotations", {{"readOnlyHint", t.readOnly}, {"openWorldHint", false}}}});
    }
    return {{"tools", std::move(tools)}};
}

json callTool(const json& id, const json& params, const RpcTarget& target) {
    if (!params.is_object() || !params.contains("name") || !params["name"].is_string()) {
        return error(id, kInvalidParams, "tools/call takes a name and arguments");
    }
    const std::string name = params["name"].get<std::string>();
    const json arguments = params.contains("arguments") && !params["arguments"].is_null() ? params["arguments"]
                                                                                          : json::object();
    const auto tools = target.tools();
    const auto it = std::find_if(tools.begin(), tools.end(), [&](const ToolInfo& t) { return t.name == name; });
    if (it == tools.end()) return error(id, kInvalidParams, "no such tool: " + name.substr(0, 64));
    if (std::string bad = validate(it->inputSchema, arguments); !bad.empty()) {
        return error(id, kInvalidParams, name + ": " + bad);
    }
    const ToolAnswer answer = target.call(name, arguments);
    json content = json::array();
    if (!answer.value.is_null()) {
        const std::string text = answer.value.is_string()
                                     ? answer.value.get<std::string>()
                                     : answer.value.dump(-1, ' ', false, json::error_handler_t::replace);
        content.push_back({{"type", "text"}, {"text", text}});
    }
    if (!answer.imageBase64.empty()) {
        content.push_back({{"type", "image"}, {"data", answer.imageBase64}, {"mimeType", answer.imageMime}});
    }
    return resultOf(id, {{"content", std::move(content)}, {"isError", answer.failed}});
}

/// One message. Null for one that is not answered.
json answerOne(const json& message, const RpcTarget& target) {
    if (!message.is_object()) return error(nullptr, kInvalidRequest, "not a JSON-RPC message");
    const json id = message.contains("id") ? message["id"] : json();
    const bool notification = !message.contains("id");
    if (!notification && !id.is_string() && !id.is_number_integer()) {
        return error(nullptr, kInvalidRequest, "an id is a string or a number");
    }
    if (!message.contains("jsonrpc") || message["jsonrpc"] != "2.0") {
        return error(id, kInvalidRequest, "JSON-RPC 2.0 only");
    }
    // An answer to something this server never asked.
    if (!message.contains("method")) {
        if (message.contains("result") || message.contains("error")) return nullptr;
        return error(id, kInvalidRequest, "no method");
    }
    if (!message["method"].is_string()) return error(id, kInvalidRequest, "a method is a string");
    const std::string method = message["method"].get<std::string>();
    if (notification) return nullptr;
    const json params = message.contains("params") ? message["params"] : json::object();
    if (!params.is_object() && !params.is_array()) {
        return error(id, kInvalidRequest, "params are an object");
    }
    if (method == "initialize") return resultOf(id, initialize(params, target));
    if (method == "ping") return resultOf(id, json::object());
    if (method == "tools/list") return resultOf(id, listTools(target));
    if (method == "tools/call") return callTool(id, params, target);
    return error(id, kMethodNotFound, "no such method: " + method.substr(0, 64));
}

std::string write(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace

const std::vector<std::string>& protocolVersions() {
    static const std::vector<std::string> versions = {"2025-06-18", "2025-03-26", "2024-11-05"};
    return versions;
}

size_t nesting(std::string_view text) {
    size_t depth = 0;
    size_t most = 0;
    bool inString = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (c == '\\') ++i;
            else if (c == '"') inString = false;
        } else if (c == '"') {
            inString = true;
        } else if (c == '{' || c == '[') {
            most = std::max(most, ++depth);
        } else if ((c == '}' || c == ']') && depth > 0) {
            --depth;
        }
    }
    return most;
}

std::optional<std::string> answerRpc(std::string_view body, const RpcTarget& target) {
    if (nesting(body) > kMostNesting) return write(error(nullptr, kParseError, "nested too deeply"));
    const json message = json::parse(body.begin(), body.end(), nullptr, false);
    if (message.is_discarded()) return write(error(nullptr, kParseError, "not JSON"));
    try {
        if (!message.is_array()) {
            const json answer = answerOne(message, target);
            if (answer.is_null()) return std::nullopt;
            return write(answer);
        }
        if (message.empty() || message.size() > kMostBatch) {
            return write(error(nullptr, kInvalidRequest, "a batch of 1 to " + std::to_string(kMostBatch)));
        }
        json answers = json::array();
        for (const auto& one : message) {
            json answer = answerOne(one, target);
            if (!answer.is_null()) answers.push_back(std::move(answer));
        }
        if (answers.empty()) return std::nullopt;
        return write(answers);
    } catch (const std::exception& e) {
        return write(error(nullptr, kInternalError, e.what()));
    }
}

}  // namespace wowee::mcp
