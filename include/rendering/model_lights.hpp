#pragma once

/// The one light a model is drawn with, of those it is given (CM2Lighting).

#include <array>
#include <cmath>
#include <glm/glm.hpp>

namespace wowee::rendering::model_lights {

/// What a lit surface gets: a light along a direction, and the ambient.
struct Lighting {
    glm::vec3 direction{0.0f, 0.0f, -1.0f};
    glm::vec3 diffuse{0.0f};
    glm::vec3 ambient{0.0f};
};

/// The directional lights given, as they are merged (0x00834dc0, 0x00835280): the direction is
/// theirs by how bright each is, the light what they send along it and a quarter more of what
/// they send elsewhere taken off it - and that into the ambient. A vector longer than 1 is the
/// more light for it.
struct Sum {
    std::array<glm::vec3, 3> along{glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f)};   // +0x18, a channel each
    glm::vec3 byBrightness{0.0f};                                                       // +0x3c
    glm::vec3 colour{0.0f};                                                             // +0x48
    glm::vec3 ambient{0.0f};                                                            // +0x54

    /// 0x00834d90
    void addAmbient(const glm::vec3& c) { ambient += c; }
    /// 0x00834dc0
    void addDirectional(const glm::vec3& c, const glm::vec3& travels) {
        for (int i = 0; i < 3; ++i) along[static_cast<size_t>(i)] += travels * c[i];
        byBrightness += travels * (c.r * 0.212671f + c.g * 0.71516f + c.b * 0.072169f);
        colour += c;
    }
    /// 0x00835280
    [[nodiscard]] Lighting merged() const {
        Lighting out;
        if (glm::dot(byBrightness, byBrightness) > 1e-5f) out.direction = glm::normalize(byBrightness);
        const glm::vec3 sent(glm::dot(out.direction, along[0]), glm::dot(out.direction, along[1]),
                             glm::dot(out.direction, along[2]));
        out.diffuse = sent * 1.25f - colour * 0.25f;
        out.ambient = ambient + (colour - sent) * 0.25f;
        return out;
    }
};

/// What a point light of M2 sends as far as `distance` (0x008353d0; 0x00834a40's 0, 0.7, 0.03).
[[nodiscard]] inline float pointReach(float distance) {
    return distance < 1e-5f ? 0.0f : 1.0f / (0.7f * distance + 0.03f * distance * distance);
}

/// A unit's portrait (0x00616bc0), in its model's space.
[[nodiscard]] inline Lighting portrait() {
    Sum sum;
    sum.addAmbient(glm::vec3(0.45f));
    sum.addDirectional(glm::vec3(1.0f), glm::vec3(-1.0f, 0.0f, -1.0f));
    return sum.merged();
}

}  // namespace wowee::rendering::model_lights
