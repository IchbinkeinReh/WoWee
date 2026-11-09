#include "rendering/collision_hover.hpp"

#include <algorithm>
#include <cmath>

namespace wowee::rendering::collision {

namespace {
constexpr float kEps = 9.536743e-07f;
}  // namespace

HoverProbe hoverProbe(const Mover& m, Sweeper& sweeper) {
    HoverProbe p;
    const float reach = m.hoverHeight + m.hoverHeight;
    const SweepResult s = sweeper.run(m, glm::vec3(0.0f, 0.0f, -1.0f), reach, true);
    p.height = s.facet >= 0 ? s.allowed : reach;
    p.found = s.facet >= 0;
    // (0x0075b6c0, which can raise a height under the hover height to it,
    // is not read; left out.)
    if (p.height < kSkin) p.height = 0.0f;
    return p;
}

bool hoverStep(Mover& m, uint32_t ms, float dist, const glm::vec3& dir, Sweeper& sweeper,
               bool gravityDisabled) {
    if (ms == 0) return true;
    const HoverProbe probe = hoverProbe(m, sweeper);
    if (!probe.found && !gravityDisabled) {
        startFall(m);
        return false;
    }
    const float T = static_cast<float>(ms);
    const float catchUp = T * 0.001f * kHoverCatchUp;
    const float rise = std::clamp(m.hoverHeight - probe.height, -catchUp, catchUp);
    glm::vec3 v(dir.x * dist, dir.y * dist, rise + dir.z * dist);
    float L = glm::length(v);
    if (std::abs(L) < kEps) return true;
    glm::vec3 d = v / L;
    float used = 0.0f;
    int stuck = 0;
    SweepResult s = sweeper.run(m, d, L);
    while (s.facet >= 0) {
        m.pos += d * s.allowed;
        const float frac = (s.allowed / L) * (T - used);
        L -= s.allowed;
        if (frac + kEps <= 1.0f) {
            if (++stuck >= 6) return true;
        } else {
            stuck = 1;
            used += frac;
        }
        if (T - used < 1.0f) return true;
        const Facet f = sweeper.facet(s.facet);
        glm::vec3 turn;
        const glm::vec3 rest = d * L;
        if (!tooSteep(f.n)) {
            // Walkable: pushed off along its normal (0x0075d1c0 true).
            const float k = -glm::dot(f.n, d) * L + kSlidePushOff;
            turn = f.n * k;
        } else {
            turn = slideAlong(d, L, glm::length(glm::vec2(rest)), f.n);
        }
        const glm::vec3 next = rest + turn;
        L = glm::length(next);
        if (std::abs(L) < kEps) return true;
        d = next / L;
        s = sweeper.run(m, d, L);
    }
    m.pos += d * s.allowed;
    return true;
}

}  // namespace wowee::rendering::collision
