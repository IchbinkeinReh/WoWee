#include "rendering/collision_swim.hpp"

#include <cmath>

#include "rendering/collision_fall.hpp"

namespace wowee::rendering::collision {

namespace {
constexpr float kEps = 9.536743e-07f;
}

bool risingInJump(const Mover& m) {
    return m.falling && m.fallV0 != 0.0f &&
           static_cast<float>(m.fallMs) * 0.001f < m.fallV0 * -(1.0f / kGravity);
}

bool updateSwimming(Mover& m, const std::optional<float>& liquidLevel, bool canSwim, bool gravityDisabled) {
    const float depth = liquidLevel ? *liquidLevel - m.pos.z : 0.0f;
    const float start = m.height * kSwimDepthShare;
    const float stop = start - kSwimStopSlack;
    if (!m.swimming) {
        if (depth > start && canSwim && !risingInJump(m)) {
            // 0x00989660: swimming, not flying, not stepping. A fall is
            // left as it is; swimming wins over it in the step.
            m.swimming = true;
            m.flying = false;
            m.stepping = false;
            return true;
        }
        return false;
    }
    if (!m.flying && (!liquidLevel || depth < stop || !canSwim)) {
        // 0x0098bff0: no longer swimming; dropped from here unless gravity
        // is off for it.
        m.swimming = false;
        if (!gravityDisabled) startFall(m);
        return true;
    }
    return false;
}

bool swimFlyStep(Mover& m, uint32_t ms, float dist, const glm::vec3& dirIn, Sweeper& solid, Sweeper* liquid,
                 bool gravityDisabled, bool ascending) {
    if (ms == 0 || std::abs(dist) < kEps) return false;
    const float T = static_cast<float>(ms);
    float used = 0.0f;
    int stuck = 0;
    bool hadSolid = false;
    glm::vec3 d = dirIn;
    float L = dist;
    SweepResult s = solid.run(m, d, L);
    for (;;) {
        Sweeper* list = &solid;
        if (s.facet < 0) {
            // Nothing solid: a swimmer's move is also swept against the
            // liquid's surface, with a body three quarters as tall
            // (0x0075f9d0 with 0xadba54), until it first meets something
            // solid in this step.
            if (!hadSolid && !gravityDisabled && liquid && m.swimming) {
                Mover short_ = m;
                short_.height = m.height * kLiquidBodyShare;
                s = liquid->run(short_, d, L);
                list = liquid;
            }
            if (s.facet < 0) {
                m.pos += d * s.allowed;
                return false;
            }
        } else {
            hadSolid = true;
        }
        m.pos += d * s.allowed;
        const float frac = (s.allowed / L) * (T - used);
        const float remL = L - s.allowed;
        if (frac + kEps <= 1.0f) {
            if (++stuck > 5) return false;
        } else {
            stuck = 1;
            used += frac;
        }
        if (!hadSolid && ascending) {
            // Rising into the surface: out of the water by a jump (0x009883f0
            // at the swimmer's speed; 0x00988370 clears swimming).
            startFall(m);
            m.fallV0 = kSwimJumpSpeed;
            m.swimming = false;
            return true;
        }
        if (T - used < 1.0f) return false;
        const glm::vec3 n = list->facet(s.facet).n;
        const float k = -glm::dot(n, d) * remL + kSlidePushOff;
        const glm::vec3 v = d * remL + n * k;
        L = glm::length(v);
        if (L < kEps) return false;
        d = v / L;
        s = solid.run(m, d, L);
    }
}

}  // namespace wowee::rendering::collision
