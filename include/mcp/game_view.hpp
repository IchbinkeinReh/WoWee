#pragma once

// The world as the tools speak of it: positions as `.gps` and /gps have them
// (x, y, z in yards), directions as degrees, guids as 0x....

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

namespace wowee::core { class Application; }
namespace wowee::game { class Entity; class GameHandler; }

namespace wowee::mcp {

using json = nlohmann::json;
struct ToolContext;

std::string guidText(uint64_t guid);
/// 0 for what is none.
uint64_t guidFromText(const std::string& text);

double rounded(double v, int places);
json vec3Json(const glm::vec3& v);

/// Where the player's model stands, and which way it faces in radians, 0 along
/// +x and counter-clockwise - the server's orientation.
glm::vec3 playerAt(ToolContext& context);
float playerFacing(ToolContext& context);
glm::vec3 entityAt(const game::Entity& entity);
/// A direction from the player, in degrees clockwise from where it faces:
/// 0 ahead, 90 to the right, -90 to the left.
double bearingTo(ToolContext& context, const glm::vec3& at);
/// The turn in radians that takes `facing` to look at `at`, -pi to pi,
/// counter-clockwise (left) positive.
float turnTo(const glm::vec3& from, float facing, const glm::vec3& at);

/// "dead", "ghost", "in_world", "char_select", "login", ...
std::string clientState(ToolContext& context);
bool inWorld(ToolContext& context);
/// The screens an account, a realm or the game's data is chosen on, where the
/// server presses nothing.
bool screenIsTheUsers(ToolContext& context);

/// Where a world point (`.gps` coordinates) is on the screen, in window
/// pixels from the top left; nothing behind the camera.
std::optional<glm::vec2> screenAt(ToolContext& context, const glm::vec3& at);
/// What the client's own picker says is under a point of the window, the
/// right click's or the left's. 0 for nothing.
uint64_t pickedAt(ToolContext& context, const glm::vec2& pixel);
/// Whether the interface, and not the world, has that point.
bool interfaceAt(ToolContext& context, const glm::vec2& pixel);
/// A point of the window that picks this one and nothing else.
std::optional<glm::vec2> clickPointOf(ToolContext& context, uint64_t guid, uint64_t* insteadPicked = nullptr);

/// What the entity is and where, short; `detail` has the rest.
json entityJson(ToolContext& context, const game::Entity& entity, bool detail);
std::string entityName(ToolContext& context, const game::Entity& entity);
/// Nothing between the player's eyes and the middle of it.
bool inLineOfSight(ToolContext& context, const game::Entity& entity);

/// The entity meant by a guid, or by part of a name (the nearest of that
/// name), or a unit word ("target", "player", "pet", "mouseover", "npc").
/// Null for none.
std::shared_ptr<game::Entity> findEntity(ToolContext& context, const json& args);

}  // namespace wowee::mcp
