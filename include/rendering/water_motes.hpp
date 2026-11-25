#pragma once

/// What floats in the water about the camera (0x0079e100, 0x0079bf40, 0x0079ca70): four thousand
/// motes of Textures\WaterPoop02.blp in a cube of thirty yards that goes with the camera, where
/// the liquid's LiquidType row says so (flag 0x8). The arithmetic; water_motes_renderer draws it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::rendering::water_motes {

inline constexpr size_t kCount = 4000;
inline constexpr float kSide = 30.0f;          // 0x41f00000
inline constexpr float kSize = 1.0f / 36.0f;   // 0x3ce38e39, times the row's ParticleScale
inline constexpr size_t kMostDrawn = 666;      // 0xa68 vertices

/// A LiquidType row's: its flags (column 2), ParticleScale (11), ParticleMovement (12) and
/// ParticleTexSlots (13).
struct Liquid {
    uint32_t flags = 0;
    float scale = 1.0f;
    uint32_t movement = 0;
    uint32_t slots = 0;

    [[nodiscard]] bool motes() const { return (flags & 0x8) != 0; }
    /// Fogged as the scene is.
    [[nodiscard]] bool fogged() const { return (flags & 0x10) != 0; }
};

struct Mote {
    glm::vec3 at{0.0f};   ///< from the camera
    float size = 0.0f;
};

inline constexpr float kCellSide = 0.19921875f;
/// The picture of a mote, of the twenty-five in the texture: left and top (0x00a3f970, 0x00adf838).
[[nodiscard]] inline glm::vec2 cell(uint32_t slots, size_t mote) {
    static constexpr std::array<std::array<uint8_t, 8>, 4> kSlots = {{{0, 1, 2, 3, 4, 5, 6, 7},
                                                                      {8, 9, 10, 11, 8, 9, 10, 11},
                                                                      {12, 12, 12, 12, 12, 12, 12, 12},
                                                                      {13, 14, 15, 16, 13, 14, 15, 16}}};
    // Three rows of four; then the column beside them.
    const uint32_t at = kSlots[std::min<uint32_t>(slots, 3)][mote & 7];
    if (at < 12) return glm::vec2(static_cast<float>(at % 4), static_cast<float>(at / 4)) * kCellSide;
    return glm::vec2(4.0f, static_cast<float>(at - 12)) * kCellSide;
}

class Swarm {
public:
    explicit Swarm(uint32_t seed = std::random_device{}()) : rng_(seed) {}

    /// Anywhere in the cube, half as large to once and a half (0x0079b8e0).
    void scatter(float scale) {
        motes_.resize(kCount);
        const float size = kSize * scale;
        for (Mote& m : motes_) {
            m.at = glm::vec3(unit(), unit(), unit()) * kSide - glm::vec3(kSide * 0.5f);
            m.size = unit() * size + size * 0.5f;
        }
        turn();
    }
    /// 0x0079bf40: left behind by the camera, and drifting; what goes out on one side comes in
    /// on the other. Scattered anew where the camera has gone farther than the cube is wide.
    void update(float dt, const glm::vec3& camera, const Liquid& liquid) {
        if (motes_.empty() || glm::length(camera - camera_) > kSide) {
            scatter(liquid.scale);
            camera_ = camera;
        }
        const glm::vec3 by = camera_ - camera + drift(dt, liquid.movement);
        camera_ = camera;
        constexpr float half = kSide * 0.5f;
        for (Mote& m : motes_) {
            m.at += by;
            for (int i = 0; i < 3; ++i) {
                if (m.at[i] > half) m.at[i] -= kSide;
                else if (m.at[i] < -half) m.at[i] += kSide;
            }
        }
    }
    [[nodiscard]] const std::vector<Mote>& motes() const { return motes_; }

    /// Whether a mote, in the camera's right, up and ahead, is drawn (0x0079ca70).
    [[nodiscard]] static bool seen(const glm::vec3& inView) {
        return inView.z > 0.0f && std::fabs(inView.x) < inView.z && std::fabs(inView.y) < inView.z;
    }

private:
    float unit() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng_); }
    /// 0x0079bcc0: another way to sway, mostly level.
    void turn() {
        constexpr float kPi = 3.1415927f;
        const float a = (unit() * 2.0f - 1.0f) * kPi, b = (unit() * 2.0f - 1.0f) * kPi;
        direction_ = glm::normalize(glm::vec3(std::cos(b) * std::sin(a), std::sin(b) * std::sin(a),
                                              std::fabs(std::cos(a) * 0.25f) + 1e-6f));
        phase_ = 0.0f;
        rate_ = (unit() + 1.0f) * 0.0125f;
        reach_ = (unit() + 1.0f) * 0.005f;
    }
    /// 0x0079be50. The sway is so much a frame there: of a sixtieth of a second here.
    glm::vec3 drift(float dt, uint32_t movement) {
        if (movement == 1) return {0.0f, 0.0f, -0.02f * dt};
        if (movement == 2) return {0.0f, 0.0f, 0.02f * dt};
        if (movement != 0) return glm::vec3(0.0f);
        phase_ += dt;
        float at = phase_ * rate_;
        if (at > 0.5f) {
            turn();
            at = 0.0f;
        }
        return direction_ * (std::sin(at * 6.2831855f) * reach_ * dt * 60.0f);
    }

    std::vector<Mote> motes_;
    glm::vec3 camera_{0.0f};
    glm::vec3 direction_{0.0f, 0.0f, 1.0f};
    float phase_ = 0.0f, rate_ = 0.0f, reach_ = 0.0f;
    std::mt19937 rng_;
};

}  // namespace wowee::rendering::water_motes
