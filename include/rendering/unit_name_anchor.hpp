#pragma once

/// Where the client puts a unit's name and nameplate, and which plates it
/// shows by distance and draws dimmed (Unit_C.cpp, NamePlateFrame.cpp).

#include <glm/glm.hpp>

#include <cstdint>

namespace wowee::rendering::unit_name_anchor {

/// The model attachments 0x0071fef0 asks for the name: PlayerNameMounted on a
/// unit that rides, PlayerName otherwise (and when the mounted one is missing).
constexpr uint32_t kAttachmentPlayerName = 18;
constexpr uint32_t kAttachmentPlayerNameMounted = 29;

/// 0x0071fef0 without either attachment: the unit's position raised by its
/// height times its scale, and a quarter again.
inline glm::vec3 fallbackNamePosition(const glm::vec3& position, float height, float scale) {
    return position + glm::vec3(0.0f, 0.0f, height * scale * 1.25f);
}

/// 0x00715720: a nameplate stands two thirds of a yard over the name.
constexpr float kPlateLift = 0.6666667f;

/// 0x0072b060: a plate is shown while the unit is within 41 yards of the
/// player (0x00adaa7c holds 1681, the square), measured between their
/// positions; in an arena there is no limit.
constexpr float kPlateRangeSq = 1681.0f;
inline bool plateInRange(const glm::vec3& player, const glm::vec3& unit) {
    const glm::vec3 d = unit - player;
    return glm::dot(d, d) <= kPlateRangeSq;
}

/// 0x0098e9f0: with a target, every other unit's plate at alpha 0x7f; the
/// target's, or every plate with no target, at 0xff.
constexpr float plateAlpha(bool haveTarget, bool isTarget) {
    return haveTarget && !isTarget ? 127.0f / 255.0f : 1.0f;
}

}  // namespace wowee::rendering::unit_name_anchor
