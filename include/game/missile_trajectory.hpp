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
#include <cstring>
#include <functional>
#include <optional>
#include <string_view>

#include <glm/glm.hpp>

#include "game/chat_language.hpp"

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
    float collisionRadius = 0.0f;///< 14, added to each unit's sphere (0x006fc0a0)
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

/// The aimed cast the active mover has going (its +0xf68..+0xf78): the
/// spell and when it was sent (0x006fbe30, after 0x0080ac90 sends the
/// cast), when SMSG_SPELL_START said it began and its cast time (0x006fbe50,
/// from 0x00806700), and the cast count of a missile held for it
/// (0x006fe7e0). Times are the client's milliseconds.
struct AimState {
    uint32_t spellId = 0;        ///< +0xf68
    uint32_t sentMs = 0;         ///< +0xf6c
    uint32_t startedMs = 0;      ///< +0xf70
    uint32_t castMs = 0;         ///< +0xf74
    uint8_t heldCastCount = 0;   ///< +0xf78

    /// 0x006fbe30.
    void sent(uint32_t spell, uint32_t nowMs) {
        spellId = spell;
        sentMs = nowMs;
    }
    /// 0x006fbe50: only for the spell sent.
    void started(uint32_t spell, uint32_t nowMs, uint32_t castTimeMs) {
        if (spell != spellId) return;
        startedMs = nowMs;
        castMs = castTimeMs;
    }
    /// 0x006fbe80: only for the spell sent; the cast time is left.
    void clear(uint32_t spell) {
        if (spell != spellId) return;
        spellId = 0;
        sentMs = 0;
        startedMs = 0;
        heldCastCount = 0;
    }
    /// 0x006fbeb0: sent and begun - another aimed cast is refused
    /// (0x00809f80, SPELL_FAILED_SPELL_IN_PROGRESS).
    [[nodiscard]] bool busy() const { return sentMs != 0 && startedMs != 0; }
    /// 0x006fbed0: and a missile held for it.
    [[nodiscard]] bool holding() const { return busy() && heldCastCount != 0; }
    /// 0x006fe7e0: none held, and the cast time has passed since it was
    /// sent.
    [[nodiscard]] bool due(uint32_t nowMs) const {
        return sentMs != 0 && startedMs != 0 && heldCastCount == 0 &&
               static_cast<int32_t>(nowMs - castMs - sentMs) >= 0;
    }
};

/// How long the shot is let fly (0x00809f80, 0x006fd6b0): half the latency,
/// the solved time and a fifth of a second, at most 1.3 times the solved
/// time.
[[nodiscard]] inline float flightWithLatency(float seconds, uint32_t latencyMs) {
    const float padded = static_cast<float>(latencyMs) * 0.001f * 0.5f + seconds + 0.2f;
    const float cap = seconds * 1.3f;
    return cap < padded ? cap : padded;
}

/// The bits 0x007fe520 sets in an aimed shot's figures before they are
/// sent: a hash of some of their bits and a running count (0x00d3f4ec,
/// which the active player's object starts at its guid's low half,
/// 0x006e6b40 -> 0x007fe510, and each stamp moves on by one) is spread over
/// their lowest bits, then each takes one bit by 0x28d7's parity table.
/// The change is far below a millimetre.
struct Stamped {
    float pitch = 0.0f;
    float speed = 0.0f;
    glm::vec3 launch{0.0f};
    glm::vec3 impact{0.0f};
};
[[nodiscard]] inline uint32_t floatBits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}
[[nodiscard]] inline float bitsFloat(uint32_t u) {
    float f;
    std::memcpy(&f, &u, sizeof f);
    return f;
}
[[nodiscard]] inline Stamped stamp(uint32_t& counter, float pitch, float speed, const glm::vec3& launch,
                                   const glm::vec3& impact) {
    uint32_t p1 = floatBits(pitch);
    uint32_t p2 = floatBits(speed);
    uint32_t p3[3] = {floatBits(launch.x), floatBits(launch.y), floatBits(launch.z)};
    uint32_t p4[3] = {floatBits(impact.x), floatBits(impact.y), floatBits(impact.z)};
    const uint32_t key = ((((p3[2] & 0xf000u) ^ p3[0]) & 0x2f000u) ^ (p3[1] & 0x300ee630u) ^ (p1 & 0x6000cf0u) ^
                          (p4[1] & 0x60e0400u) ^ (p2 & 0x240e00u) ^ (p4[0] & 0xa9070f01u) ^ counter) |
                         0x1010101u;
    ++counter;
    // 0x0076f340(key's four bytes, 1, 0): none of them is 0.
    const char text[4] = {static_cast<char>(key), static_cast<char>(key >> 8), static_cast<char>(key >> 16),
                          static_cast<char>(key >> 24)};
    const uint32_t h = chat_language::stringHash(std::string_view(text, 4), false);
    p1 = (h >> 0x1b & 2u) | (h >> 10 & 1u) | (p1 & 0xfffffffcu);
    p2 = (h >> 0x13 & 8u) | (h >> 1 & 1u) | (p2 & 0xfffffff6u);
    p3[0] = (((h >> 9 & 0x10000u) | (h & 0x8000u)) >> 0xe) | (p3[0] & 0xfffffff9u);
    p3[1] = p3[1] ^ ((p3[1] ^ h >> 1) & 2u);
    p3[1] = p3[1] ^ ((h >> 0xb ^ p3[1]) & 8u);
    p3[2] = p3[2] ^ ((p3[2] ^ h >> 6) & 8u);
    p3[2] = p3[2] ^ ((h >> 0x1c ^ p3[2]) & 4u);
    p4[0] = (h >> 3 & 4u) | (h >> 6 & 2u) | (p4[0] & 0xfffffff9u);
    p4[1] = p4[1] ^ ((h >> 0x1d ^ p4[1]) & 1u);
    p4[1] = p4[1] ^ ((p4[1] ^ h >> 3) & 8u);
    p4[2] = p4[2] ^ ((h >> 0x14 ^ p4[2]) & 1u);
    p4[2] = p4[2] ^ ((p4[2] ^ h >> 10) & 4u);
    // Each value's parity bit, by 0x28d7 over the folded value without it.
    const auto parity = [](uint32_t& v, uint32_t bit) {
        const uint32_t kept = v & ~bit;
        uint32_t x = kept ^ (v >> 0x10);
        x = x ^ (x >> 8);
        const uint32_t index = (static_cast<uint8_t>(x >> 4) ^ static_cast<uint8_t>(x)) & 0xfu;
        v = ((0x28d7u >> index) & 1u ? bit : 0u) | kept;
    };
    parity(p1, 8u);
    parity(p2, 2u);
    parity(p3[0], 8u);
    parity(p3[1], 4u);
    parity(p3[2], 1u);
    parity(p4[0], 1u);
    parity(p4[1], 4u);
    parity(p4[2], 2u);
    Stamped out;
    out.pitch = bitsFloat(p1);
    out.speed = bitsFloat(p2);
    out.launch = glm::vec3(bitsFloat(p3[0]), bitsFloat(p3[1]), bitsFloat(p3[2]));
    out.impact = glm::vec3(bitsFloat(p4[0]), bitsFloat(p4[1]), bitsFloat(p4[2]));
    return out;
}

}  // namespace wowee::game::missile_trajectory
