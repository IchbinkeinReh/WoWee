// The lie of the land: the ground round the player, the ways that are open,
// and what a straight line meets.

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <vector>

#include "core/application.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "mcp/game_view.hpp"
#include "mcp/schema.hpp"
#include "mcp/tool_context.hpp"
#include "mcp/walk_planner.hpp"
#include "mcp/world_probe.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/renderer.hpp"
#include "rendering/water_renderer.hpp"

namespace wowee::mcp {

std::vector<probe::Facet> facetsAround(ToolContext& context, const glm::vec3& middle, float radius, float below,
                                       float above) {
    auto* renderer = context.app.getRenderer();
    auto* cc = renderer ? renderer->getCameraController() : nullptr;
    if (!cc) return {};
    const glm::vec3 reach(radius, radius, 0.0f);
    const auto facets = cc->gatherWorldFacets(middle - reach - glm::vec3(0.0f, 0.0f, below),
                                              middle + reach + glm::vec3(0.0f, 0.0f, above));
    return facets ? *facets : std::vector<probe::Facet>{};
}

std::vector<probe::Facet> facetsForWalk(ToolContext& context, const glm::vec3& from, const glm::vec3& to) {
    constexpr float kMost = 45.0f;
    const float length = glm::length(glm::vec2(from) - glm::vec2(to));
    const float radius = std::min(length * 0.5f + 15.0f, kMost);
    const glm::vec3 middle = from + (to - from) * (length > 1e-3f ? std::min(0.5f, 25.0f / length) : 0.0f);
    return facetsAround(context, glm::vec3(glm::vec2(middle), from.z), radius, 45.0f, 45.0f);
}

WalkPlan planWalk(ToolContext& context, const glm::vec3& from, const glm::vec3& to) {
    return planWalk(FacetIndex(facetsForWalk(context, from, to)), from, to);
}

namespace {

constexpr float kDegrees = 57.29578f;

json objectNamed(ToolContext& context, uint64_t guid) {
    json out = {{"guid", guidText(guid)}};
    auto* gh = context.app.getGameHandler();
    if (const auto e = gh ? gh->getEntityManager().getEntity(guid) : nullptr) out["name"] = entityName(context, *e);
    return out;
}

json terrain(ToolContext& context, const json& args) {
    const auto radius = static_cast<float>(args.value("radius", 10.0));
    const auto step = static_cast<float>(args.value("step", 2.0));
    if (radius / step > 20.0f) throw ToolError("at most 41 points a side: a larger step or a smaller radius");
    const glm::vec3 me = playerAt(context);
    const auto facets = facetsAround(context, me, radius + step, 60.0f, 30.0f);
    if (facets.empty()) throw ToolError("the ground here is not loaded");
    const auto grid = probe::groundGrid(facets, me, radius, step, 2.0f);
    auto* renderer = context.app.getRenderer();
    auto* water = renderer ? renderer->getWaterRenderer() : nullptr;
    json heights = json::array();
    json steep = json::array();
    json liquid = json::array();
    json built = json::array();
    bool anyLiquid = false;
    std::set<uint64_t> objects;
    for (int row = 0; row < grid.side; ++row) {
        json line = json::array();
        std::string steepLine;
        std::string builtLine;
        json liquidLine = json::array();
        for (int column = 0; column < grid.side; ++column) {
            const auto& g = grid.at(row, column);
            const glm::vec2 p = grid.place(row, column);
            line.push_back(g ? json(rounded(g->z - me.z, 1)) : json());
            steepLine += !g ? '?' : (g->slopeDegrees > 50.0f ? '#' : (g->slopeDegrees > 30.0f ? '+' : '.'));
            const bool built = g && (g->kind == probe::Facet::Kind::Building || g->kind == probe::Facet::Kind::Doodad);
            const uint64_t object =
                built ? context.app.gameObjectOfFace(g->kind == probe::Facet::Kind::Building, g->owner,
                                                     glm::vec3(p, g->z))
                      : 0;
            builtLine += !g              ? '?'
                         : object != 0   ? 'o'
                         : !built        ? '.'
                         : g->kind == probe::Facet::Kind::Building ? 'B'
                                                                   : 'd';
            if (object != 0) objects.insert(object);
            json depth;
            if (water) {
                if (const auto h = water->getWaterHeightAt(p.x, p.y); h && g && *h > g->z) {
                    depth = rounded(*h - g->z, 1);
                    anyLiquid = true;
                }
            }
            liquidLine.push_back(std::move(depth));
        }
        heights.push_back(std::move(line));
        steep.push_back(std::move(steepLine));
        built.push_back(std::move(builtLine));
        liquid.push_back(std::move(liquidLine));
    }
    json out = {{"at", vec3Json(me)},
                {"step", step},
                {"rows", "north (+x) to south, columns west (+y) to east, the player in the middle"},
                {"heights", std::move(heights)},
                {"slope", std::move(steep)},
                {"slope_key", ". up to 30 deg, + up to 50, # too steep to walk, ? nothing there"},
                {"made_of", std::move(built)},
                {"made_of_key", ". ground, B building, d doodad, o game object, ? nothing there"},
                {"faces", facets.size()}};
    if (!objects.empty()) {
        json list = json::array();
        for (const uint64_t guid : objects) list.push_back(objectNamed(context, guid));
        out["objects"] = std::move(list);
    }
    if (anyLiquid) out["liquid_depth"] = std::move(liquid);
    return out;
}

json lookAround(ToolContext& context, const json& args) {
    const int directions = args.value("directions", 16);
    const auto reach = static_cast<float>(args.value("reach", 20.0));
    const glm::vec3 me = playerAt(context);
    const float facing = playerFacing(context);
    const auto facets = facetsAround(context, me, reach + 2.0f, 40.0f, 25.0f);
    if (facets.empty()) throw ToolError("the ground here is not loaded");
    json ways = json::array();
    for (int i = 0; i < directions; ++i) {
        // Clockwise from ahead, as bearings are.
        const float bearing = 360.0f * static_cast<float>(i) / static_cast<float>(directions);
        const float angle = facing - bearing / kDegrees;
        const auto way = probe::wayAlong(facets, me, glm::vec2(std::cos(angle), std::sin(angle)), reach, 5.0f);
        json w = {{"bearing", rounded(bearing > 180.0f ? bearing - 360.0f : bearing, 1)},
                  {"clear", rounded(way.clear, 1)}};
        using Why = probe::Way::Why;
        if (way.why == Why::Wall) {
            const json what = faceOwnerJson(context, way.kind, way.owner, way.at);
            w["then"] = "wall of " + what["of"].get<std::string>();
            if (what.contains("guid")) w["object"] = {{"guid", what["guid"]}, {"name", what.value("name", "")}};
        }
        if (way.why == Why::Drop) w["then"] = "drop";
        if (way.why == Why::Steep) w["then"] = "too steep";
        ways.push_back(std::move(w));
    }
    return {{"at", vec3Json(me)}, {"facing", rounded(facing, 4)}, {"reach", reach}, {"ways", std::move(ways)}};
}

json raycast(ToolContext& context, const json& args) {
    auto* renderer = context.app.getRenderer();
    if (!renderer) throw ToolError("no world");
    const glm::vec3 from = playerAt(context) + glm::vec3(0.0f, 0.0f, static_cast<float>(args.value("height", 1.6)));
    glm::vec3 to(0.0f);
    json out;
    if (args.contains("to")) {
        if (args["to"].size() != 3) throw ToolError("to: [x, y, z]");
        to = glm::vec3(args["to"][0].get<float>(), args["to"][1].get<float>(), args["to"][2].get<float>());
    } else if (args.contains("guid") || args.contains("name")) {
        const auto entity = findEntity(context, args);
        if (!entity) throw ToolError("no such entity");
        to = entityAt(context, *entity);
        float radius = 0.0f;
        if (!context.app.getRenderBoundsForGuid(entity->getGuid(), to, radius)) to.z += 1.0f;
        out["entity"] = guidText(entity->getGuid());
    } else {
        const float angle = playerFacing(context) - static_cast<float>(args.value("bearing", 0.0)) / kDegrees;
        const float pitch = static_cast<float>(args.value("pitch", 0.0)) / kDegrees;
        const auto reach = static_cast<float>(args.value("reach", 100.0));
        to = from + reach * glm::vec3(std::cos(angle) * std::cos(pitch), std::sin(angle) * std::cos(pitch),
                                      std::sin(pitch));
    }
    const float length = glm::length(to - from);
    if (length > 1000.0f) throw ToolError("at most 1000 yards");
    out["from"] = vec3Json(from);
    out["to"] = vec3Json(to);
    out["length"] = rounded(length, 2);
    // The faces the character is moved against, game objects' among them; and
    // what the client asks for a line of sight, which are the ground's and the
    // buildings' from either side.
    auto* cc = renderer->getCameraController();
    float known = 0.0f;
    std::optional<probe::Hit> face;
    if (cc) {
        face = probe::castLine([cc](const glm::vec3& lo, const glm::vec3& hi) { return cc->gatherWorldFacets(lo, hi); },
                               from, to, &known);
    }
    std::optional<float> distance;
    if (face) distance = face->distance;
    if (const auto sight = renderer->intersectSegment(from, to); sight && (!distance || *sight * length < *distance)) {
        distance = *sight * length;
        face.reset();
    }
    if (!distance) {
        out["hit"] = nullptr;
        if (known < length) out["looked_at_yards"] = rounded(known, 1);
        return out;
    }
    const glm::vec3 at = length > 0.0f ? from + (to - from) * (*distance / length) : from;
    out["hit"] = vec3Json(at);
    out["hit_distance"] = rounded(*distance, 2);
    if (face) out.update(faceOwnerJson(context, face->kind, face->owner, at));
    else out["of"] = "building or ground";
    return out;
}

json findPath(ToolContext& context, const json& args) {
    glm::vec3 to(0.0f);
    if (args.contains("to") && args["to"].size() >= 2) {
        to = glm::vec3(args["to"][0].get<float>(), args["to"][1].get<float>(),
                       args["to"].size() > 2 ? args["to"][2].get<float>() : playerAt(context).z);
    } else if (const auto entity = findEntity(context, args)) {
        to = entityAt(context, *entity);
    } else {
        throw ToolError("to, guid or name");
    }
    const glm::vec3 me = playerAt(context);
    const FacetIndex world(facetsForWalk(context, me, to));
    const auto plan = planWalk(world, me, to);
    json points = json::array();
    for (const auto& p : plan.points) points.push_back(vec3Json(p));
    json out = {{"reaches", plan.reaches},
                {"ends_yards_off", rounded(plan.left, 1)},
                {"points", std::move(points)},
                {"looked_at", plan.visited},
                {"faces", world.size()}};
    // A plan that got no further than where it began says why each of the
    // first steps round the feet is not taken (tester: walk_to out of the
    // Warsong flag rooms, 'looked_at 1').
    if (!plan.reaches && plan.visited <= 1) {
        json steps = json::object();
        const char* names[8] = {"north", "north-east", "east", "south-east", "south", "south-west", "west",
                                "north-west"};
        const glm::vec2 dirs[8] = {{1, 0}, {1, -1}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1}, {0, 1}, {1, 1}};
        for (int i = 0; i < 8; ++i) {
            const std::string why = whyNoStep(world, me, glm::vec2(me) + dirs[i]);
            steps[names[i]] = why.empty() ? std::string("open") : why;
        }
        out["first_steps"] = std::move(steps);
    }
    return out;
}

}  // namespace

