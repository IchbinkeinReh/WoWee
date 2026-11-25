#pragma once

/// A game object's type. The client makes the object of the class its own
/// field names as it creates it (0x00714250 switches on +0x2d of the fields,
/// GAMEOBJECT_BYTES_1's second byte); the template, which is asked of the
/// server and may not be there yet, has no part in it.

#include "game/entity.hpp"
#include "game/go_interaction_anim.hpp"
#include "game/update_field_table.hpp"
#include "game/world_packets.hpp"

#include <cstdint>
#include <optional>

namespace wowee::game {

/// No type: neither field nor template to tell.
inline constexpr uint32_t kGameObjectTypeUnknown = 0xFFFFFFFFu;

/// The field's, or the template's on an expansion without the field.
[[nodiscard]] inline uint32_t gameObjectTypeOf(const Entity& object, const GameObjectQueryResponseData* info) {
    if (const uint16_t bytes1 = fieldIndex(UF::GAMEOBJECT_BYTES_1); bytes1 != 0xFFFF)
        return (object.getField(bytes1) >> 8) & 0xFFu;
    return info ? info->type : kGameObjectTypeUnknown;
}

/// Whether an object whose state (GAMEOBJECT_BYTES_1's first byte) has become
/// `state`, which it plays as `entered` (go_interaction::stateForChange), is in
/// the way of what moves: a door as that state has it (0x0070d8d0: in Closed
/// alone, so not while it closes, 0x0070d7e0 going to Closed as that has played;
/// nor destroyed), a trapdoor not while open (0); nullopt for what its state
/// does not change.
[[nodiscard]] inline std::optional<bool> stateCollides(uint32_t type, uint8_t state, uint8_t entered) {
    if (type == 35u) return state != 0;
    return go_interaction::collidesAfterEntering(type, entered);
}

}  // namespace wowee::game
