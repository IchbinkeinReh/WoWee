#pragma once

/// The line from a fishing pole to its bobber, as the client draws it
/// (0x007221d0 makes it, 0x006f8f50 draws it from the pole's model):
/// 65 points from the tip to the bobber - 64 segments, a line strip - each
/// lowered by half a yard times sin(pi t), untextured, in one colour, the
/// pole's ambient light, opaque.

#include <array>
#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

namespace wowee::rendering {

namespace fishing_line {

/// The pole model's event the line hangs from (0x006f8f50: 0x008275f0 and
/// 0x008317e0 with "$CCH").
constexpr uint32_t kTipEvent = 0x48434324;  // "$CCH"
constexpr int kSegments = 64;
constexpr int kPoints = kSegments + 1;
constexpr float kSag = 0.5f;
/// The bobber end: the object's position raised by its scale times its
/// model's height (the header's vertex box, 0x00713f50 into +0xac) times this.
constexpr float kBobberHeightFraction = 0.2f;

/// 0x006f8f50: point i is tip + (end - tip) * i/64, lowered by 0.5 sin(pi i/64).
inline std::array<glm::vec3, kPoints> points(const glm::vec3& tip, const glm::vec3& end) {
    std::array<glm::vec3, kPoints> out{};
    const glm::vec3 delta = end - tip;
    for (int i = 0; i < kPoints; ++i) {
        const float t = static_cast<float>(i) * (1.0f / kSegments);
        out[i] = tip + delta * t;
        out[i].z -= std::sin(t * 3.1415927f) * kSag;
    }
    return out;
}

/// Where the line meets the bobber.
inline glm::vec3 bobberEnd(const glm::vec3& position, float scale, float modelHeight) {
    return position + glm::vec3(0.0f, 0.0f, scale * modelHeight * kBobberHeightFraction);
}

/// The line's colour: the pole's lighting's ambient (+0x54), each channel
/// held within 0..1 (0x006f8f50 rounds it to a byte), alpha 1.
inline glm::vec3 colour(const glm::vec3& ambient) {
    return glm::clamp(ambient, glm::vec3(0.0f), glm::vec3(1.0f));
}

}  // namespace fishing_line

}  // namespace wowee::rendering
