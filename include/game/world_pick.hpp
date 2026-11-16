#pragma once

/// What the pointer may find in the world (CGWorldFrame, WorldFrame.cpp):
/// the pick flags the world frame asks for (0x004f7650), and which objects
/// those flags let the scene ray pick (0x004f9930) take - the filter its
/// model search (0x004f9550) runs every candidate through (0x004f7530, a
/// unit's 0x004f7350, a corpse's 0x007ffea0). Arithmetic only: the objects
/// and the spell held are the caller's.
///
/// The ray pick itself, 0x004f9930, runs in this order:
///  1. the world (CWorld::Intersect, 0x0077f310, flags 0x1000124 - terrain
///     and buildings, with 0x20000 the liquid surface besides when the flags
///     ask for liquid) from the near plane to the far plane, shortening the
///     ray to its first hit;
///  2. with any object flag (0x7c), the nearest model of an object these
///     filters take along what is left of the ray (0x004f9550);
///  3. with a ground flag (0x3), the world again (flags 0x100151) for the
///     place on the ground;
/// and answers 2 for an object, 3 for a world hit that belongs to an object
/// (a transport, an object's own collision), 1 for the ground where the
/// flags asked for it, else 0. The world hides every object behind it.

#include <cstdint>

namespace wowee::game::world_pick {

/// The flags 0x004f7650 builds and 0x004f9930 takes.
inline constexpr uint32_t kGround = 0x1;            ///< the ground's place (with kLiquid both bits)
inline constexpr uint32_t kLiquid = 0x2;            ///< ...the liquid surface counting as ground
inline constexpr uint32_t kGameObjects = 0x4;       ///< OBJECT_FIELD_TYPE 0x21
inline constexpr uint32_t kUnits = 0x8;             ///< 0x9
inline constexpr uint32_t kPlayers = 0x10;          ///< 0x19
inline constexpr uint32_t kSelf = 0x20;             ///< the active player and what it possesses
inline constexpr uint32_t kCorpses = 0x40;          ///< 0x81
inline constexpr uint32_t kObjectMask = 0x7c;       ///< any of the above objects
inline constexpr uint32_t kParty = 0x10000;         ///< 0x00718ca0
inline constexpr uint32_t kRaid = 0x20000;          ///< 0x00718d70
inline constexpr uint32_t kFriendly = 0x40000;      ///< 0x007293d0 (player, unit, 0)
inline constexpr uint32_t kHostile = 0x80000;       ///< 0x00729740
inline constexpr uint32_t kRelationMask = 0xf0000;
inline constexpr uint32_t kAlive = 0x100000;        ///< UNIT_FIELD_HEALTH above 0
inline constexpr uint32_t kDead = 0x200000;
inline constexpr uint32_t kCompanion = 0x400000;    ///< creature type 12, the non-combat pet
inline constexpr uint32_t kCharmedEnemy = 0x800000; ///< a charmed unit the player may attack, for an ally

/// SpellCastTargetFlags of the spell the cursor holds (0x00d3f4e0) the
/// flags are built from.
inline constexpr uint32_t kTargetUnitAny = 0x1878e;  ///< 0x007fd650
inline constexpr uint32_t kTargetSelfOk = 0x850e;    ///< 0x00801880
inline constexpr uint32_t kTargetParty = 0x408;      ///< 0x007fd670
inline constexpr uint32_t kTargetRaid = 0x404;       ///< 0x007fd690
inline constexpr uint32_t kTargetAlly = 0x500;       ///< 0x007fd6b0
inline constexpr uint32_t kTargetEnemy = 0x480;      ///< 0x007fd6d0
inline constexpr uint32_t kTargetDead = 0x8600;      ///< 0x007fd6f0, 0x008018f0
inline constexpr uint32_t kTargetLocation = 0x60;    ///< 0x007fd750
inline constexpr uint32_t kTargetGameObject = 0x4800;///< 0x007fd730
inline constexpr uint32_t kTargetMinipet = 0x10000;  ///< 0x007fd780
inline constexpr uint32_t kTargetCorpseAlly = 0x8000;///< 0x007fd710
inline constexpr uint32_t kTargetCorpseEnemy = 0x200;///< 0x007fd720

/// Movement flags 0x004f7650 reads of the active mover (+0xd8 +0x44).
inline constexpr uint32_t kMoveSwimming = 0x200000;
inline constexpr uint32_t kMoveWaterWalking = 0x10000000;

/// UNIT_FIELD_FLAGS the filter reads.
inline constexpr uint32_t kUnitFlagImmuneToPc = 0x100;
inline constexpr uint32_t kUnitFlagPossessed = 0x01000000;  ///< +0xd7 bit 0

/// Creature type 12, the non-combat pet (0x0071f300).
inline constexpr uint32_t kCreatureTypeCompanion = 12;

/// What 0x004f7650 reads.
struct FlagsInput {
    /// A spell the cursor holds (0x00d3f4e4, 0x007fd620), and what it still
    /// wants (0x00d3f4e0).
    bool spellHeld = false;
    uint32_t targetMask = 0;
    uint32_t attrEx = 0;   ///< 0x80000: never the caster (0x00801880)
    uint32_t attrEx2 = 0;  ///< 0x1: the dead (0x008018f0)
    uint32_t attrEx6 = 0;  ///< 0x200: a charmed enemy for an ally (0x00801960)
    /// The active player is in the world.
    bool activePlayer = false;
    /// 0x00721f50 of the active mover: alive, and autoInteract (click to
    /// move) on; with no click-to-move under way (0x00bd0748 == 0).
    bool moverClickToMove = false;
    bool clickToMoveIdle = true;
    uint32_t moverMoveFlags = 0;
};

/// 0x004f7650: with no spell held, every object kind once there is an active
/// player (0x5c), and the ground with click to move (the liquid surface too
/// while walking on water, not swimming); with one, only what the spell
/// wants. The two corpse wants count either way.
[[nodiscard]] constexpr uint32_t pickFlags(const FlagsInput& in) {
    uint32_t f = 0;
    const uint32_t m = in.targetMask;
    if (!in.spellHeld) {
        if (in.moverClickToMove && in.clickToMoveIdle) {
            f = kGround;
            if ((in.moverMoveFlags & kMoveWaterWalking) != 0 && (in.moverMoveFlags & kMoveSwimming) == 0)
                f = kGround | kLiquid;
        }
        if (in.activePlayer) f |= kGameObjects | kUnits | kPlayers | kCorpses;
    } else {
        if ((m & kTargetLocation) != 0) f = kGround | kLiquid;
        if ((m & kTargetGameObject) != 0) f |= kGameObjects;
        if ((m & kTargetUnitAny) != 0) {
            const bool selfOk = (m & kTargetSelfOk) != 0 && (in.attrEx & 0x80000u) == 0;
            f |= selfOk ? (kUnits | kPlayers | kSelf) : (kUnits | kPlayers);
            if ((m & kTargetParty) != 0) f |= kParty;
            if ((m & kTargetAlly) != 0) f |= kFriendly;
            if ((m & kTargetEnemy) != 0) f |= kHostile;
            if ((m & kTargetRaid) != 0) f |= kRaid;
            if ((m & kTargetDead) != 0 || (in.attrEx2 & 0x1u) != 0) f |= kDead | kCorpses;
            if ((m & kTargetDead) == 0) f |= kAlive;
            if ((m & kTargetMinipet) != 0) f |= kCompanion;
            if ((in.attrEx6 & 0x200u) != 0) f |= kCharmedEnemy;
        }
    }
    if ((m & kTargetCorpseAlly) != 0) f |= kFriendly | kCorpses;
    if ((m & kTargetCorpseEnemy) != 0) f |= kHostile | kCorpses;
    return f;
}

/// What 0x004f7350 reads of a unit or player. The relations are the active
/// player's to it.
struct UnitInput {
    bool isActivePlayer = false;
    /// 0x004f7250: UNIT_FLAG_POSSESSED, and its charmer - else its creator -
    /// the active player.
    bool possessedByActivePlayer = false;
    bool inParty = false;     ///< 0x00718ca0
    bool inRaid = false;      ///< 0x00718d70
    bool mayAssist = false;   ///< 0x007293d0 (unit, 0)
    bool mayAttack = false;   ///< 0x00729740
    bool charmed = false;     ///< UNIT_FIELD_CHARMEDBY set
    int32_t health = 0;       ///< UNIT_FIELD_HEALTH
    uint32_t creatureType = 0;
    uint32_t unitFlags = 0;
    /// The spell the cursor holds, if any: AttributesEx3 0x10000000 or
    /// AttributesEx5 0x200000 take a unit immune to players.
    bool spellHeld = false;
    uint32_t attrEx3 = 0;
    uint32_t attrEx5 = 0;
};

/// 0x004f7350: never the player itself or the unit it possesses but with
/// kSelf; with a relation wanted, one of them (an ally wanted and the unit
/// not one: only a charmed enemy, where kCharmedEnemy lets it); alive or
/// dead as wanted; a companion wherever one is wanted; and while a spell is
/// held, no unit immune to players unless the spell says otherwise.
[[nodiscard]] constexpr bool unitPickable(uint32_t f, const UnitInput& u) {
    if ((f & kSelf) == 0) {
        if (u.isActivePlayer) return false;
        if (u.possessedByActivePlayer) return false;
    }
    if ((f & kRelationMask) != 0) {
        if ((f & kFriendly) != 0 && !u.mayAssist) {
            if ((f & kCharmedEnemy) == 0 || !u.charmed) return false;
            return u.mayAttack;
        }
        const bool related = ((f & kParty) != 0 && u.inParty) || ((f & kRaid) != 0 && u.inRaid) ||
                             ((f & kFriendly) != 0 && u.mayAssist) || ((f & kHostile) != 0 && u.mayAttack);
        if (!related) return false;
    }
    if ((f & (kAlive | kDead)) != 0) {
        if ((f & (u.health < 1 ? kDead : kAlive)) == 0) return false;
    }
    if ((f & kCompanion) != 0 && u.creatureType == kCreatureTypeCompanion) return true;
    if (!u.spellHeld) return true;
    if ((u.unitFlags & kUnitFlagImmuneToPc) == 0) return true;
    return (u.attrEx3 & 0x10000000u) != 0 || (u.attrEx5 & 0x200000u) != 0;
}

/// 0x004f7530 for a game object: wanted, and with a spell held one the spell
/// may open (0x007fff20: it wants an object, 0x0070f010 its lock).
[[nodiscard]] constexpr bool gameObjectPickable(uint32_t f, bool spellHeld, bool spellTakesObject) {
    return (f & kGameObjects) != 0 && (!spellHeld || spellTakesObject);
}

/// What 0x007ffea0 reads of a corpse.
struct CorpseInput {
    bool spellHeld = false;
    uint32_t targetMask = 0;
    bool bones = false;     ///< CORPSE_FIELD_FLAGS 0x1
    bool friendly = false;  ///< 0x0071f890: the player's reaction to it above neutral
};

/// 0x004f7530 for a corpse (0x004f58e0): wanted, and with a spell held one it
/// takes (0x007ffea0) - an ally's for an ally's corpse, else an enemy's for
/// an enemy's; never bones.
[[nodiscard]] constexpr bool corpsePickable(uint32_t f, const CorpseInput& c) {
    if ((f & kCorpses) == 0) return false;
    if (!c.spellHeld) return true;
    if (c.bones) return false;
    if ((c.targetMask & kTargetCorpseAlly) != 0) return c.friendly;
    return (c.targetMask & kTargetCorpseEnemy) != 0 && !c.friendly;
}

/// 0x004f9930's answer.
enum class Hit : uint8_t {
    None = 0,
    Ground = 1,       ///< the ground, where the flags asked for it
    Object = 2,       ///< an object's model (0x004f9550)
    WorldObject = 3,  ///< the world, where what was hit belongs to an object
};

/// 0x004f9930's choice between what it met: an object's model where one was
/// met in front of the world; else the world where it belongs to an object;
/// else the ground where the flags asked for it.
[[nodiscard]] constexpr Hit resolveHit(uint32_t f, bool objectHit, bool worldHit, bool worldHitIsObject) {
    if (objectHit) return Hit::Object;
    if (!worldHit) return Hit::None;
    if (worldHitIsObject) return Hit::WorldObject;
    return (f & (kGround | kLiquid)) != 0 ? Hit::Ground : Hit::None;
}

}  // namespace wowee::game::world_pick
