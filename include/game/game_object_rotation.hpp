#pragma once

// A game object's rotation, as the update block's UPDATEFLAG_ROTATION carries
// it and the client keeps it.
//
// The movement block's reader keeps the packed 64 bits (0x004d3890, +0x2d0),
// and the game object takes them as its own rotation (0x00714250 copies them
// to its passenger's +0x20 and sets +0x2c bit 2, a full rotation rather than
// a facing). Without the flag the bits stay 0, which unpacks to no turn at
// all. Its facing is that rotation's yaw (GetFacing, 0x0070c310 ->
// 0x004f4630), and it is drawn by the matrix of its place and that rotation
// (0x00712ee0) - the stationary block's orientation is never asked.
//
// The quaternion is on the wire's axes (x, y, z, w), which are the
// renderer's: canonicalToRender and canonicalToServer are the same swap.

#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace wowee::game::game_object_rotation {

// 0x00982340: x the top 22 bits over 2^21, y the next 21 and z the low 21
// over 2^20, each signed; w what makes it unit, or 0 when x, y and z already
// are to within 2^-20.
inline glm::quat unpack(uint64_t packed) {
    const auto bits = static_cast<int64_t>(packed);
    const float x = static_cast<float>(bits >> 42) * (1.0f / 2097152.0f);
    const float y = static_cast<float>(static_cast<int32_t>(static_cast<uint32_t>(packed >> 10)) >> 11) *
                    (1.0f / 1048576.0f);
    const float z = static_cast<float>(static_cast<int64_t>(packed << 43) >> 43) * (1.0f / 1048576.0f);
    const float sum = x * x + y * y + z * z;
    const float w = std::abs(sum - 1.0f) < (1.0f / 1048576.0f) ? 0.0f : std::sqrt(1.0f - sum);
    return glm::quat(w, x, y, z);
}

// 0x004f4630: the yaw of a rotation, on the wire's axes - its facing.
inline float facing(const glm::quat& q) {
    const float c = 1.0f - (q.y * q.y + q.z * q.z) * 2.0f;
    const float s = (q.y * q.x + q.w * q.z) * 2.0f;
    constexpr float kEpsilon = 2.3841858e-07f;
    constexpr float kPi = 3.14159265358979323846f;
    if (std::abs(c) < kEpsilon) return s < 0.0f ? 1.5f * kPi : 0.5f * kPi;
    if (std::abs(s) < kEpsilon) return c <= 0.0f ? kPi : 0.0f;
    return std::atan2(s, c);
}

// The rotation a facing alone gives: a turn about +z.
inline glm::quat fromFacing(float facing) {
    return glm::angleAxis(facing, glm::vec3(0.0f, 0.0f, 1.0f));
}

// The renderers' placement angles (rendering::placementModelMatrix turns
// about z, then y, then x) for a rotation, so a game object is drawn by
// 0x00712ee0's matrix: its place, this rotation, its scale. One turned only
// about z comes out as (0, 0, its facing).
inline glm::vec3 placementEuler(const glm::quat& q) {
    const glm::mat3 m = glm::mat3_cast(glm::normalize(q));
    // m[column][row]: Rz(a) Ry(b) Rx(c) has row 2 (-sin b, cos b sin c,
    // cos b cos c) and column 0 (cos a cos b, sin a cos b, -sin b).
    const float b = std::asin(glm::clamp(-m[0][2], -1.0f, 1.0f));
    const float a = std::atan2(m[0][1], m[0][0]);
    const float c = std::atan2(m[1][2], m[2][2]);
    return {c, b, a};
}

}  // namespace wowee::game::game_object_rotation
