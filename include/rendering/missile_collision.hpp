#pragma once

/// A siege missile meeting a unit on its way (UnitMissileTrajectory_C.cpp).
///
/// A missile on an aimed arc (flag 0x10000) whose caster the client moves
/// (flag 0x200000) and whose SpellMissile row has any of Flags 0x3c is given
/// a collision object when it is launched (0x00700880 -> 0x006fc920 into
/// missile +0x148): its path seen from above (0x006fcb60) and the units that
/// could step into it before it lands, at most fifteen (0x006fcaa0,
/// 0x006fc4a0, 0x006fc7d0). Every frame the step it flies is then traced
/// against a sphere about each of those units where they stand now
/// (0x006fc9b0 -> 0x006fc0a0, 0x006fbff0, 0x006fc360); the first it passes
/// through stops it there, and the client tells the server where with
/// CMSG_UPDATE_PROJECTILE_POSITION (0x007015d0 -> 0x00703640).
///
/// What is here is that arithmetic, kept apart from the world so it can be
/// tested. Points are the wire's (the renderer's) x, y, z.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::rendering::missile_collision {

/// SpellMissile Flags that give a missile a collision object (0x00700880),
/// and which units each lets it meet (0x006fc4a0): players it may attack,
/// players it may not, creatures it may attack, creatures it may not.
inline constexpr uint32_t kFlagsCollide = 0x3c;
inline constexpr uint32_t kFlagHostilePlayers = 0x04;
inline constexpr uint32_t kFlagFriendlyPlayers = 0x08;
inline constexpr uint32_t kFlagHostileCreatures = 0x10;
inline constexpr uint32_t kFlagFriendlyCreatures = 0x20;
/// The creature cache's type flag without which no creature is met
/// (CreatureTemplate type_flags 0x80000, 0x006fc4a0 reading +0x964 +0xc).
inline constexpr uint32_t kTypeFlagCollidesWithMissiles = 0x80000;
/// How many units a missile keeps (0x006fcb60), and how many are looked at
/// before the search stops (0x006fcaa0).
inline constexpr size_t kMaxUnits = 15;
inline constexpr size_t kMaxSearched = 128;

/// The cosine 0x006fcb60 takes of the pitch: the pitch over pi split into a
/// whole and a share (0x005fe800, the whole truncated, one less for a
/// pitch not above 0), the share's smooth curve 1 - (6 - 4f) f^2, negated
/// for an odd whole.
[[nodiscard]] inline float smoothCos(float pitch) {
    const float turns = pitch * 0.31830987f;
    int32_t whole = static_cast<int32_t>(turns);
    if (!(turns > 0.0f)) whole -= 1;
    const float f = turns - static_cast<float>(whole);
    float c = 1.0f - (6.0f - f * 4.0f) * f * f;
    if (static_cast<uint32_t>(whole) & 1u) c = -c;
    return c;
}

/// The missile's path seen from above (the collision object's +0x44..+0x6c,
/// 0x006fcb60).
struct Path {
    glm::vec2 start{0.0f};     ///< +0x44
    glm::vec2 end{0.0f};       ///< +0x4c
    glm::vec2 dir{1.0f, 0.0f}; ///< +0x54, from start to end
    glm::vec2 side{0.0f, 1.0f};///< +0x5c, dir turned a quarter left
    float length = 1.0f;       ///< +0x64
    float time = 0.0f;         ///< +0x68, length over the speed
    float speed = 0.0f;        ///< +0x6c, the speed across the ground
    float rowRadius = 0.0f;    ///< the row's CollisionRadius (+0x70 -> +0x38)
};

