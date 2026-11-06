#pragma once

/// The aimed trajectory cast: a vehicle weapon - a catapult, a siege engine -
/// whose spell's SpellMissile row has Flags bit 1 is not cast at a target.
/// The client works out where its shot lands and sends that with the cast
/// (0x0080ac90 -> 0x00809f80 -> 0x006fcd60).
///
/// What is here is the solver's arithmetic, kept apart from the world so it
/// can be tested: the pitch, speed and facing the shot leaves with, and the
/// path it flies in 0.1 s steps until something is in the way.

#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>

#include <glm/glm.hpp>

namespace wowee::game::missile_trajectory {

/// SpellMissile.dbc, as 0x006fcd60 reads it (+4 .. +0x34).
struct MissileRow {
    uint32_t flags = 0;          ///< column 1; bit 1 makes a spell an aimed one
    float pitchMin = 0.0f;       ///< 2
    float pitchMax = 0.0f;       ///< 3
    float speedMin = 0.0f;       ///< 4
    float speedMax = 0.0f;       ///< 5
    float facingRandMin = 0.0f;  ///< 6
    float facingRandMax = 0.0f;  ///< 7
    float pitchRandMin = 0.0f;   ///< 8
    float pitchRandMax = 0.0f;   ///< 9
    float speedRandMin = 0.0f;   ///< 10
    float speedRandMax = 0.0f;   ///< 11
    float gravity = 0.0f;        ///< 12, yards per second squared
    float maxDuration = 0.0f;    ///< 13, seconds; none for 20
};
inline constexpr uint32_t kFlagAimed = 0x1;

/// Vehicle.dbc Flags the solver reads from the caster's own vehicle row.
inline constexpr uint32_t kVehicleFlagAimFromPitch = 0x10;  ///< the unit's pitch, not the row's
inline constexpr uint32_t kVehicleFlagPitchLimits = 0x40;   ///< that pitch held in PitchMin..PitchMax
inline constexpr uint32_t kVehicleFlagAimPower = 0x800;     ///< the speed by VehicleAimSetNormPower

/// What the solver takes from the caster.
struct AimInput {
    bool hasVehicleRow = false;
    uint32_t vehicleFlags = 0;
    float vehiclePitchMin = 0.0f;  ///< Vehicle.dbc +0x10
    float vehiclePitchMax = 0.0f;  ///< +0x14
    float unitPitch = 0.0f;
    float aimPower = 0.0f;         ///< DAT_00c24958, 0..1
    float facing = 0.0f;           ///< the unit's, in the wire's convention
    glm::vec3 launch{0.0f};        ///< where the shot leaves, wire coordinates
};

/// What the shot leaves with.
struct Aim {
    float pitch = 0.0f;
    float speed = 0.0f;
    float facing = 0.0f;
    glm::vec3 launch{0.0f};
};

/// The pitch, speed and facing (0x006fcd60's first half). `random` gives a
/// value in [0, 1) each time it is asked, and is asked eight times in this
/// order: the pitch's spread, the speed's, the facing's, then the jitter of
/// the pitch (+-0.01), the speed (+-0.01) and the launch point's x, y (+-0.1)
/// and z (+-0.01). `exact` takes the middle of each spread with no jitter, as
/// the client's aim preview does (param_10).
[[nodiscard]] inline Aim chooseAim(const MissileRow& row, const AimInput& in, const std::function<float()>& random,
                                   bool exact = false) {
    Aim aim;
    aim.launch = in.launch;
    if (in.hasVehicleRow && (in.vehicleFlags & kVehicleFlagAimFromPitch)) {
        aim.pitch = in.unitPitch;
        if (in.vehicleFlags & kVehicleFlagPitchLimits) {
            // 0x00497a90
            if (aim.pitch < in.vehiclePitchMin) aim.pitch = in.vehiclePitchMin;
            if (aim.pitch > in.vehiclePitchMax) aim.pitch = in.vehiclePitchMax;
        }
    } else {
        aim.pitch = (row.pitchMax + row.pitchMin) * 0.5f;
    }
    if (in.hasVehicleRow && (in.vehicleFlags & kVehicleFlagAimPower))
        aim.speed = (row.speedMax - row.speedMin) * in.aimPower + row.speedMin;
    else
        aim.speed = (row.speedMax + row.speedMin) * 0.5f;
    if (exact) {
        aim.pitch += (row.pitchRandMax + row.pitchRandMin) * 0.5f;
        aim.speed += (row.speedRandMax + row.speedRandMin) * 0.5f;
        aim.facing = (row.facingRandMax + row.facingRandMin) * 0.5f + in.facing;
        return aim;
    }
    aim.pitch += row.pitchRandMin;
    aim.pitch += random() * (row.pitchRandMax - row.pitchRandMin);
    aim.speed += row.speedRandMin;
    aim.speed += random() * (row.speedRandMax - row.speedRandMin);
    aim.facing = random() * (row.facingRandMax - row.facingRandMin) + row.facingRandMin + in.facing;
    aim.pitch = random() * 0.02f + aim.pitch - 0.01f;
    aim.speed = random() * 0.02f + aim.speed - 0.01f;
    aim.launch.x = random() * 0.2f + aim.launch.x - 0.1f;
    aim.launch.y = random() * 0.2f + aim.launch.y - 0.1f;
    aim.launch.z = random() * 0.02f + aim.launch.z - 0.01f;
    return aim;
}

/// How many 0.1 s steps the shot may fly: MaxDuration's, at most 200, and
/// 200 for a row with none.
[[nodiscard]] inline int maxSteps(const MissileRow& row) {
    if (row.maxDuration <= 0.0001f) return 200;
    const int steps = static_cast<int>(row.maxDuration * 10.0f);
    return steps > 200 ? 200 : steps;
}

/// Where a segment from `from` to `to` first meets the world, as a share of
/// it (0x0077f310 with 0x100111), or none.
using SegmentQuery = std::function<std::optional<float>(const glm::vec3& from, const glm::vec3& to)>;

struct Flight {
    glm::vec3 impact{0.0f};
    float seconds = 0.0f;
};

/// The shot's path (0x006fcd60's second half). `origin` is the caster's
/// middle: where the launch point is behind a wall from there, the shot
/// lands at once. Otherwise it flies in steps of 0.1 s - each one the speed
/// a tenth of a second carries it, falling by Gravity - until a step meets
/// the world, which is where it lands, or the steps run out, where it is
/// then.
[[nodiscard]] inline Flight fly(const MissileRow& row, const Aim& aim, const glm::vec3& origin,
                                const SegmentQuery& segment) {
    Flight flight;
    if (segment && segment(origin, aim.launch)) {
        flight.impact = aim.launch;
        return flight;
    }
    const float perStep = aim.speed * 0.1f;
    const float cosPitch = std::cos(aim.pitch);
    glm::vec3 step(std::cos(aim.facing) * cosPitch * perStep, std::sin(aim.facing) * cosPitch * perStep, 0.0f);
    const float fall = row.gravity * 0.01f;
    step.z = perStep * std::sin(aim.pitch) - fall * 0.5f;
    const int steps = maxSteps(row);
    glm::vec3 from = aim.launch;
    glm::vec3 to = from + step;
    int left = steps;
    std::optional<float> hit = segment ? segment(from, to) : std::nullopt;
    while (!hit) {
        if (--left < 1) {
            flight.impact = to;
            flight.seconds = static_cast<float>(steps) * 0.1f;
            return flight;
        }
        step.z -= fall;
        from = to;
        to = to + step;
        hit = segment ? segment(from, to) : std::nullopt;
    }
    flight.impact = from + (to - from) * *hit;
    flight.seconds = (static_cast<float>(steps - left) + *hit) * 0.1f;
    return flight;
}

}  // namespace wowee::game::missile_trajectory
