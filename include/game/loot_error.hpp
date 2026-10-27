#pragma once

// A refused loot, as the client shows it.
//
// SMSG_LOOT_RESPONSE with loot type 0 (LOOT_NONE) is a refusal, its reason
// the byte after (the server's LootError). 0x006d53b0 shows it through
// 0x005216f0 - so the character says the line the error has - when the
// response is to the loot the player asked for (+0x18e0, the guid its
// CMSG_LOOT named); a response for anything else is only shown when it is
// an already picked pocket, which is no CMSG_LOOT's.

#include <cstdint>

namespace wowee::game::loot_error {

struct Line {
    const char* key;
    const char* enUS;
};

/// LOOT_ERROR_ALREADY_PICKPOCKETED.
constexpr uint8_t kAlreadyPickpocketed = 15;

/// The game error for a refusal of the player's own loot, by the reason.
constexpr Line forReason(uint8_t reason) {
    switch (reason) {
    case 4: return {"ERR_LOOT_TOO_FAR", "You are too far away to loot that corpse."};
    case 5: return {"ERR_LOOT_BAD_FACING", "You must be facing the corpse to loot it."};
    case 6: return {"ERR_LOOT_LOCKED", "Someone is already looting that corpse."};
    case 8: return {"ERR_LOOT_NOTSTANDING", "You need to be standing up to loot something!"};
    case 9: return {"ERR_LOOT_STUNNED", "You can't loot anything while stunned!"};
    case 10: return {"ERR_LOOT_PLAYER_NOT_FOUND", "Player not found"};
    case 11: return {"ERR_PLAY_TIME_EXCEEDED", "Maximum play time exceeded"};
    case 12: return {"ERR_LOOT_MASTER_INV_FULL", "That player's inventory is full"};
    case 13: return {"ERR_LOOT_MASTER_UNIQUE_ITEM", "Player has too many of that item already"};
    case 14: return {"ERR_LOOT_MASTER_OTHER", "Can't assign item to that player"};
    case 16: return {"ERR_NOT_WHILE_SHAPESHIFTED", "You can't do that while shapeshifted."};
    // DIDNT_KILL and every other - an already picked pocket among them,
    // when it answers a CMSG_LOOT.
    default: return {"ERR_LOOT_DIDNT_KILL", "You don't have permission to loot that corpse."};
    }
}

/// What a refusal of anything else says: only an already picked pocket.
constexpr Line alreadyPickpocketed() {
    return {"ERR_ALREADY_PICKPOCKETED", "Your target has already had its pockets picked"};
}

}  // namespace wowee::game::loot_error
