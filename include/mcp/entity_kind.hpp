#pragma once

/// Which kind of thing of the world the entities tool lists for its `kind`
/// argument. "players" are the player objects and the units the realm plays
/// as players: a battleground's bots come as creatures (creature guids, type
/// UNIT) and were left out, although the scoreboard lists them (tester's
/// wish, session 23). A unit counts as one when its guid is a row of the
/// battleground's scoreboard, or when it is drawn as a player character
/// (UNIT_FLAG2_MIRROR_IMAGE, game/mirror_image.hpp) without anyone who
/// summoned or made it - a mage's mirror image has its caster there.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "game/entity.hpp"
#include "game/mirror_image.hpp"
#include "game/update_field_table.hpp"

namespace wowee::mcp {

/// Whether a unit is a player the realm plays: `scored` are the guids of
/// the scoreboard's rows.
[[nodiscard]] inline bool realmPlayedUnit(const game::Entity& entity, const std::vector<uint64_t>& scored) {
    if (entity.getType() != game::ObjectType::UNIT) return false;
    if (std::find(scored.begin(), scored.end(), entity.getGuid()) != scored.end()) return true;
    const auto& fields = entity.getFields();
    const auto word = [&fields](uint16_t i) -> uint64_t {
        if (i == 0xFFFF) return 0;
        const auto it = fields.find(i);
        return it == fields.end() ? 0 : it->second;
    };
    if ((word(game::fieldIndex(game::UF::UNIT_FIELD_FLAGS_2)) & game::kUnitFlag2MirrorImage) == 0) return false;
    const uint16_t made = game::fieldIndex(game::UF::UNIT_FIELD_CREATEDBY);
    const uint64_t maker = made == 0xFFFF ? 0 : word(made) | word(static_cast<uint16_t>(made + 1)) << 32;
    return game::unitSummonedByGuid(entity) == 0 && maker == 0;
}

/// Whether the entity is of the kind asked for ("units", "players",
/// "objects", "corpses", "dynamic"; anything else is all of them). A unit
/// the realm plays as a player is both a unit and a player.
[[nodiscard]] inline bool kindWanted(const game::Entity& entity, const std::string& kind,
                                     const std::vector<uint64_t>& scored) {
    using T = game::ObjectType;
    const T type = entity.getType();
    if (kind == "units") return type == T::UNIT;
    if (kind == "players") return type == T::PLAYER || realmPlayedUnit(entity, scored);
    if (kind == "objects") return type == T::GAMEOBJECT;
    if (kind == "corpses") return type == T::CORPSE;
    if (kind == "dynamic") return type == T::DYNAMICOBJECT;
    return type == T::UNIT || type == T::PLAYER || type == T::GAMEOBJECT || type == T::CORPSE ||
           type == T::DYNAMICOBJECT;
}

}  // namespace wowee::mcp
