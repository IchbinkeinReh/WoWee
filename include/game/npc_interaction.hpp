#pragma once

// What a right-click on a unit in reach does, as 0x006ddbb0 decides it. The
// NPC flags are tried in a fixed order and the first that matches sends its
// own hello (gossip, questgiver, flight master, vendor, trainer, the spirits,
// innkeeper, banker, petitioner, tabard, battlemaster, auctioneer, stable,
// guild banker). Only when none of those matches does the spell click come
// (0x0071c570, CMSG_SPELLCLICK), then a group member's vehicle with a free
// seat (0x00723050, CMSG_PLAYER_VEHICLE_ENTER), then a mailbox NPC's mail
// (0x0056da60). A flight master whose node the player has not learned is
// sent CMSG_ENABLETAXI first, and only a questgiver with something to say
// goes on past it.

#include <cstdint>

namespace wowee::game::npc_interaction {

enum class Action : uint8_t {
    Hello,          ///< one of the NPC hellos (hello() says which)
    SpellClick,     ///< CMSG_SPELLCLICK, 0x006d2740
    VehicleEnter,   ///< CMSG_PLAYER_VEHICLE_ENTER, 0x006d27c0
    Mailbox,        ///< the mail window, 0x0056da60
    Nothing,
};

/// Which hello (0x006ddbb0's chain, in its order).
enum class Hello : uint8_t {
    None,
    Gossip,             ///< 0x1: CMSG_GOSSIP_HELLO (0x006d1e20)
    QuestGiver,         ///< a status past 1: CMSG_QUESTGIVER_HELLO (0x006d1ea0)
    FlightMaster,       ///< 0x2000: CMSG_TAXIQUERYAVAILABLENODES (0x006d6320)
    Vendor,             ///< 0x80: CMSG_LIST_INVENTORY (0x006daf60)
    Trainer,            ///< 0x10: CMSG_TRAINER_LIST (0x006d1f20)
    SpiritHealer,       ///< 0x4000: CMSG_SPIRIT_HEALER_ACTIVATE (0x006db010)
    SpiritGuide,        ///< 0x8000: the area spirit healer, while a ghost (0x006db180)
    Innkeeper,          ///< 0x10000: CMSG_BINDER_ACTIVATE (0x006d2120)
    Banker,             ///< 0x20000: CMSG_BANKER_ACTIVATE (0x006d2340)
    Petitioner,         ///< 0x40000: CMSG_PETITION_SHOWLIST (0x006d2480)
    TabardDesigner,     ///< 0x80000: MSG_TABARDVENDOR_ACTIVATE (0x006d2500)
    Battlemaster,       ///< 0x100000: CMSG_BATTLEMASTER_HELLO (0x006d25c0)
    Auctioneer,         ///< 0x200000: MSG_AUCTION_HELLO (0x006d2640)
    StableMaster,       ///< 0x400000: MSG_LIST_STABLED_PETS (0x006d26c0)
    GuildBanker,        ///< 0x800000: CMSG_GUILD_BANKER_ACTIVATE (0x006d23c0)
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
    bool taxiNodeUnknown = false;  ///< SMSG_TAXINODE_STATUS said 0 (+0x94 == 7)
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

/// 0x006d1de0: a questgiver with something to say.
[[nodiscard]] inline bool questGiverHas(const Unit& u) {
    return (u.npcFlags & kQuestGiverFlag) && u.questStatus > 1;
}

/// The hello the flags call for, in 0x006ddbb0's order.
[[nodiscard]] inline Hello hello(const Unit& u) {
    const uint32_t f = u.npcFlags;
    if (f & 0x1u) return Hello::Gossip;
    if (questGiverHas(u)) return Hello::QuestGiver;
    if (f & 0x2000u) return Hello::FlightMaster;
    if (f & 0x80u) return Hello::Vendor;
    if (f & 0x10u) return Hello::Trainer;
    if (f & 0x4000u) return Hello::SpiritHealer;
    if (f & 0x8000u) return Hello::SpiritGuide;
    if (f & 0x10000u) return Hello::Innkeeper;
    if (f & 0x20000u) return Hello::Banker;
    // 0x004f5f80 (petitioner and tabard designer both) comes to the same.
    if (f & 0x40000u) return Hello::Petitioner;
    if (f & 0x80000u) return Hello::TabardDesigner;
    if (f & 0x100000u) return Hello::Battlemaster;
    if (f & 0x200000u) return Hello::Auctioneer;
    if (f & 0x400000u) return Hello::StableMaster;
    if (f & 0x800000u) return Hello::GuildBanker;
    return Hello::None;
}

inline constexpr uint32_t kMailboxFlag = 0x04000000u;

[[nodiscard]] inline Action decide(const Unit& u) {
    if (hello(u) != Hello::None) return Action::Hello;
    if (spellClickable(u)) return Action::SpellClick;
    if (vehicleEnterable(u)) return Action::VehicleEnter;
    if (u.npcFlags & kMailboxFlag) return Action::Mailbox;
    return Action::Nothing;
}

/// What 0x00729620 reads of the player and of a unit before the client offers
/// that unit's services: the cursor over it (0x004f7a50) and the nearest-NPC
/// search (0x00519e80).
struct ServiceAsk {
    // The player.
    uint32_t playerUnitFlags = 0;     ///< UNIT_FIELD_FLAGS (0x100000: on a taxi)
    uint32_t playerFlags2 = 0;        ///< UNIT_FIELD_FLAGS_2 (0x4000)
    bool playerCharmed = false;       ///< UNIT_FIELD_CHARMEDBY set
    bool playerAlive = true;          ///< UNIT_FIELD_HEALTH above 0
    bool playerGhost = false;         ///< PLAYER_FLAGS 0x10
    bool playerInForm = false;        ///< 0x00721ca0: a form that is not a stance
    uint32_t formFlags = 0;           ///< its SpellShapeshiftForm Flags (8)
    bool wouldAutoUnshift = false;    ///< 0x0071b130
    // The unit.
    bool alive = true;
    uint32_t typeFlags = 0;           ///< the creature's type flags (+0x964 +0xc)
    bool charmed = false;             ///< UNIT_FIELD_CHARMEDBY set
    bool vehicle = false;             ///< has a vehicle (+0xf5c +0xc)
    bool inCombat = false;            ///< 0x0071afb0: UNIT_FLAG 0x800 or a melee target
    bool isPlayer = false;
    uint32_t unitFlags = 0;           ///< UNIT_FIELD_FLAGS (0x2000000)
    uint32_t npcFlags = 0;            ///< UNIT_NPC_FLAGS
    uint32_t flags2 = 0;              ///< UNIT_FIELD_FLAGS_2 (0x4000)
    bool createdByOther = false;      ///< UNIT_FIELD_CREATEDBY is not the player
    bool spellClickable = false;      ///< 0x0071c570
    int reactionToPlayer = 3;         ///< 0x007251c0, 0 hated..7 exalted
    int playerReaction = 3;           ///< how the player regards it
};

/// 0x00729530: whether the player may speak to the unit.
[[nodiscard]] constexpr bool mayInteract(const ServiceAsk& a) {
    // A ghost speaks only to what ghosts see (type flag 2).
    if (a.playerGhost && (a.typeFlags & 0x2u) == 0) return false;
    // A gossip creature flagged 0x800000 speaks only to whoever made it.
    if ((a.npcFlags & 0x1u) != 0 && !a.isPlayer && (a.typeFlags & 0x800000u) != 0 && a.createdByOther)
        return false;
    // Selectable, with services, and neutral or better both ways round -
    // or UNIT_FIELD_FLAGS_2 0x4000 on the unit.
    if ((a.unitFlags & 0x2000000u) == 0 && a.npcFlags != 0) {
        if (a.reactionToPlayer > 2 && a.playerReaction > 2) return true;
        if ((a.flags2 & 0x4000u) != 0) return true;
    }
    return false;
}

/// 0x00729620 (0x00729530 at its end): whether the client offers the
/// unit's services at all.
[[nodiscard]] constexpr bool serviceOffered(const ServiceAsk& a) {
    // The player alive, off a taxi and not charmed.
    if ((a.playerUnitFlags & 0x100000u) != 0 || a.playerCharmed || !a.playerAlive) return false;
    // In a form only one flagged 8, or one autoUnshift would leave.
    if (a.playerInForm && (a.formFlags & 0x8u) == 0 && !a.wouldAutoUnshift) return false;
    // The unit alive, or of a kind spoken to dead (type flag 0x80); and not
    // charmed, unless it is a vehicle.
    if (!a.alive && (a.typeFlags & 0x80u) == 0) return false;
    if (a.charmed && !a.vehicle) return false;
    // Not in combat, unless its type flags 0x40000 or the player's
    // UNIT_FIELD_FLAGS_2 0x4000 allow it.
    if (a.inCombat && (a.typeFlags & 0x40000u) == 0 && (a.playerFlags2 & 0x4000u) == 0) return false;
    // A spell click unit must take the click (0x0071c570).
    if ((a.npcFlags & 0x1000000u) != 0 && !a.spellClickable) return false;
    return mayInteract(a);
}

/// CONFIRM_XP_LOSS's one "%d" (0x006db010, the spirit healer's hello and
/// SMSG_SPIRIT_HEALER_CONFIRM): the active player's PLAYER_NEXT_LEVEL_XP
/// (0x0060a600, field +0x79c) as an x87 fild, times the float 0.05 at
/// 0x00a31bb0, truncated (_ftol2 0x0088b9c0) - five percent of the level.
[[nodiscard]] inline int confirmXpLossArg(uint32_t nextLevelXp) {
    return static_cast<int>(static_cast<double>(static_cast<int32_t>(nextLevelXp)) * static_cast<double>(0.05f));
}

}  // namespace wowee::game::npc_interaction
