#pragma once

/// Whose a thing of the world is, for the rules on what is done towards other
/// players (send_guard.hpp): who summoned a creature, made a game object, cast
/// an area, left a corpse. Each kind keeps it in a field of its own, and the
/// creatures' is in a game object its dynamic flags and its faction, which
/// together look like a player's guid.

#include "game/entity.hpp"
#include "game/update_field_table.hpp"

#include <cstdint>

namespace wowee::mcp {

/// The first of the two update fields of the owner's guid, 0xFFFF for a kind
/// that has none.
[[nodiscard]] inline uint16_t ownerField(game::ObjectType kind) {
    switch (kind) {
        case game::ObjectType::UNIT: return game::fieldIndex(game::UF::UNIT_FIELD_SUMMONEDBY_LO);
        case game::ObjectType::GAMEOBJECT: return game::fieldIndex(game::UF::GAMEOBJECT_FIELD_CREATED_BY);
        case game::ObjectType::DYNAMICOBJECT: return game::fieldIndex(game::UF::DYNAMICOBJECT_CASTER);
        case game::ObjectType::CORPSE: return 6;   // CORPSE_FIELD_OWNER, the first of its own
        default: return 0xFFFF;
    }
}

[[nodiscard]] inline uint64_t ownerGuid(const game::Entity& entity) {
    const uint16_t at = ownerField(entity.getType());
    if (at == 0xFFFF) return 0;
    const auto& fields = entity.getFields();
    const auto word = [&fields](uint16_t i) -> uint64_t {
        const auto it = fields.find(i);
        return it == fields.end() ? 0 : it->second;
    };
    return word(at) | word(static_cast<uint16_t>(at + 1)) << 32;
}

}  // namespace wowee::mcp
