#pragma once

#include <cstdint>

// What the pointer over a unit shows (0x004f7a50) and what a right click on
// one does (0x00731260). Both hold off while the player looks through another
// object, or sits in a seat that lets its passenger interact with nothing.

namespace wowee::game::unit_pointer {

/// What both ask first.
struct Player {
    /// Looks through an object of its own (PLAYER_FARSIGHT, +0x1858 0x1).
    bool seesElsewhere = false;
    /// That object is a unit the player possesses (0x006dd060).
    bool possesses = false;
    /// 0x006d7aa0: on no vehicle in sight, or in a seat with flags 0x80000000.
    bool mayInteract = true;
};

/// 0x006d7aa0.
inline constexpr uint32_t kSeatInteracts = 0x80000000u;

/// The kinds of ui/cursor_art.hpp; kUnable further on for what is out of
/// reach. 0 is the pointer as it is.
inline constexpr int kUnable = 0x1a;

struct CursorAsk {
    Player player;
    bool autoLoots = false;       ///< 0x00513700
    bool pacified = false;        ///< UNIT_FIELD_FLAGS 0x20000
    bool hasControl = true;       ///< 0x00bcfb8c
    float distanceSq = 0.0f;      ///< from the player
    // The unit.
    bool alive = true;
    bool isPlayer = false;
    bool serviceOffered = false;  ///< 0x00729620
    uint32_t npcFlags = 0;
    float combatReach = 0.0f;
    uint8_t questStatus = 0;      ///< +0x90, as the server names it
    int icon = 0;                 ///< the creature's IconName, a kind or the custom one's
    int iconUnable = 0;           ///< the same with "Unable" before it
    bool seatFree = false;        ///< 0x00723050: a group member's vehicle with a seat left
    bool lootable = false;        ///< 0x00717b60
    bool mayLoot = false;         ///< 0x006d5a60, or the player loots it now
    bool skinnable = false;       ///< UNIT_FLAG 0x4000000 with the player's spell for it, not a player to assist
    float skinReach = 0.0f;       ///< that spell's
    int skinKind = 0;             ///< 0x00715e50: 0 skin, 1 herbs, 2 ore, 3 parts
    bool attackable = false;      ///< 0x00729a70
    float meleeReach = 5.0f;      ///< 0x004f5f40
    bool playerIsHorde = false;   ///< its race's faction (0x006d6e90)
};

/// 0x00744640: what a quest giver's status shows before all else.
[[nodiscard]] constexpr int questCursor(uint8_t status) {
    switch (status) {
        case 2: case 3: case 4: case 5: case 7: case 8: return 0x17;
        case 6: return 0x18;
        case 9: case 10: return 0x19;
        default: return 0;
    }
}

/// The services by their flags, the first that is there.
[[nodiscard]] constexpr int serviceCursor(uint32_t npcFlags) {
    constexpr struct { uint32_t flags; int kind; } kLadder[] = {
        {0x1000, 18},             // repairs
        {0x10000, 22},            // innkeeper
        {0x2000, 9},              // flight master
        {0x10, 10},               // trainer
        {0x4000 | 0x8000, 6},     // spirit healer, spirit guide
        {0x20000 | 0x800000, 3},  // banker, guild banker
        {0x40000 | 0x80000 | 0x100000, 6},  // petitions, tabards, battlemaster
        {0x200000, 3},            // auctioneer
        {0x400000, 6},            // stable master
        {0x80, 8},                // vendor
        {0x1, 6},                 // gossip
        {0x1000000, 5},           // spell click
    };
    for (const auto& row : kLadder)
        if ((npcFlags & row.flags) != 0) return row.kind;
    return 0;
}

/// 0x004f7a50: the kind, 0 for the pointer as it is.
[[nodiscard]] constexpr int cursor(const CursorAsk& a) {
    const auto reached = [&](float reach) { return reach * reach >= a.distanceSq; };
    const auto kind = [](int k, bool within) { return within ? k : k + kUnable; };
    if (a.player.seesElsewhere || !a.player.mayInteract)
        return a.alive && a.player.possesses && a.attackable ? 4 : 0;
    if (a.serviceOffered && a.npcFlags != 0) {
        const bool within = reached(a.combatReach + 4.0f);
        if (const int quest = questCursor(a.questStatus); quest != 0) return kind(quest, within);
        if (a.icon != 0) return within ? a.icon : a.iconUnable;
        if (const int service = serviceCursor(a.npcFlags); service != 0) return kind(service, within);
        return a.seatFree ? kind(0x1a, within) : 0;
    }
    if (a.lootable) return kind(a.autoLoots ? 16 : 8, a.mayLoot);
    if (a.skinnable) {
        const bool within = reached(a.skinReach);
        if (a.isPlayer) return kind(a.playerIsHorde ? 21 : 20, within);
        return kind(a.skinKind == 0 ? 12 : a.skinKind == 1 ? 13 : 11, within);
    }
    if (a.attackable && !a.pacified && a.hasControl) return kind(4, reached(a.meleeReach));
    return 0;
}

struct ClickAsk {
    Player player;
    bool playerAlive = true;     ///< UNIT_FIELD_HEALTH above 0 (a ghost has some)
    bool playerIsMover = true;   ///< the player moves itself (0x004d43c0)
    // The unit.
    bool alive = true;
    bool shownDead = false;      ///< UNIT_DYNAMIC_FLAGS 0x20
    bool maySpeak = false;       ///< 0x00729530
    bool mayAttack = false;      ///< 0x00729a70
};

enum class Click : uint8_t {
    Nothing,
    Dead,       ///< looted, skinned or left
    Speak,      ///< 0x006ddbb0
    Attack,     ///< 0x006e4950; where the player may not start one, its voice
    Voice,      ///< 0x0072a680
    PetAttack,  ///< 0x005d4620: by the unit the player possesses
};

/// 0x00731260.
[[nodiscard]] constexpr Click click(const ClickAsk& a) {
    if (!a.playerAlive && !a.player.possesses) return Click::Nothing;
    if (a.player.seesElsewhere)
        return a.alive && a.player.possesses && a.mayAttack ? Click::PetAttack : Click::Nothing;
    if (a.player.mayInteract) {
        if (!a.alive && !a.shownDead && a.playerIsMover) return Click::Dead;
        if (a.maySpeak) return Click::Speak;
    }
    if (a.mayAttack) return Click::Attack;
    return a.player.mayInteract ? Click::Voice : Click::Nothing;
}

}  // namespace wowee::game::unit_pointer
