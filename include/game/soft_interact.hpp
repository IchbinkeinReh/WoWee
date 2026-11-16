#pragma once

// The object the client makes the mouseover while its cursor is turned off
// (0x005220b0, through the object walk 0x004d4b30 calling 0x00519e80 for
// each). It runs each frame the input control's cursor flag (+0x58 bit 0) is
// clear, which a TOGGLEMOUSE binding brings about (0x00562d60 clears +0x58
// 0x2 when one is bound); with the mouse in play the flag is set and the
// pointer decides the mouseover instead.
//
// The search looks along the facing of the unit the player moves (the mover,
// else the player): the object most nearly ahead wins, a tie the nearer, and
// nothing behind (the best dot product starts at 0, the best distance at
// 10000 squared yards, 0x00ac8a74). Each kind passes a gate of its own first.

#include <cmath>
#include <cstdint>
#include <vector>

namespace wowee::game::soft_interact {

enum class Kind : uint8_t {
    Unit,        ///< type mask 0x8
    GameObject,  ///< type mask 0x20
    Corpse,      ///< type mask 0x80
    Other,       ///< anything else: never taken here
};

/// 0x006d5a60: whether the player may loot a unit or corpse now.
struct LootCheck {
    /// 0x00721f50: the player alive, the active mover, and autoInteract on -
    /// then any distance will do (the player is walked there).
    bool anyDistance = false;
    /// The reach squared: for a unit max(5, both combat reaches + 4/3)
    /// (0x004f5f40), anything else 5.
    float reachSq = 25.0f;
    float distSq = 0.0f;
    /// 0x00721ca0 (a form that is not a stance) with that form's
    /// SpellShapeshiftForm Flags 0x100.
    bool formForbids = false;
    bool looting = false;        ///< the player's loot object (+0x18e0) set
    bool deadOrGhost = false;    ///< vtable +0x128
    bool falling = false;        ///< movement flags 0x1000 (+0xd8 +0x44)
    bool stunned = false;        ///< UNIT_FLAG 0x40000
    bool clientControl = true;   ///< +0xa30 0x400 (0x00714ac0)
};
[[nodiscard]] inline bool playerMayLoot(const LootCheck& c) {
    if (c.formForbids) return false;
    if (!c.anyDistance && c.distSq > c.reachSq) return false;
    return !c.looting && !c.deadOrGhost && !c.falling && !c.stunned && c.clientControl;
}
/// The reach 0x006d5a60 loots a unit within (0x004f5f40): its and the
/// player's combat reach and four thirds, at least five yards.
[[nodiscard]] inline float lootReach(float unitReach, float playerReach) {
    const float r = unitReach + playerReach + 4.0f / 3.0f;
    return r < 5.0f ? 5.0f : r;
}

struct Candidate {
    uint64_t guid = 0;
    /// From the mover to it, in the world's (server) axes.
    float dx = 0.0f, dy = 0.0f, dz = 0.0f;
    Kind kind = Kind::Other;
    bool isMover = false;  ///< the mover itself, passed over

    // A unit.
    bool serviceOffered = false;  ///< 0x00729620
    uint32_t npcFlags = 0;        ///< UNIT_NPC_FLAGS (+0x130)
    float combatReach = 0.0f;     ///< UNIT_FIELD_COMBATREACH (+0xf0)
    bool lootableDead = false;    ///< 0x00717b60: dead, LOOTABLE (dynamic flags 0x1)
    bool playerMayLoot = false;   ///< 0x006d5a60 (playerMayLoot), a unit's or a corpse's
    bool playersLootTarget = false;  ///< the player's loot object (+0x18e0) is it
    /// UNIT_FLAG_SKINNABLE (0x4000000) with a gather spell the player knows
    /// for it (0x0053bce0: a creature's SKINNING spell by its kind, a
    /// player's SKIN_PLAYER_CORPSE), unless it is a player the player may
    /// help (0x007293d0 with 0); for a corpse, CORPSE_FLAG 0x20 with the
    /// SKIN_PLAYER_CORPSE spell: that spell's reach (0x00802c30); negative
    /// for none.
    float gatherReach = -1.0f;
    bool attackable = false;      ///< 0x00729a70

    // A game object: its type handler's +0x18 and +0x1c both say yes
    // (0x0070ba00, 0x0070ba10).
    bool goUsable = false;

    // A corpse: CORPSE_FIELD_DYNAMIC_FLAGS 0x1, lootable (0x007058f0); and
    // whether the player regards its race's faction as friendly (0x0071f890:
    // 0x00718c20 above 3), which keeps it from being skinned.
    bool corpseLootable = false;
    bool corpseFriendly = false;
};

struct Asker {
    float facing = 0.0f;          ///< the mover's, server radians (vfunc +0x34)
    bool playerIsMover = true;    ///< game objects only for the player itself
    bool playerPacified = false;  ///< the player's UNIT_FLAG 0x20000
    bool hasControl = true;       ///< 0x00bcfb8c
};

/// The yards squared an enemy is taken at (0x00519e80: 109.2025, 10.45 yd).
inline constexpr float kAttackReachSq = 109.2025f;

/// 0x00519e80 over every candidate, in the walk's order: the guid it leaves
/// in the search (+0x18), 0 for none.
inline uint64_t pick(const Asker& a, const std::vector<Candidate>& objects) {
    const float fx = std::cos(a.facing), fy = std::sin(a.facing);
    float bestDot = 0.0f;
    float bestDistSq = 10000.0f;
    uint64_t best = 0;
    for (const Candidate& c : objects) {
        if (c.isMover || c.guid == 0) continue;
        const float distSq = c.dx * c.dx + c.dy * c.dy + c.dz * c.dz;
        // 0x004c3600: the way to it, as a unit vector.
        float nx = 0.0f, ny = 0.0f;
        if (distSq > 0.0f) {
            const float inv = 1.0f / std::sqrt(distSq);
            nx = c.dx * inv;
            ny = c.dy * inv;
        }
        const float dot = fx * nx + fy * ny;
        if (dot < bestDot) continue;
        // 0x00482890: as far ahead as the best, within 2^-22, and further.
        if (std::fabs(dot - bestDot) < 2.3841858e-07f && bestDistSq < distSq) continue;
        bool take = false;
        switch (c.kind) {
            case Kind::GameObject:
                take = a.playerIsMover && c.goUsable;
                break;
            case Kind::Unit: {
                float reachSq = -1.0f;
                if (c.serviceOffered && c.npcFlags != 0) {
                    const float r = c.combatReach + 4.0f;
                    reachSq = r * r;
                } else if (c.lootableDead) {
                    // Lootable by the player, or what it loots now: no reach.
                    take = c.playerMayLoot || c.playersLootTarget;
                    break;
                } else if (c.gatherReach >= 0.0f) {
                    reachSq = c.gatherReach * c.gatherReach;
                } else if (c.attackable && !a.playerPacified && a.hasControl) {
                    reachSq = kAttackReachSq;
                }
                take = reachSq >= 0.0f && !(reachSq < distSq);
                break;
            }
            case Kind::Corpse:
                // Only for the player's own moving.
                if (!a.playerIsMover) break;
                if (c.corpseLootable) {
                    take = c.playerMayLoot || c.playersLootTarget;
                } else if (c.gatherReach >= 0.0f && !c.corpseFriendly) {
                    take = !(c.gatherReach * c.gatherReach < distSq);
                }
                break;
            case Kind::Other:
                break;
        }
        if (!take) continue;
        bestDot = dot;
        bestDistSq = distSq;
        best = c.guid;
    }
    return best;
}

}  // namespace wowee::game::soft_interact
