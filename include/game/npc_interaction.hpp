#pragma once

// What a right-click on a unit in reach does, as 0x006ddbb0 decides it. The
// NPC flags are tried in a fixed order and the first that matches sends its
// own hello (gossip, questgiver, flight master, vendor, trainer, the spirits,
// innkeeper, banker, petitioner, tabard, battlemaster, auctioneer, stable,
// guild banker). Only when none of those matches does the spell click come
// (0x0071c570, CMSG_SPELLCLICK), then a group member's vehicle with a free
// seat (0x00723050, CMSG_PLAYER_VEHICLE_ENTER).
//
// This client answers every one of the hellos with CMSG_GOSSIP_HELLO, so the
// order matters here only for what comes after them: a spell-click unit that
// is also, say, a vendor is a vendor first.

#include <cstdint>

namespace wowee::game::npc_interaction {

enum class Action : uint8_t {
    Hello,          ///< one of the NPC hellos
    SpellClick,     ///< CMSG_SPELLCLICK, 0x006d2740
    VehicleEnter,   ///< CMSG_PLAYER_VEHICLE_ENTER, 0x006d27c0
    Nothing,
};

/// The flags 0x006ddbb0 tests before the spell click, each with a hello of its
/// own: 0x1 gossip, 0x10 trainer, 0x80 vendor, 0x2000 flight master, 0x4000
/// spirit healer, 0x8000 spirit guide, then 0x10000 to 0x800000 (innkeeper,
/// banker, petitioner, tabard designer, battlemaster, auctioneer, stable
/// master, guild banker). The questgiver bit is tested apart, by its status.
inline constexpr uint32_t kHelloFlags = 0x00ffe091u;
inline constexpr uint32_t kQuestGiverFlag = 0x00000002u;
inline constexpr uint32_t kSpellClickFlag = 0x01000000u;

/// UNIT_FIELD_FLAGS_2 bits 0x0071c570 reads.
inline constexpr uint32_t kFlag2RestrictPartyInteraction = 0x00001000u;
inline constexpr uint32_t kFlag2PreventSpellClick = 0x00002000u;

struct Unit {
    uint32_t npcFlags = 0;
    uint32_t flags2 = 0;      ///< UNIT_FIELD_FLAGS_2
    uint8_t questStatus = 0;  ///< the questgiver status, 0 none, 1 unavailable
    bool isPlayer = false;
    bool isSelf = false;
    bool inGroup = false;     ///< in the player's party or raid (0x0052c8c0, 0x00573200)
    uint8_t emptySeats = 0;   ///< its vehicle's empty seats (+0x16d, 0x00757ef0)
};

/// 0x0071c570: a spell click, unless the unit forbids it; one that keeps it
/// to its party only answers a group member.
[[nodiscard]] inline bool spellClickable(const Unit& u) {
    if ((u.npcFlags & kSpellClickFlag) == 0) return false;
    if (u.flags2 & kFlag2PreventSpellClick) return false;
    if ((u.flags2 & kFlag2RestrictPartyInteraction) && !u.inGroup) return false;
    return true;
}

/// 0x00723050: another player, in the group, whose vehicle has a free seat.
[[nodiscard]] inline bool vehicleEnterable(const Unit& u) {
    return u.isPlayer && !u.isSelf && u.emptySeats != 0 && u.inGroup;
}

[[nodiscard]] inline Action decide(const Unit& u) {
    if (u.npcFlags & kHelloFlags) return Action::Hello;
    // 0x006d1de0: a questgiver with something to say.
    if ((u.npcFlags & kQuestGiverFlag) && u.questStatus > 1) return Action::Hello;
    if (spellClickable(u)) return Action::SpellClick;
    if (vehicleEnterable(u)) return Action::VehicleEnter;
    return Action::Nothing;
}

}  // namespace wowee::game::npc_interaction