/// 0x006fcb60: from where the missile was let go to where it lands, at the
/// arc's pitch and launch speed. One that goes straight up or down is given
/// a yard's run east, a hundredth of a yard short of its start, that it
/// takes the whole flight to cover.
[[nodiscard]] inline Path makePath(const glm::vec3& start, const glm::vec3& end, float pitch, float speed,
                                   float rowRadius) {
    Path p;
    p.rowRadius = rowRadius;
    p.start = glm::vec2(start);
    p.end = glm::vec2(end);
    glm::vec2 d = p.end - p.start;
    if (std::abs(d.x) >= 0.001f || std::abs(d.y) >= 0.001f) {
        p.length = std::sqrt(d.x * d.x + d.y * d.y);
        const float inv = 1.0f / std::sqrt(d.y * d.y + d.x * d.x);
        p.dir = glm::vec2(d.x * inv, inv * d.y);
        p.speed = smoothCos(pitch) * speed;
        if (p.speed < 0.0001f) p.speed = 0.0001f;
    } else {
        float f = 1.0f;
        if (speed > 0.0001f) f = glm::length(end - start) / speed;
        p.speed = 0.01f / f;
        p.start.x -= 0.01f;
        p.dir = glm::vec2(1.0f, 0.0f);
        p.length = 1.0f;
    }
    p.side = glm::vec2(-p.dir.y, p.dir.x);
    p.time = p.length / p.speed;
    return p;
}

/// What 0x006fbff0 reads of a unit to put a sphere about it: its place, its
/// scale (vtable +0x7c), and its CreatureModelData row where it has one
/// (0x00717a20): CollisionHeight (+0x3c), MissileCollisionRadius (+0x64),
/// MissileCollisionPush (+0x68) and MissileCollisionRaise (+0x6c).
struct UnitShape {
    glm::vec3 position{0.0f};
    float scale = 1.0f;
    bool hasModelData = false;
    float collisionHeight = 0.0f;
    float missileRadius = 0.0f;
    float missilePush = 0.0f;
    float missileRaise = 0.0f;
};

struct Sphere {
    glm::vec3 centre{0.0f};
    float radius = 0.0f;
};

/// 0x006fbff0 with 0x006fc0a0's addition: the radius is MissileCollisionRadius
/// times the scale, or half the CollisionHeight where that is not above 0;
/// the centre is pushed back against the missile's way by Push times the
/// scale, raised by Raise times the scale and then by the radius. A unit
/// without a model row is a yard's sphere a yard up. The row's
/// CollisionRadius is added to every radius.
[[nodiscard]] inline Sphere unitSphere(const UnitShape& u, const glm::vec2& dir, float rowRadius) {
    Sphere s;
    s.centre = u.position;
    if (u.hasModelData) {
        s.radius = u.missileRadius <= 0.0f ? u.collisionHeight * 0.5f : u.scale * u.missileRadius;
        const float push = u.missilePush * u.scale;
        s.centre.x -= dir.x * push;
        s.centre.y -= push * dir.y;
        s.centre.z += u.scale * u.missileRaise;
        s.centre.z += s.radius;
    } else {
        s.radius = 1.0f;
        s.centre.z += s.radius;
    }
    s.radius += rowRadius;
    return s;
}

/// What 0x006fc4a0 asks of a unit before it measures it.
struct UnitKind {
    bool isPlayer = false;
    /// A creature whose cache has type flag 0x80000.
    bool collidesWithMissiles = false;
    /// Whether the active mover may attack it (0x00729740); unasked where
    /// the row's flags meet both kinds.
    bool moverMayAttack = false;
    /// There is an active mover to ask (0x004d4db0 of DAT_00ca1238).
    bool hasMover = true;
};

/// 0x006fc4a0's first half: the row's flags 0x4/0x8 for a player, 0x10/0x20
/// for a creature with type flag 0x80000; with only one of a pair, the
/// unit must be one the mover may attack (0x4, 0x10) or may not (0x8,
/// 0x20).
[[nodiscard]] inline bool rowMeets(uint32_t rowFlags, const UnitKind& k) {
    uint32_t f;
    uint32_t shift;
    if (k.isPlayer) {
        f = rowFlags & 0xc;
        shift = 2;
    } else {
        f = rowFlags & 0x30;
        if (f != 0 && !k.collidesWithMissiles) return false;
        shift = 4;
    }
    if (f == 0) return false;
    if (f == (k.isPlayer ? 0xcu : 0x30u)) return true;
    if (!k.hasMover) return false;
    const bool wantsHostile = ((f >> shift) & 1u) != 0;
    return wantsHostile == k.moverMayAttack;
}

