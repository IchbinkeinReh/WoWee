#pragma once

/// The unit a spell is cast at, as the client takes one (Spell_C.cpp): what a
/// unit fills of the targets a spell still needs (0x0080bc80), the caster's
/// relations it asks - may help (0x007293d0), may attack (0x00729740) - the
/// reach between them (0x007ff480, 0x00803850), and what 0x0080cce0 does
/// when no unit fills them. Arithmetic only: the units are the caller's.

#include "game/pvp_flags.hpp"

#include <cstdint>

namespace wowee::game::unit_target {

/// OBJECT_FIELD_TYPE bits.
inline constexpr uint32_t kTypeUnit = 0x8;
inline constexpr uint32_t kTypePlayer = 0x10;
inline constexpr uint32_t kTypeGameObject = 0x20;

/// SpellCastTargetFlags 0x0080bc80 fills from a unit.
inline constexpr uint32_t kUnit = 0x2;
inline constexpr uint32_t kUnitRaid = 0x4;
inline constexpr uint32_t kUnitParty = 0x8;
inline constexpr uint32_t kUnitEnemy = 0x80;
inline constexpr uint32_t kUnitAlly = 0x100;
inline constexpr uint32_t kCorpseEnemy = 0x200;
inline constexpr uint32_t kUnitDead = 0x400;
inline constexpr uint32_t kGameObject = 0x800;
inline constexpr uint32_t kGameObjectItem = 0x4000;
inline constexpr uint32_t kCorpseAlly = 0x8000;
inline constexpr uint32_t kUnitMinipet = 0x10000;

/// UNIT_FIELD_FLAGS (+0xd4) bits the relations read.
inline constexpr uint32_t kFlagPlayerControlled = 0x8;
inline constexpr uint32_t kFlagImmuneToPc = 0x100;
inline constexpr uint32_t kFlagImmuneToNpc = 0x200;
inline constexpr uint32_t kFlagNotSelectable = 0x2000000;

/// 0x007251c0's answer, 0 hated to 7 exalted (UnitReaction less one): from
/// 4 friendly (0x00514050, with the creature flag 0x4000000 besides), to 1
/// hostile (0x00514080).
inline constexpr int kReactionFriendly = 4;
inline constexpr int kReactionHostile = 1;

/// One side as the two relations read it.
struct Side {
    bool isPlayer = false;            ///< OBJECT_FIELD_TYPE 0x10
    uint32_t unitFlags = 0;           ///< UNIT_FIELD_FLAGS (+0xd4)
    uint32_t unitFlags2 = 0;          ///< UNIT_FIELD_FLAGS_2 (+0xd8)
    uint32_t pvpFlags = 0;            ///< UNIT_FIELD_BYTES_2 byte 1 (+0x1d1)
    uint32_t creatureTypeFlags = 0;   ///< its creature cache row's type flags (+0x964 +0xc), 0 for none
    uint32_t playerFlags = 0;         ///< a player's PLAYER_FLAGS (+0x1008 +8)
    /// A vehicle whose Vehicle.dbc row has Flags 0x20000000, none carrying it
    /// (+0xf5c +0xc, 0x0074c650 answering itself).
    bool vehicleOutOfReach = false;
};

/// The players behind both sides (0x00718b70: the unit, else its charmer or
/// creator, else theirs) and their duels - PLAYER_DUEL_ARBITER and
/// PLAYER_DUEL_TEAM (+0x1008 [0], [8]).
struct Controllers {
    bool found = false;  ///< both have one
    uint64_t casterArbiter = 0;
    uint64_t unitArbiter = 0;
    uint32_t casterTeam = 0;
    uint32_t unitTeam = 0;
};

/// 0x007293d0: whether the caster may help the unit. `reaction` is the
/// caster's of it (0x007251c0); `ignoreImmunity` the last argument, which
/// also lets it help a creature not flagged for it.
[[nodiscard]] constexpr bool mayAssist(const Side& caster, const Side& unit, int reaction, bool ignoreImmunity,
                                       const Controllers& c) {
    if ((unit.unitFlags & kFlagNotSelectable) != 0) return false;
    const bool casterPc = (caster.unitFlags & kFlagPlayerControlled) != 0;
    if (!ignoreImmunity) {
        if (casterPc && (unit.unitFlags & kFlagImmuneToPc) != 0) return false;
        if (!casterPc && (unit.unitFlags & kFlagImmuneToNpc) != 0) return false;
    }
    if (reaction < kReactionFriendly && (caster.creatureTypeFlags & 0x4000000u) == 0) return false;
    if ((unit.unitFlags & kFlagPlayerControlled) == 0) {
        // A creature: one not flagged for PvP wants CAN_ASSIST (0x1000,
        // 0x00715f90) or 0x4000000 (0x00715df0) from a player's side.
        if (casterPc && !ignoreImmunity && (unit.pvpFlags & pvp::kPvp) == 0)
            return (unit.creatureTypeFlags & (0x1000u | 0x4000000u)) != 0;
        return true;
    }
    // Not one in a duel, but by its partner on the same side.
    if (c.found && c.unitTeam != 0 && (c.casterArbiter != c.unitArbiter || c.casterTeam != c.unitTeam)) return false;
    if ((unit.pvpFlags & pvp::kFreeForAll) != 0 && (caster.pvpFlags & pvp::kFreeForAll) == 0) return false;
    if ((caster.pvpFlags & pvp::kSanctuary) != 0 && (unit.pvpFlags & pvp::kSanctuary) == 0 &&
        (unit.pvpFlags & pvp::kPvp) != 0)
        return false;
    return true;
}

/// 0x00729740: whether the caster may attack the unit. `reaction` is the
/// caster's of the unit, `reactionBack` the unit's of the caster.
[[nodiscard]] constexpr bool mayAttack(const Side& caster, const Side& unit, int reaction, int reactionBack,
                                       const Controllers& c) {
    if (caster.isPlayer && (caster.playerFlags & 0x80000u) != 0) return false;
    // A ghost, unless the caster sees them (creature flag 2).
    if (unit.isPlayer && (unit.playerFlags & 0x10u) != 0 && (caster.creatureTypeFlags & 0x2u) == 0) return false;
    if ((unit.unitFlags & (0x2u | 0x80u | 0x10000u | 0x100000u | kFlagNotSelectable)) != 0) return false;
    const bool casterPc = (caster.unitFlags & kFlagPlayerControlled) != 0;
    const bool unitPc = (unit.unitFlags & kFlagPlayerControlled) != 0;
    if (casterPc && (unit.unitFlags & kFlagImmuneToPc) != 0) return false;
    if (!casterPc && (unit.unitFlags & kFlagImmuneToNpc) != 0) return false;
    if (unitPc && (caster.unitFlags & kFlagImmuneToPc) != 0) return false;
    if (!unitPc && (caster.unitFlags & kFlagImmuneToNpc) != 0) return false;
    if (((caster.unitFlags2 | unit.unitFlags2) & 0x10000u) != 0 &&
        (caster.vehicleOutOfReach || unit.vehicleOutOfReach))
        return false;
    if (!casterPc && !unitPc) return reaction <= kReactionHostile || reactionBack <= kReactionHostile;
    const bool friendly = reaction >= kReactionFriendly || (caster.creatureTypeFlags & 0x4000000u) != 0;
    if (casterPc && unitPc) {
        if (friendly) return false;
        if (!c.found) return (caster.pvpFlags & pvp::kSanctuary) == 0 && (unit.pvpFlags & pvp::kSanctuary) == 0;
        if (c.casterTeam != 0 && c.unitTeam != 0 && c.casterArbiter == c.unitArbiter) return true;
        return pvpFlagsAllowAttack(caster.pvpFlags, unit.pvpFlags);
    }
    if (casterPc && (unit.pvpFlags & pvp::kSanctuary) != 0) return false;
    if (unitPc && (caster.pvpFlags & pvp::kSanctuary) != 0) return false;
    return !friendly;
}

/// What 0x0080bc80 reads of the spell.
struct Spell {
    uint32_t attrEx = 0;              ///< +0x14: 0x80000 not at its caster
    uint32_t attrEx2 = 0;             ///< +0x18: 0x1 at the dead
    uint32_t attrEx3 = 0;             ///< +0x1c: 0x10000000 a unit immune to players
    uint32_t attrEx5 = 0;             ///< +0x24: 0x200000 likewise
    uint32_t attrEx6 = 0;             ///< +0x28: 0x200 a charmed ally, 0x1000000 a unit flagged 0x10000
    uint32_t targetCreatureType = 0;  ///< +0x44, a mask by type less one
};

/// What it reads of the unit.
struct Unit {
    uint32_t type = 0;           ///< OBJECT_FIELD_TYPE
    bool isCaster = false;
    int32_t health = 0;          ///< UNIT_FIELD_HEALTH (+0x48)
    uint32_t unitFlags = 0;      ///< UNIT_FIELD_FLAGS
    uint32_t dynamicFlags = 0;   ///< UNIT_DYNAMIC_FLAGS (+0x124), 0x20 dead
    uint32_t petNumber = 0;      ///< UNIT_FIELD_PETNUMBER (+0x114)
    uint32_t creatureType = 0;   ///< 0x0071f300
    bool charmed = false;        ///< UNIT_FIELD_CHARMEDBY set (0x004f5f00)
};

/// The caster's relations to it, as 0x0080bc80 asks them.
struct Relations {
    bool inParty = false;       ///< 0x00718ca0
    bool inRaid = false;        ///< 0x00718d70
    bool mayAssist = false;     ///< 0x007293d0 (unit, 0)
    bool mayAssistEx6 = false;  ///< 0x007293d0 (unit, AttributesEx6 0x8)
    bool mayAttack = false;     ///< 0x00729740
};

/// 0x007ffc40, which 0x0080bc80 asks first of a unit for a spell with
/// AttributesEx5 0x800: a unit not of the side wanted stands for its own
/// target (UNIT_FIELD_TARGET). An enemy wanted, and no ally, party or raid
/// member (0x10c): a unit the caster may help (0x007293d0 with
/// AttributesEx6 0x8) gives its target if the caster may attack that, else
/// nothing. An ally, party or raid member wanted: a unit the caster may
/// attack gives its target if the caster may help that and it is a party
/// member where one is wanted, a raid member where one is, or an ally is
/// wanted; else nothing. Any other unit is itself.
enum class Retarget : uint8_t { None, Itself, ItsTarget };
[[nodiscard]] constexpr Retarget retarget(uint32_t required, const Relations& unit, bool hasTarget,
                                          const Relations& target) {
    if ((required & (kUnitAlly | kUnitParty | kUnitRaid)) != 0) {
        if (!unit.mayAttack) return Retarget::Itself;
        if (!hasTarget || !target.mayAssistEx6) return Retarget::None;
        if ((required & kUnitParty) != 0 && target.inParty) return Retarget::ItsTarget;
        if ((required & kUnitRaid) != 0 && target.inRaid) return Retarget::ItsTarget;
        return (required & kUnitAlly) != 0 ? Retarget::ItsTarget : Retarget::None;
    }
    if ((required & kUnitEnemy) == 0 || !unit.mayAssistEx6) return Retarget::Itself;
    return hasTarget && target.mayAttack ? Retarget::ItsTarget : Retarget::None;
}
/// What is still wanted after it (0x0080bc80): with an ally, party or raid
/// member wanted, no longer an enemy.
[[nodiscard]] constexpr uint32_t retargetRequired(uint32_t required) {
    return (required & (kUnitAlly | kUnitParty | kUnitRaid)) != 0 ? required & ~kUnitEnemy : required;
}

/// What taking it did: the unit fills some of what was needed (`required`
/// is what is left), with the cast's targets now `mask`.
struct Take {
    bool taken = false;
    uint32_t required = 0;
    uint32_t mask = 0;
};

/// 0x0080bc80: whether the unit fills what the spell still needs. A game
/// object fills a game object wanted; a unit, in order: a party member, a
/// raid member, an ally, an enemy, any unit not immune to players; a dead
/// player or pet for an ally's corpse, a dead player not to be helped for an
/// enemy's, a companion; and with AttributesEx6 0x200 a charmed unit not to
/// be attacked for an ally. Never the caster with AttributesEx 0x80000, a
/// unit flagged 0x10000 without AttributesEx6 0x1000000, nor one of a type
/// the spell does not name; a dead one only where the dead are wanted, a
/// living one not where only the dead are.
[[nodiscard]] constexpr Take takeUnit(const Spell& s, const Unit& u, const Relations& r, uint32_t required) {
    Take t{false, required, 0};
    if ((u.type & kTypeUnit) == 0) {
        if ((u.type & kTypeGameObject) != 0 && (required & (kGameObject | kGameObjectItem)) != 0) {
            t = {true, required & ~(kGameObject | kGameObjectItem), kGameObject};
        }
        return t;
    }
    if (u.isCaster && (s.attrEx & 0x80000u) != 0) return t;
    if ((u.unitFlags & 0x10000u) != 0 && (s.attrEx6 & 0x1000000u) == 0) return t;
    if (s.targetCreatureType != 0 &&
        (u.creatureType == 0 || (s.targetCreatureType & (1u << ((u.creatureType - 1) & 31))) == 0))
        return t;
    const bool dead = u.health < 1;
    if (dead && (u.dynamicFlags & 0x20u) == 0 && (required & (kCorpseAlly | kUnitDead | kCorpseEnemy)) == 0 &&
        (s.attrEx2 & 0x1u) == 0)
        return t;
    if (!dead && (required & kUnitDead) != 0) return t;
    // As a unit: the dead also fill a dead unit wanted.
    const auto asUnit = [&](uint32_t bit) {
        Take k{true, required & ~bit, kUnit};
        if (dead) k.required &= ~kUnitDead;
        return k;
    };
    if ((required & kUnitParty) != 0 && r.inParty && r.mayAssist) return asUnit(kUnitParty);
    if ((required & kUnitRaid) != 0 && r.inRaid && r.mayAssist) return asUnit(kUnitRaid);
    if ((required & kUnitAlly) != 0 && r.mayAssistEx6) return asUnit(kUnitAlly);
    if ((required & kUnitEnemy) != 0 && r.mayAttack) return asUnit(kUnitEnemy);
    if ((required & kUnit) != 0 &&
        ((u.unitFlags & kFlagImmuneToPc) == 0 || (s.attrEx3 & 0x10000000u) != 0 || (s.attrEx5 & 0x200000u) != 0)) {
        return {true, required & ~(dead ? (kCorpseAlly | kUnitDead | kCorpseEnemy | kUnit) : kUnit), kUnit};
    }
    if ((required & kCorpseAlly) != 0 && dead && ((u.type & kTypePlayer) != 0 || u.petNumber != 0)) {
        if (r.mayAssist) t = {true, required & ~kCorpseAlly, kUnit};
        return t;
    }
    if ((required & kCorpseEnemy) != 0 && dead && (u.type & kTypePlayer) != 0) {
        if (!r.mayAssist) t = {true, required & ~kCorpseEnemy, kUnit};
        return t;
    }
    if ((required & kUnitMinipet) != 0) {
        if (u.creatureType == 12) t = {true, required & ~kUnitMinipet, kUnitMinipet};
        return t;
    }
    if ((required & kUnitAlly) != 0 && (s.attrEx6 & 0x200u) != 0 && u.charmed && !r.mayAttack) {
        t = {true, required & ~kUnitAlly, kUnitAlly};
    }
    return t;
}

/// 0x0080bc80: a unit taken as one of these is held to the spell's reach
/// (0x00803850).
[[nodiscard]] constexpr bool reachChecked(uint32_t mask) { return (mask & 0x18202u) != 0; }

/// What 0x007ff480 reads for the reach.
struct RangeInput {
    uint32_t attr = 0;          ///< Attributes (+0x10): 0x404 is 0 to 100
    bool hasRow = false;        ///< a SpellRange row
    uint32_t rowFlags = 0;      ///< its Flags (+0x14): 1 melee, 2 ranged
    float rowMin = 0.0f;        ///< its minimum and maximum, hostile or
    float rowMax = 0.0f;        ///< friendly (0x00801650)
    float casterReach = 0.0f;   ///< UNIT_FIELD_COMBATREACH (+0xf0)
    bool hasUnit = false;       ///< aimed at a unit
    float unitReach = 0.0f;
    bool unitIsPlayer = false;  ///< OBJECT_FIELD_TYPE 0x10
    /// Each going forward, sideways or falling (0x100d) faster than twice
    /// its walk speed (0x00716fa0).
    bool casterMoving = false;
    bool unitMoving = false;
};
struct Range {
    float min = 0.0f;
    float max = 0.0f;
};

/// 0x007ff480: from the caster to the unit. A melee row reaches both
/// combat reaches and 4/3, at least 5; a ranged row's minimum starts there;
/// at a unit both reaches lengthen the maximum, and a minimum other than a
/// ranged row's. `maxMods` turns the row's maximum by the Range modifiers
/// (op 5). Both moving, a melee row or a player unit reach 8/3 more.
template <typename MaxMods>
[[nodiscard]] constexpr Range unitRange(const RangeInput& in, MaxMods&& maxMods) {
    if ((in.attr & 0x404u) != 0) return {0.0f, 100.0f};
    Range r{};
    float extra = 0.0f;
    if (in.hasRow) {
        const float near = (in.hasUnit ? in.unitReach : in.casterReach) + in.casterReach + 4.0f / 3.0f;
        if ((in.rowFlags & 1u) == 0) {
            r.min = (in.rowFlags & 2u) == 0 ? in.rowMin : (near <= 5.0f ? 5.0f : near) + in.rowMin;
            r.max = in.rowMax;
            if (in.hasUnit) {
                extra = in.casterReach + in.unitReach;
                if (r.min != 0.0f && (in.rowFlags & 2u) == 0) r.min += extra;
            }
        } else {
            extra = near <= 5.0f ? 5.0f : near;
        }
        if (in.hasUnit && in.casterMoving && in.unitMoving && ((in.rowFlags & 1u) != 0 || in.unitIsPlayer))
            extra += 8.0f / 3.0f;
    }
    r.max = maxMods(r.max) + extra;
    return r;
}

/// 0x00803850 -> 0x0080bc80's refusal of a unit outside the reach: closer
/// than the minimum SPELL_FAILED_TOO_CLOSE, beyond the maximum
/// SPELL_FAILED_OUT_OF_RANGE; 0 within it.
inline constexpr uint8_t kFailedOutOfRange = 0x61;
inline constexpr uint8_t kFailedTooClose = 0x80;
[[nodiscard]] constexpr uint8_t reachFailure(float distSq, const Range& r) {
    if (r.min * r.min > distSq) return kFailedTooClose;
    if (r.max * r.max < distSq) return kFailedOutOfRange;
    return 0;
}

/// 0x0080cce0 when no unit filled the spell (0x0080c790 answering none):
/// Attributes 0x200's main hand, for a caster other than the player or one
/// with nothing there, SPELL_FAILED_MAINHAND_EMPTY (0x32);
/// else the cursor holds the spell for a unit to be clicked - unless an enemy
/// is wanted and AttributesEx5 0x800 does not let an ally or party member
/// stand for it: SPELL_FAILED_BAD_TARGETS (0xc) with a target, else
/// SPELL_FAILED_BAD_IMPLICIT_TARGETS (0xb). 0 for the cursor.
inline constexpr uint8_t kFailedMainhand = 0x32;
inline constexpr uint8_t kFailedBadTargets = 0xc;
inline constexpr uint8_t kFailedNoTarget = 0xb;
[[nodiscard]] constexpr uint8_t noUnitFailure(uint32_t attr, uint32_t attrEx5, uint32_t required, bool casterIsPlayer,
                                              bool playerHasMainhand, bool playerHasTarget) {
    if ((attr & 0x200u) != 0 && (!casterIsPlayer || !playerHasMainhand)) return kFailedMainhand;
    if ((required & kUnitEnemy) == 0 || ((attrEx5 & 0x800u) != 0 && (required & 0x10cu) != 0)) return 0;
    return playerHasTarget ? kFailedBadTargets : kFailedNoTarget;
}

/// 0x0071b0c0: whether the player would get down from its mount for it -
/// not on a taxi (UNIT_FIELD_FLAGS 0x100000), autoDismount on, and
/// autoDismountFlying as well while flying (MOVEMENTFLAG 0x2000000).
[[nodiscard]] constexpr bool wouldDismount(uint32_t unitFlags, bool flying, bool autoDismount,
                                           bool autoDismountFlying) {
    return (unitFlags & 0x100000u) == 0 && autoDismount && (!flying || autoDismountFlying);
}

/// 0x00729a70: 0x00729740 asked only of a caster able to fight now - alive,
/// and not mounted (+0x9c0) or one that would get down for it - at a unit
/// with health, or flagged dead (UNIT_DYNFLAG 0x20).
[[nodiscard]] constexpr bool mayAttackNow(bool casterDead, bool casterMounted, bool wouldDismount,
                                          uint32_t unitHealth, uint32_t unitDynamicFlags, bool mayAttack) {
    if (casterDead || (casterMounted && !wouldDismount)) return false;
    if (unitHealth == 0 && (unitDynamicFlags & 0x20u) == 0) return false;
    return mayAttack;
}

/// 0x0071f5c0 between the player and another player: in a duel with each
/// other - the same PLAYER_DUEL_ARBITER, both on a PLAYER_DUEL_TEAM, not the
/// same one.
[[nodiscard]] constexpr bool duelOpponents(uint64_t arbiter, uint32_t team, uint64_t unitArbiter, uint32_t unitTeam) {
    return team != 0 && unitTeam != 0 && arbiter == unitArbiter && team != unitTeam;
}

/// A unit's end (CGUnit_C, 0x00735xxx) -> 0x00524350: a unit leaving view is
/// forgotten - no longer the target, last target, last enemy or friend, nor
/// the focus - unless it is one of the player's party or raid (0x00512a30)
/// not in a duel with the player (0x0071f5c0).
[[nodiscard]] constexpr bool forgottenOnLeaving(bool groupMember, bool duelOpponent) {
    return !groupMember || duelOpponent;
}

/// 0x0073c330: a unit whose UNIT_FIELD_FLAGS turn not selectable (0x2000000)
/// is forgotten as well (0x00524350), in the group or not.
[[nodiscard]] constexpr bool forgottenOnFlags(uint32_t oldFlags, uint32_t newFlags) {
    return ((oldFlags ^ newFlags) & newFlags & kFlagNotSelectable) != 0;
}

/// 0x00524bf0: the last target once the target goes from `old` to `taken` -
/// the old one, none included, when a unit is taken; the one let go when the
/// target is cleared (none to let go, nothing changes).
[[nodiscard]] constexpr uint64_t lastTargetAfter(uint64_t old, uint64_t taken, uint64_t last) {
    return taken != 0 || old != 0 ? old : last;
}

/// 0x00809610, before 0x0080cce0 picks the spell's units: what the unit cast
/// at may be taken as - Spell.dbc Targets (+0x40, its low half) with each
/// effect's ImplicitTargetA - and whether TARGET_UNIT_PET (5) puts the
/// player's pet in its place (where no plain unit is wanted).
struct ReachKinds {
    uint32_t mask = 0;
    bool pet = false;
};
[[nodiscard]] constexpr ReachKinds reachKinds(uint32_t targets, const uint32_t (&implicitA)[3]) {
    uint32_t mask = targets & 0xFFFFu;
    bool pet = false;
    for (const uint32_t a : implicitA) {
        switch (a) {
            case 1: mask &= ~kUnitDead; break;
            case 5: pet = true; mask &= ~kCorpseAlly; break;
            case 6: case 53: mask |= kUnitEnemy; break;
            case 21: case 45: mask |= kUnitAlly; break;
            case 23: mask |= kGameObject; break;
            case 25: case 63: case 64: case 65: case 66: case 67: case 68: case 69: case 70: case 71: case 74: case 75:
                mask |= kUnit;
                break;
            case 26: mask |= kGameObjectItem; break;
            case 35: mask |= kUnitParty; break;
            case 57: case 61: mask |= kUnitRaid; break;
            default: break;
        }
    }
    pet = pet && (mask & kUnit) == 0;
    if (pet) mask |= kUnit;
    return {mask, pet};
}

}  // namespace wowee::game::unit_target
