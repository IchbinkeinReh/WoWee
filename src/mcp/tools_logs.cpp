// The client's log, the interface's events, the chat and the combat log, each
// since a cursor.

#include <algorithm>
#include <cctype>
#include <string>

#include "core/logger.hpp"
#include "mcp/game_view.hpp"
#include "mcp/lua_json.hpp"
#include "mcp/schema.hpp"
#include "mcp/tool_context.hpp"

namespace wowee::mcp {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int levelNamed(const std::string& name) {
    if (name == "debug") return 0;
    if (name == "info") return 1;
    if (name == "error") return 3;
    if (name == "fatal") return 4;
    return 2;
}

const char* levelName(int level) {
    constexpr const char* kNames[] = {"debug", "info", "warning", "error", "fatal"};
    return kNames[std::clamp(level, 0, 4)];
}

/// `since` and `limit`, and the cursor to come back with.
template <typename Ring>
json cursorOf(const Ring& ring, uint64_t since) {
    json out = {{"next", ring.last()}};
    if (since != 0 && ring.oldest() > since + 1) out["lost"] = ring.oldest() - since - 1;
    return out;
}

uint64_t sinceOf(const json& args) {
    return args.value("since", static_cast<uint64_t>(0));
}

json logTool(ToolContext& context, const json& args) {
    const uint64_t since = sinceOf(args);
    const int least = levelNamed(args.value("level", std::string("warning")));
    const std::string grep = lower(args.value("grep", std::string()));
    json out = cursorOf(context.log, since);
    json lines = json::array();
    for (const auto& e : context.log.after(since, args.value("limit", static_cast<size_t>(40)), [&](const LogLine& l) {
             return l.level >= least && (grep.empty() || lower(l.text).find(grep) != std::string::npos);
         })) {
        lines.push_back({{"seq", e.seq},
                         {"level", levelName(e.value.level)},
                         {"ago_s", rounded(context.now - e.value.at, 1)},
                         {"text", e.value.text.substr(0, 600)}});
    }
    out["lines"] = std::move(lines);
    return out;
}

json eventsTool(ToolContext& context, const json& args) {
    const uint64_t since = sinceOf(args);
    const std::string names = args.value("names", std::string());
    const auto wanted = [&](const std::string& name) {
        if (names.empty()) return true;
        size_t at = 0;
        while (at <= names.size()) {
            const size_t comma = names.find(',', at);
            const std::string one = names.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            if (!one.empty() && name.find(one) != std::string::npos) return true;
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        return false;
    };
    json out = cursorOf(context.events, since);
    json lines = json::array();
    for (const auto& e :
         context.events.after(since, args.value("limit", static_cast<size_t>(60)),
                              [&](const EventLine& l) { return wanted(l.name); })) {
        json line = {{"seq", e.seq}, {"event", e.value.name}, {"ago_s", rounded(context.now - e.value.at, 2)}};
        if (!e.value.args.empty()) line["args"] = e.value.args;
        lines.push_back(std::move(line));
    }
    out["events"] = std::move(lines);
    return out;
}

json chatTool(ToolContext& context, const json& args) {
    const uint64_t since = sinceOf(args);
    const std::string kind = args.value("kind", std::string());
    const std::string grep = lower(args.value("grep", std::string()));
    json out = cursorOf(context.chat, since);
    json lines = json::array();
    for (const auto& e :
         context.chat.after(since, args.value("limit", static_cast<size_t>(40)), [&](const EventLine& l) {
             if (!kind.empty() && l.name != "CHAT_MSG_" + kind) return false;
             return grep.empty() || (!l.args.empty() && lower(l.args[0]).find(grep) != std::string::npos);
         })) {
        json line = {{"seq", e.seq},
                     {"kind", e.value.name.substr(std::string("CHAT_MSG_").size())},
                     {"ago_s", rounded(context.now - e.value.at, 1)}};
        if (!e.value.args.empty()) line["text"] = plainText(e.value.args[0]);
        if (e.value.args.size() > 1 && !e.value.args[1].empty()) line["from"] = e.value.args[1];
        if (e.value.args.size() > 8 && !e.value.args[8].empty()) line["channel"] = e.value.args[8];
        lines.push_back(std::move(line));
    }
    out["lines"] = std::move(lines);
    return out;
}

json combatTool(ToolContext& context, const json& args) {
    const uint64_t since = sinceOf(args);
    const std::string grep = lower(args.value("grep", std::string()));
    json out = cursorOf(context.combat, since);
    json lines = json::array();
    for (const auto& e :
         context.combat.after(since, args.value("limit", static_cast<size_t>(40)), [&](const EventLine& l) {
             if (grep.empty()) return true;
             return std::any_of(l.args.begin(), l.args.end(),
                                [&](const std::string& a) { return lower(a).find(grep) != std::string::npos; });
         })) {
        // timestamp, event, source (guid, name, flags), dest (guid, name,
        // flags), and what belongs to the event.
        const auto& a = e.value.args;
        json line = {{"seq", e.seq}, {"ago_s", rounded(context.now - e.value.at, 1)}};
        if (a.size() > 1) line["event"] = a[1];
        if (a.size() > 3 && !a[3].empty()) line["source"] = a[3];
        if (a.size() > 6 && !a[6].empty()) line["dest"] = a[6];
        if (a.size() > 8) line["more"] = std::vector<std::string>(a.begin() + 8, a.end());
        lines.push_back(std::move(line));
    }
    out["lines"] = std::move(lines);
    return out;
}

}  // namespace

void addLogTools(Toolbox& box) {
    using namespace schema;
    const auto since = [] {
        return schema::optional("since", integer("only what came after this seq (the `next` of the last call)", 0,
                                                 9007199254740991LL));
    };
    const auto limit = [](const char* what) { return schema::optional("limit", integer(what, 1, 300)); };
    box.push_back(lookTool("log", "The client's own log: warnings and errors, or more with `level`.",
                           object({since(), limit("lines, 40 by default, the newest"),
                                   optional("level", choice("the least", {"debug", "info", "warning", "error"})),
                                   optional("grep", text("only lines with this in them", 64))}),
                           logTool, Tool::Where::Anywhere));
    box.push_back(lookTool("events", "The events the interface was told (PLAYER_TARGET_CHANGED, ...), with arguments.",
                           object({since(), limit("events, 60 by default, the newest"),
                                   optional("names", text("parts of names, comma separated", 256))}),
                           eventsTool, Tool::Where::Anywhere));
    box.push_back(lookTool("chat", "Chat lines, by kind (SYSTEM, SAY, LOOT, MONSTER_SAY, WHISPER, ...).",
                           object({since(), limit("lines, 40 by default, the newest"),
                                   optional("kind", text("one kind only", 48)),
                                   optional("grep", text("only lines with this in them", 64))}),
                           chatTool, Tool::Where::Anywhere));
    box.push_back(lookTool("combat_log", "The combat log.",
                           object({since(), limit("lines, 40 by default, the newest"),
                                   optional("grep", text("only lines with this in them", 64))}),
                           combatTool, Tool::Where::Anywhere));
}

}  // namespace wowee::mcp