void addTerrainTools(Toolbox& box) {
    using namespace schema;
    const json place = list("x, y, z in .gps coordinates", json{{"type", "number"}}, 3);
    box.push_back(lookTool("terrain",
                           "A square of the ground round the player, as the character is moved on it: heights "
                           "relative to the feet, slopes, what it is made of, depth of water.",
                           object({optional("radius", number("yards, 10 by default", 1, 60)),
                                   optional("step", number("yards between points, 2 by default", 0.5, 10))}),
                           terrain));
    box.push_back(lookTool("look_around",
                           "How far one can walk in each direction before a wall (of what: a game object's with its "
                           "guid), a drop or a slope.",
                           object({optional("directions", integer("how many, 16 by default", 4, 72)),
                                   optional("reach", number("yards, 20 by default", 2, 50))}),
                           lookAround));
    box.push_back(lookTool("raycast",
                           "What a straight line from the player meets (ground, buildings, doodads, game objects - "
                           "lifts, ships, doors - with their guid): towards a place, an entity, or a bearing and pitch "
                           "in degrees.",
                           object({optional("to", place), optional("guid", guid("0x...")),
                                   optional("name", text("the nearest with this in its name", 64)),
                                   optional("bearing", number("degrees clockwise from ahead", -360, 360)),
                                   optional("pitch", number("degrees up", -90, 90)),
                                   optional("reach", number("yards, 100 by default", 1, 1000)),
                                   optional("height", number("over the feet, 1.6 by default", 0, 10))}),
                           raycast));
    box.push_back(lookTool("path",
                           "The way walk_to would take now, as far as the ground is known: points, and whether they "
                           "get there.",
                           object({optional("to", place), optional("guid", guid("0x...")),
                                   optional("name", text("the nearest with this in its name", 64))}),
                           findPath));
}

}  // namespace wowee::mcp
