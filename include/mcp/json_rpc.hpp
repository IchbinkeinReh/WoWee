#pragma once

// The Model Context Protocol over JSON-RPC 2.0, as far as a server of tools
// needs it: initialize, ping, tools/list, tools/call, and the notifications it
// may be sent and has nothing to say to. Text in and text out; the tools
// behind it are whatever it is given.

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace wowee::mcp {

using json = nlohmann::json;

/// What a tool answers with.
struct ToolAnswer {
    json value;           ///< a string is given as it is, anything else as JSON
    bool failed = false;
    std::string imageBase64;
    std::string imageMime;
};

/// One tool as tools/list has it.
struct ToolInfo {
    std::string name;
    std::string description;
    json inputSchema;
    /// Whether it only looks (annotations.readOnlyHint).
    bool readOnly = false;
};

struct RpcTarget {
    std::string serverName = "wowee";
    std::string serverVersion;
    /// What the client is told of the whole at initialize.
    std::string instructions;
    std::function<std::vector<ToolInfo>()> tools;
    /// A call whose name and arguments have been checked against its schema.
    std::function<ToolAnswer(const std::string& name, const json& arguments)> call;
};

/// The protocol versions spoken, newest first.
const std::vector<std::string>& protocolVersions();

/// The answer to what a POST carried: a request, a notification, or an array
/// of them. Nothing for what is not answered - HTTP says 202 to it. Never
/// throws, whatever the text.
std::optional<std::string> answerRpc(std::string_view body, const RpcTarget& target);

/// How deep the brackets of a JSON text go, without parsing it.
size_t nesting(std::string_view text);

}  // namespace wowee::mcp