/// 0x006fc4a0's measure of a unit that `maxSpeed` (the most of its run,
/// swim and flight speeds, CMovement +0x94/+0x9c/+0xa4) carries, -1 where
/// it cannot come near the path in the flight: how far its sphere, grown by
/// how far it could run, reaches into the path at the point the path
/// passes it closest, and at the end.
[[nodiscard]] inline float unitScore(const Path& p, const Sphere& s, float maxSpeed) {
    const float r = s.radius;
    if (!(r >= 0.0001f)) return -1.0f;
    const glm::vec2 c(s.centre);
    const float ex = p.end.x - c.x;
    const float ey = p.end.y - c.y;
    float closest;
    if (p.speed <= maxSpeed) {
        closest = -1.0f;
    } else {
        const float off = std::abs(p.side.x * ex + p.side.y * ey);
        const float along =
            ((p.side.y * off + c.y) - p.start.y) * p.dir.y + p.dir.x * ((p.side.x * off + c.x) - p.start.x);
        if (along >= 0.0001f) {
            if (p.length <= along) {
                closest = -1.0f;
            } else {
                const float reach = (along / p.length) * p.time * maxSpeed + r;
                closest = (reach * reach - off * off) / along;
            }
        } else {
            const float sx = p.start.x - c.x;
            const float sy = p.start.y - c.y;
            closest = (r * r - (sy * sy + sx * sx)) * 10000.0f;
        }
    }
    const float reach = p.time * maxSpeed + r;
    const float last = (reach * reach - (ey * ey + ex * ex)) / p.length;
    if (last > 0.0f && closest < last) return last;
    if (closest > 0.0f) return closest;
    return -1.0f;
}

/// A unit the search found, with its measure.
struct Scored {
    uint64_t guid = 0;
    float score = 0.0f;
};

/// 0x006fcb60 after 0x006fcaa0: past fifteen, the list is heap-sorted by
/// 0x006fc7d0 - a heap with the least at its root, each least swapped to the
/// back - which leaves the greatest first, and the first fifteen are kept.
[[nodiscard]] inline std::vector<uint64_t> keepUnits(std::vector<Scored> found) {
    if (found.size() > kMaxUnits) {
        std::stable_sort(found.begin(), found.end(),
                         [](const Scored& a, const Scored& b) { return a.score > b.score; });
        found.resize(kMaxUnits);
    }
    std::vector<uint64_t> kept;
    kept.reserve(found.size());
    for (const Scored& s : found) kept.push_back(s.guid);
    return kept;
}

/// 0x006fc360: the share of the step from `from` to `to` at which it passes
/// closest to the first sphere it comes within, the spheres taken in order
/// and the first without a radius ending the list; -1 for none, or for a
/// step of no length.
[[nodiscard]] inline float stepHit(const glm::vec3& from, const glm::vec3& to, const std::vector<Sphere>& spheres) {
    const glm::vec3 d = to - from;
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len < 1e-05f) return -1.0f;
    const float inv = 1.0f / len;
    for (const Sphere& s : spheres) {
        if (s.radius <= 0.0f) return -1.0f;
        const glm::vec3 c = s.centre - from;
        const float along = c.y * (d.y * inv) + c.x * (d.x * inv) + c.z * (inv * d.z);
        if (0.0f <= along && (c.z * c.z + c.x * c.x + c.y * c.y) - along * along <= s.radius * s.radius &&
            along <= len)
            return along * inv;
    }
    return -1.0f;
}

}  // namespace wowee::rendering::missile_collision
