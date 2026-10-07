#pragma once

/// Spell missiles: the model that flies from the caster to the target between
/// SMSG_SPELL_GO and the impact - the Frostbolt, the Shadow Bolt, the Wrath.
///
/// In 3.3.5a this is CMissile (Missile_C.cpp). SMSG_SPELL_GO hands every
/// target to FUN_007fffb0, which builds one missile per target with
/// FUN_00732ff0 from the spell's SpellVisual row: the model is
/// SpellVisualEffectName[MissileModel], the speed is Spell.dbc Speed (column
/// 47, yards per second), the destination is MissileDestinationAttachment on
/// the target plus MissileImpactOffset. The caster launches it from
/// MissileAttachment plus MissileCastOffset (FUN_00720bf0). Every frame
/// FUN_007015d0 moves it straight at where the target is now - it homes - by
/// speed times the frame time, faces it along that line, and when the step
/// would reach the target it arrives (FUN_00703410), which is when the impact
/// kit plays (FUN_00700e20) and the missile goes away.
///
/// What is here is the arithmetic of that, kept apart from the renderer so it
/// can be tested without one.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <string>

namespace wowee::rendering::spell_missile {

/// SpellVisual.dbc MissileAttachment and MissileDestinationAttachment are not
/// M2 attachment ids. They index the client's table at 0x00ADAA20 (read by
/// FUN_0071a7f0 and FUN_0071a860): head, chest, base, the two spell hands,
/// breath, the three specials, the two chest blood points, the omni and the
/// directed spell hand. Entry 13 is the main-hand weapon; without a weapon
/// model of its own the client falls back to the right spell hand (22,
/// DAT_00ADAA30), which is what it is here.
inline constexpr std::array<int32_t, 16> kVisualAttachmentToM2 = {
    20, 34, 19, 21, 22, 17, 23, 24, 25, 15, 16, 37, 38, 22, 47, 48};

/// SpellVisual.dbc Flags the missile reads.
/// 0x200: the two attachment columns hold M2 attachment ids already.
inline constexpr uint32_t kFlagRawAttachmentIds = 0x200;
/// 0x1: a spell aimed at a location still sends a missile to each target.
inline constexpr uint32_t kFlagMissilePerTargetAtLocation = 0x1;
/// 0x100: no missile to a location when nothing was hit.
inline constexpr uint32_t kFlagNoLocationMissileWithoutTargets = 0x100;

/// When a target's model has no attachment the visual names, FUN_007022d0
/// tries ChestBloodFront (15), then Base (19), then gives up on attachments
/// and aims at the unit's origin.
inline constexpr std::array<int32_t, 2> kDestinationFallbacks = {15, 19};

/// The M2 attachment id a SpellVisual attachment column names, or -1 for
/// none (the column holds -1 when the visual leaves it to the default).
inline int32_t m2AttachmentFor(int32_t dbcValue, uint32_t visualFlags) {
    if (dbcValue < 0) return -1;
    if (visualFlags & kFlagRawAttachmentIds) return dbcValue;
    if (static_cast<size_t>(dbcValue) >= kVisualAttachmentToM2.size()) return -1;
    return kVisualAttachmentToM2[static_cast<size_t>(dbcValue)];
}

/// SpellVisual.dbc offsets are stored with y mirrored: the client negates it
/// before carrying the offset through the attachment's frame (FUN_00720bf0,
/// FUN_00732ff0).
inline glm::vec3 attachmentOffset(const glm::vec3& dbcOffset) {
    return {dbcOffset.x, -dbcOffset.y, dbcOffset.z};
}

/// SpellCastTargets flags SMSG_SPELL_GO carries, as FUN_007fffb0 tests them.
inline constexpr uint32_t kTargetFlagUnit           = 0x0002;
inline constexpr uint32_t kTargetFlagSourceLocation = 0x0020;
inline constexpr uint32_t kTargetFlagDestLocation   = 0x0040;
inline constexpr uint32_t kTargetFlagGameObject     = 0x0800;

enum class Targeting {
    None,           ///< no missile at all
    EachTarget,     ///< one missile to every target in the hit and miss lists
    Location,       ///< one missile to the cast's source or destination point
};

/// Which missiles a cast sends, as FUN_007fffb0 decides it: one per target
/// when the cast was aimed at a unit or object and either named no location
/// or the visual asks for per-target missiles anyway; otherwise one to the
/// location, unless nothing was hit and the visual says not to bother.
inline Targeting chooseTargeting(uint32_t targetFlags, size_t targetCount,
                                 uint32_t visualFlags) {
    const bool hasLocation =
        (targetFlags & (kTargetFlagSourceLocation | kTargetFlagDestLocation)) != 0;
    const bool perTargetAllowed =
        !hasLocation || (visualFlags & kFlagMissilePerTargetAtLocation) != 0;
    if (targetCount != 0 &&
        (targetFlags & (kTargetFlagUnit | kTargetFlagGameObject)) != 0 &&
        perTargetAllowed) {
        return Targeting::EachTarget;
    }
    if (hasLocation &&
        (targetCount != 0 || (visualFlags & kFlagNoLocationMissileWithoutTargets) == 0)) {
        return Targeting::Location;
    }
    return Targeting::None;
}

/// Seconds a missile needs to cover `distance` yards at `speed` yards per
/// second, if neither end moves. A spell without a speed hits at once and
/// sends nothing - FUN_007015d0 does not move a missile whose speed is zero.
inline float flightTime(float distance, float speed) {
    if (!(speed > 0.0f)) return 0.0f;
    return std::max(distance, 0.0f) / speed;
}

struct Step {
    glm::vec3 position{0.0f};
    bool arrived = false;
};

/// One frame of FUN_007015d0: from `position` straight at `target` - the
/// target as it is this frame, so a missile follows a unit that runs - by
/// `speed * dt`. When that step would reach the target the missile has
/// arrived and stands on it.
inline Step advance(const glm::vec3& position, const glm::vec3& target,
                    float speed, float dt) {
    const glm::vec3 toTarget = target - position;
    const float distance = glm::length(toTarget);
    const float stepLength = std::max(speed, 0.0f) * std::max(dt, 0.0f);
    if (distance <= stepLength || distance < 1e-4f) {
        return {target, true};
    }
    return {position + toTarget * (stepLength / distance), false};
}

// ---------------------------------------------------------------------------
// ADJUST_MISSILE: the arc.
//
// A cast with flag 0x20000 - siege engines, vehicles, a thrown barrel - names
// in SMSG_SPELL_GO the missile's elevation and how long after the packet it
// lands (0x0080e1b0). Such a missile is not homed: at launch the client works
// out one ballistic flight from the launch point to where the target is then,
// under the gravity of the spell's SpellMissile row (0x00700880), and every
// frame puts the missile on it, landing when the time is up (0x007015d0).

/// Gravity, yards per second squared, when the spell has no SpellMissile row
/// (0x419a542f, 0x00700880).
inline constexpr float kDefaultMissileGravity = 19.29f;

/// What the arc reads of a SpellMissile.dbc row: its two speeds (+0x10,
/// +0x14) and its gravity (+0x30) - columns 4, 5 and 12 of the fifteen the
/// client loads (0x008b8310).
struct SpellMissileRow {
    float speeds[2] = {0.0f, 0.0f};
    float gravity = kDefaultMissileGravity;
};

/// An elevation past straight up or down folded back into [-pi/2, pi/2]
/// (0x00700880).
inline float foldElevation(float pitch) {
    constexpr float kHalfPi = 1.5707964f;
    if (pitch > kHalfPi) return pitch - (pitch - kHalfPi) * 2.0f;
    if (pitch < -kHalfPi) return pitch + (-kHalfPi - pitch) * 2.0f;
    return pitch;
}

enum class ArcSolve { NoSolution = 0, Degenerate = 1, Solved = 2 };

/// The flight that leaves at `pitch` and passes through `delta` under
/// `gravity` (0x0076c070): its launch speed, its time and its velocity.
/// Degenerate when the pitch is vertical, the target straight above or
/// below, or there is no gravity; no solution when the target lies above the
/// line the pitch points along.
inline ArcSolve solveArcAtPitch(float pitch, const glm::vec3& delta, float gravity,
                                float& speed, float& time, glm::vec3& velocity) {
    const float c = std::cos(pitch);
    const float s = std::sin(pitch);
    const float horizontal = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    if (std::abs(c) < 1e-4f || std::abs(horizontal) < 1e-4f || std::abs(gravity) < 1e-4f)
        return ArcSolve::Degenerate;
    const float timeSq = ((1.0f / c) * horizontal * s - delta.z) / gravity * 2.0f;
    if (timeSq <= 0.01f) return ArcSolve::NoSolution;
    time = std::sqrt(timeSq);
    speed = (horizontal / time) * (1.0f / c);
    velocity = {speed * delta.x * (1.0f / horizontal) * c,
                speed * (1.0f / horizontal) * c * delta.y,
                s * speed};
    return ArcSolve::Solved;
}

/// The time a flight at `pitch` and `speed` takes to reach `delta`
/// (0x0076bf80): by the horizontal distance when it moves sideways, else by
/// the height under gravity, the earlier root that is not negative. -1 when
/// it never gets there.
inline float arcTimeAtSpeed(float pitch, float speed, const glm::vec3& delta, float gravity) {
    const float horizontalSpeed = std::cos(pitch) * speed;
    if (horizontalSpeed > 1e-4f)
        return std::sqrt(delta.x * delta.x + delta.y * delta.y) / horizontalSpeed;
    if (speed <= 1e-4f) {
        if (std::abs(gravity) <= 1e-4f) return -1.0f;
    } else if (std::abs(gravity) <= 1e-4f) {
        return std::abs(delta.z) / speed;
    }
    const float vertical = std::sin(pitch) * speed;
    const float disc = vertical * vertical - (delta.z * gravity + delta.z * gravity);
    if (disc < 0.0f) return -1.0f;
    const float root = std::sqrt(disc);
    const float first = (root - vertical) * (-1.0f / gravity);
    const float second = (-vertical - root) * (-1.0f / gravity);
    if (first >= 0.0f && (second < 0.0f || first < second)) return first;
    return second;
}

/// A missile's flight, as 0x00700880 plans it at launch.
struct MissileArc {
    /// On the arc. False when no arc reaches the target: the missile then
    /// flies straight at `straightSpeed`, timed to land when the arc would.
    bool onArc = false;
    glm::vec3 velocity{0.0f};
    float gravity = kDefaultMissileGravity;
    /// Arc seconds per second of flight, so the arc's own time fits the
    /// flight the server gave it; held to [0, 2].
    float timeScale = 0.0f;
    float straightSpeed = 0.0f;
    /// Seconds from launch to landing.
    float flightSeconds = 0.0f;
};

/// Plan the flight from a launch point to a target `delta` away, at the
/// cast's `elevation`, landing `flightSeconds` after launch (0x00700880).
/// `castSpeed` is the speed the cast carries - zero for another's cast, whose
/// spell object starts cleared (0x009ab770). `row` is the spell's SpellMissile
/// row, null when it has none.
inline MissileArc planMissileArc(float elevation, float castSpeed, const glm::vec3& delta,
                                 const SpellMissileRow* row, float flightSeconds) {
    MissileArc arc;
    arc.gravity = row ? row->gravity : kDefaultMissileGravity;
    float pitch = foldElevation(elevation);
    float speed = 0.0f;
    float time = 0.0f;
    glm::vec3 velocity(0.0f);
    bool onArc = true;
    ArcSolve solved = solveArcAtPitch(pitch, delta, arc.gravity, speed, time, velocity);
    if (solved == ArcSolve::NoSolution) {
        // Once more a hundredth of a radian further along the gravity.
        const float nudge = (std::isnan(arc.gravity) || arc.gravity < 0.0f) ? -0.01f : 0.01f;
        pitch = foldElevation(elevation + nudge);
        solved = solveArcAtPitch(pitch, delta, arc.gravity, speed, time, velocity);
        if (solved == ArcSolve::NoSolution) onArc = false;
    }
    if (onArc && solved == ArcSolve::Degenerate) {
        // Timed by a speed instead: the cast's, then the row's two.
        if (!row) {
            onArc = false;
        } else {
            time = arcTimeAtSpeed(pitch, castSpeed, delta, arc.gravity);
            if (time < 1e-4f) time = arcTimeAtSpeed(pitch, row->speeds[0], delta, arc.gravity);
            if (time < 1e-4f) time = arcTimeAtSpeed(pitch, row->speeds[1], delta, arc.gravity);
            if (time < 1e-4f) onArc = false;
            else velocity = delta * (1.0f / time);
        }
    }
    const float remaining = std::max(flightSeconds, 1e-4f);
    arc.flightSeconds = std::max(flightSeconds, 0.0f);
    arc.onArc = onArc;
    if (!onArc) {
        arc.straightSpeed = glm::length(delta) / remaining;
        return arc;
    }
    arc.velocity = velocity;
    const float scale = time / remaining;
    arc.timeScale = scale < 0.0f ? 0.0f : (scale >= 2.0f ? 2.0f : scale);
    return arc;
}

/// Where a missile on its arc is `elapsed` seconds after leaving `start`
/// (0x007015d0): along the launch velocity, fallen under gravity, both in the
/// arc's own time.
inline glm::vec3 arcPosition(const glm::vec3& start, const MissileArc& arc, float elapsed) {
    const float t = std::max(elapsed, 0.0f) * arc.timeScale;
    glm::vec3 p = start + arc.velocity * t;
    p.z -= arc.gravity * t * t * 0.5f;
    return p;
}

/// Euler angles (render axes, the order placementModelMatrix composes) that
/// turn an M2's forward axis, +x, onto `direction` - the client builds the
/// missile's matrix from its travel direction each frame (FUN_00701230). A
/// zero direction keeps the model level, facing +x.
inline glm::vec3 facingEuler(const glm::vec3& direction) {
    const float horizontal = std::sqrt(direction.x * direction.x + direction.y * direction.y);
    if (horizontal < 1e-6f && std::abs(direction.z) < 1e-6f) return glm::vec3(0.0f);
    return {0.0f, -std::atan2(direction.z, horizontal), std::atan2(direction.y, direction.x)};
}

/// A missile's sound: SpellVisual MissileSound, a SoundEntries row, started
/// at the missile when it is launched and kept looping wherever it flies
/// (0x007022d0 starts it with the loop forced on; 0x007015d0 moves it each
/// frame), then faded out over 0.15 s when it arrives (0x00703410).
inline constexpr float kMissileSoundFadeSeconds = 0.15f;

/// The file a SoundEntries row plays, from its DirectoryBase and its ten
/// File columns: the first that is set. Empty when none is.
inline std::string soundEntryFile(const std::string& directory, const std::array<std::string, 10>& files) {
    for (const std::string& file : files) {
        if (file.empty()) continue;
        return directory.empty() ? file : directory + "\\" + file;
    }
    return {};
}

}  // namespace wowee::rendering::spell_missile
