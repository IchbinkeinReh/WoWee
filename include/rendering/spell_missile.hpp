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
