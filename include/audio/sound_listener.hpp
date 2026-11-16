#pragma once

/// Where the world frame puts the sound listener each frame (0x004fa5f0,
/// after the camera's update): with Sound_ListenerAtCharacter on (its
/// default, "1"), behind and above the listener object (0x007450b0 - the
/// active mover, else the player, else what the player's far sight looks
/// through), facing the way it faces; otherwise at the camera, facing the
/// camera's way. Arithmetic only: the object and the camera are the caller's.

#include <cmath>
#include <optional>

#include <glm/glm.hpp>

namespace wowee::audio::sound_listener {

/// Sound_ListenerBackDist and Sound_ListenerUpDist, which 0x004fa5f0 looks up
/// but the client never registers: their fallbacks, 2 (0x00a4040c) and 4
/// (0x009e8d2c), always apply.
inline constexpr float kBackDist = 2.0f;
inline constexpr float kUpDist = 4.0f;

struct Placement {
    glm::vec3 position{0.0f};
    glm::vec3 forward{0.0f, 1.0f, 0.0f};
    glm::vec3 up{0.0f, 0.0f, 1.0f};
};

/// The listener object as 0x004fa5f0 reads it: where it stands, and for a
/// unit the way it faces (0x004f6a90: its facing's cosine and sine, level).
/// Anything else - a far sight's dynamic object - faces the camera's way.
struct Anchor {
    glm::vec3 position{0.0f};
    std::optional<glm::vec3> facing;
};

/// The facing 0x004f6a90 builds from a yaw: (cos, sin, 0).
[[nodiscard]] inline glm::vec3 levelFacing(float yawRadians) {
    return {std::cos(yawRadians), std::sin(yawRadians), 0.0f};
}

/// 0x004fa5f0: at the character, two yards back along its facing and four
/// up (0x004c5b20 with the up (0,0,1) of 0x00b743b8); else at the camera.
[[nodiscard]] inline Placement place(const std::optional<Anchor>& anchor, const glm::vec3& cameraPosition,
                                     const glm::vec3& cameraForward) {
    Placement p;
    if (!anchor) {
        p.position = cameraPosition;
        p.forward = cameraForward;
        return p;
    }
    p.forward = anchor->facing ? *anchor->facing : cameraForward;
    p.position = anchor->position - p.forward * kBackDist + glm::vec3(0.0f, 0.0f, kUpDist);
    return p;
}

}  // namespace wowee::audio::sound_listener
