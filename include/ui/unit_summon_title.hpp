#pragma once

#include <string>

namespace wowee {
namespace game {
class GameHandler;
class Entity;
class Unit;
}  // namespace game
namespace ui {

/// 0x0061e830: the line under the name of a unit something made - "%s's
/// Pet", "%s's Minion" and the rest (UNITNAME_SUMMON_TITLE1-12) with its
/// owner's name; empty for none, or while the owner's name is being asked
/// for. The nameplates and the unit tooltip both write it.
std::string summonTitleLine(game::GameHandler& gh, const game::Entity& e, const game::Unit& unit);

}  // namespace ui
}  // namespace wowee
