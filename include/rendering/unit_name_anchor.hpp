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

/// How many pixels a yard is on screen in the plane that faces the camera at
/// clip depth `clipW`, for a projection whose [1][1] is `proj11` and a screen
/// `screenH` pixels tall. The name's world height is measured this way
/// (0x007e5420 through the projection at the anchor).
inline float pixelsPerYard(float proj11, float screenH, float clipW) {
    return (proj11 < 0.0f ? -proj11 : proj11) * 0.5f * screenH / clipW;
}

/// The nearest view depth a name is sized for. A name is world text, a fifth
/// of a yard tall, so its size on screen grows as 1/depth; with the near clip
/// at the client's 0.2 a unit right by the camera - an NPC behind or under it
/// in a tight room - is still drawn, and its name filled half the screen. Up
/// close it keeps the size it has at this depth, about 40 pixels on a
/// 1080-line screen.
inline constexpr float kNameNearestSizingDepth = 5.0f;
inline float nameSizingDepth(float clipW) {
    return clipW > kNameNearestSizingDepth ? clipW : kNameNearestSizingDepth;
}

/// A point of text laid out on screen, as its offset in that plane through
/// the anchor: yards right and up of it. The screen's y runs down and the
/// view's up, so the second is turned over. Added to the anchor in view
/// space and projected, it lands back on the pixel it was laid out at, and
/// at the anchor's depth - the string stands in the world as the client's
/// world string batch does (0x007e5640), not over it.
inline glm::vec2 billboardOffset(const glm::vec2& pointPx, const glm::vec2& anchorPx, float pxPerYard) {
    return {(pointPx.x - anchorPx.x) / pxPerYard, -(pointPx.y - anchorPx.y) / pxPerYard};
}

}  // namespace wowee::rendering::unit_name_anchor
