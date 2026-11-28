// What is about: units, players, objects, corpses and spell areas.

#include <algorithm>
#include <cctype>
#include <memory>
#include <utility>
#include <vector>

#include "core/application.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "game/transport_manager.hpp"
#include "mcp/entity_kind.hpp"
#include "mcp/game_view.hpp"
#include "mcp/schema.hpp"
#include "mcp/tool_context.hpp"

namespace wowee::mcp {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// The guids of the battleground scoreboard's rows: bots the realm plays
/// as players are among them (mcp/entity_kind.hpp).
std::vector<uint64_t> scoredGuids(const game::GameHandler& gh) {
    std::vector<uint64_t> out;
    if (const auto* board = gh.getBgScoreboard()) {
        for (const auto& row : board->players) out.push_back(row.guid);
    }
    return out;
}

/// Whether it answers a filter's question, by its own description.
bool filterKeeps(const json& e, const std::string& filter) {
    const auto flag = [&](const char* name) { return e.contains(name) && e[name] == true; };
    if (filter == "hostile") {
        return e.contains("reaction") && (e["reaction"] == "hostile" || e["reaction"] == "hated");
    }
    if (filter == "attackable") return flag("attackable");
    if (filter == "friendly") {
        return e.contains("reaction") && (e["reaction"] == "friendly" || e["reaction"] == "honored" ||
                                          e["reaction"] == "revered" || e["reaction"] == "exalted");
    }
    if (filter == "npc") return e.contains("roles") && !e["roles"].empty();
    if (filter == "quest") return e.contains("quest");
    if (filter == "lootable") return flag("lootable");
    if (filter == "usable") return flag("usable");
    if (filter == "dead") return flag("dead");
    if (filter == "gm_only") return flag("gm_only");
    if (filter == "alive") return e.contains("hp") && !flag("dead");
    // A role: "vendor", "trainer", ...
    if (e.contains("roles")) {
        for (const auto& r : e["roles"]) {
            if (r == filter) return true;
        }
    }
    return false;
}

json entities(ToolContext& context, const json& args) {
    auto* gh = context.app.getGameHandler();
    if (!gh) throw ToolError("not in the world");
    const std::string kind = args.value("kind", "all");
    const double radius = args.value("radius", 60.0);
    const auto limit = args.value("limit", static_cast<size_t>(20));
    const std::string name = lower(args.value("name", std::string()));
    const std::string filter = args.value("filter", std::string());
    const bool sight = args.value("line_of_sight", false);

    const glm::vec3 me = playerAt(context);
    std::vector<std::pair<float, std::shared_ptr<game::Entity>>> sorted;
    const std::vector<uint64_t> scored = scoredGuids(*gh);
    size_t total = 0;
    for (const auto& [guid, entity] : gh->getEntityManager().getEntities()) {
        if (!entity || guid == gh->getPlayerGuid()) continue;
        if (kind == "transports") {
            // The game objects the transport manager moves: lifts, ships,
            // zeppelins, the tram (tester's wish, session 14).
            auto* manager = gh->getTransportManager();
            if (entity->getType() != game::ObjectType::GAMEOBJECT || !manager || !manager->getTransport(guid)) continue;
        } else if (!kindWanted(*entity, kind, scored)) {
            continue;
        }
        const float d = glm::length(entityAt(context, *entity) - me);
        if (d > radius) continue;
        if (!name.empty() && lower(entityName(context, *entity)).find(name) == std::string::npos) continue;
        ++total;
        sorted.emplace_back(d, entity);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    json list = json::array();
    size_t kept = 0;
    for (const auto& [d, entity] : sorted) {
        json e = entityJson(context, *entity, false);
        if (realmPlayedUnit(*entity, scored)) e["realm_player"] = true;
        if (!filter.empty() && !filterKeeps(e, filter)) continue;
        ++kept;
        if (list.size() >= limit) continue;
        if (sight) e["line_of_sight"] = inLineOfSight(context, *entity);
        list.push_back(std::move(e));
    }
    json out = {{"entities", std::move(list)}, {"in_radius", total}};
    if (kept > limit) out["left_out"] = kept - limit;
    return out;
}

json entityDetail(ToolContext& context, const json& args) {
    if (!args.contains("guid") && !args.contains("name") && !args.contains("unit")) {
        throw ToolError("a guid, a name or a unit is wanted");
    }
    const auto entity = findEntity(context, args);
    if (!entity) throw ToolError("no such entity in sight of the client");
    return entityJson(context, *entity, true);
}

}  // namespace

void addEntityTools(Toolbox& box) {
    using namespace schema;
    box.push_back(lookTool(
        "entities",
        "What the client has round the player, nearest first: guid, name, distance in yards, bearing in degrees "
        "clockwise from the facing (0 ahead, 90 right), level, health, reaction, npc roles, quest mark (! or ?), "
        "lootable, usable, gm_only (a unit the realm shows the player only as a game master: a trigger's GM model, "
        "stealthed or invisible, a spirit healer while alive - gm_only_why says which). kind players also lists the "
        "units the realm plays as players (battleground bots: realm_player true).",
        object({optional("kind", choice("which kind", {"all", "units", "players", "objects", "corpses", "dynamic", "transports"})),
                optional("radius", number("yards, 60 by default", 1, 500)),
                optional("limit", integer("how many, 20 by default", 1, 500)),
                optional("name", text("only names with this in them", 64)),
                optional("filter", text("hostile, attackable, friendly, npc, quest, lootable, usable, dead, alive, gm_only, or a "
                                        "role: vendor, trainer, questgiver, flightmaster, innkeeper, banker, ...",
                                        24)),
                optional("line_of_sight", flag("also say whether the player sees it"))}),
        entities));
    box.push_back(lookTool(
        "entity",
        "One entity in full: flags, auras, speed, owner, line of sight, where it is on the screen and the point a "
        "click would pick it at; for a game object its update fields (fields: GAMEOBJECT_FLAGS, BYTES_1's state, "
        "type, art kit and animation progress, DYNAMIC, faction, level, scale) and the template the realm answered "
        "(template: type, data words, size, quest items; null before the answer).",
        object({optional("guid", guid("0x...")), optional("name", text("the nearest with this in its name", 64)),
                optional("unit", text("target, player, pet, mouseover, focus, npc", 16))}),
        entityDetail));
}

}  // namespace wowee::mcp
