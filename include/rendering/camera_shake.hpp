#pragma once

/// The client's camera shakes, apart from the camera.
///
/// A SpellEffectCameraShakes row names up to three CameraShakes rows, and
/// each is added to the camera's list where it happens (0x00606410 ->
/// 0x00606330): a spell kit's ShakeID (+0x40) where its first model loads
/// (0x006f9840) or, with none, at the unit (0x0073b140, 0x007fa620);
/// SMSG_CAMERA_SHAKE at the active player. Each frame (0x00606970) the
/// strongest live shake in each of the three directions, faded with the
/// camera's distance from where it happened (0x006004b0), moves the camera's
/// pivot (0x00606f90) along the followed unit's facing, its left or up
/// (0x005fe6c0).

#include <array>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <vector>

namespace wowee::rendering::camera_shake {

/// A CameraShakes.dbc row as 0x00606410 hands it over.
struct Shake {
    uint32_t type = 0;       ///< ShakeType (+4): 1 decays by Coefficient
    uint32_t direction = 0;  ///< Direction (+8): 0 forward, 1 left, 2 up
    float amplitude = 0.0f;  ///< Amplitude (+0xc) / 36
    float frequency = 0.0f;  ///< Frequency (+0x10), cycles a second
    float duration = 0.0f;   ///< Duration (+0x14), seconds
    float phase = 0.0f;      ///< Phase (+0x18), seconds it starts into
    float coefficient = 0.0f;///< Coefficient (+0x1c)
};

/// 0x00606410: the row's amplitude is in 36ths of a yard.
inline Shake fromRow(uint32_t type, uint32_t direction, float amplitude, float frequency, float duration,
                     float phase, float coefficient) {
    return Shake{.type = type, .direction = direction, .amplitude = amplitude * 0.027777778f,
                 .frequency = frequency, .duration = duration, .phase = phase, .coefficient = coefficient};
}

/// A shake on the camera: its row, where it happened and when it began.
struct Active {
    Shake shake;
    glm::vec3 origin{0.0f};
    float startSeconds = 0.0f;
};

/// 0x006004b0: a shake's amplitude at the camera's squared distance from
/// it - whole within 9 yards, 0.7 to the power of each further 9 yards out
/// to 80, nothing beyond.
inline std::optional<float> attenuated(float amplitude, float distanceSq) {
    if (distanceSq > 6400.0f) return std::nullopt;
    if (distanceSq > 81.0f) amplitude *= std::pow(0.7f, (std::sqrt(distanceSq) - 9.0f) / 9.0f);
    return amplitude;
}

/// 0x005fe6c0: the displacement `t` seconds in, sin(2 pi f t) times the
/// amplitude, decaying by e^(-t c) for a type 1 shake.
inline float displacement(const Shake& shake, float amplitude, float t) {
    float d = std::sin(shake.frequency * t * 6.2831855f) * amplitude;
    if (shake.type == 1) d *= std::exp(-(t * shake.coefficient));
    return d;
}

/// 0x00606970: the camera's offset this frame (in render space, where the
/// axes are the client's own) and the shakes that have run out dropped.
/// `facingRad` is the followed unit's facing, its forward (cos, sin).
inline glm::vec3 offset(std::vector<Active>& shakes, float nowSeconds, const glm::vec3& cameraPos,
                        float facingRad) {
    // Those run out go first (0x006064f0), so nothing below points at a
    // moved element.
    std::erase_if(shakes, [nowSeconds](const Active& a) {
        return !((nowSeconds - a.startSeconds) + a.shake.phase < a.shake.duration);
    });
    std::array<float, 3> amplitude{0.0f, 0.0f, 0.0f};
    std::array<float, 3> time{0.0f, 0.0f, 0.0f};
    std::array<const Shake*, 3> strongest{nullptr, nullptr, nullptr};
    for (const Active& active : shakes) {
        const float t = (nowSeconds - active.startSeconds) + active.shake.phase;
        const glm::vec3 d = cameraPos - active.origin;
        const auto amp = attenuated(active.shake.amplitude, glm::dot(d, d));
        const uint32_t dir = active.shake.direction;
        if (amp && dir < 3 && amplitude[dir] < *amp) {
            amplitude[dir] = *amp;
            time[dir] = t;
            strongest[dir] = &active.shake;
        }
    }
    glm::vec3 out(0.0f);
    for (uint32_t dir = 0; dir < 3; ++dir) {
        if (!strongest[dir]) continue;
        const float d = displacement(*strongest[dir], amplitude[dir], time[dir]);
        if (dir == 2) {
            out.z += d;
        } else {
            const float a = facingRad + (dir == 1 ? 1.5707964f : 0.0f);
            out.x += std::cos(a) * d;
            out.y += std::sin(a) * d;
        }
    }
    return out;
}

}  // namespace wowee::rendering::camera_shake
