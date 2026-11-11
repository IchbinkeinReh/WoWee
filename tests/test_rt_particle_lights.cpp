// The split of an emitter's particle light (rt_particle_lights.hpp): the
// strengths sum to the one light's, the youngest the strongest; the ages
// space evenly over the lifespan, N = 1 at half of it; the mean path in
// closed form agrees with the client's step taken finely.
#include <catch_amalgamated.hpp>

#include "rendering/m2_particle_motion.hpp"
#include "rendering/rt_particle_lights.hpp"

#include <cmath>

using namespace wowee::rendering;
using namespace wowee::rendering::rt_lights;

TEST_CASE("Particle light weights sum to one and fall", "[rt_particle_lights]") {
    for (int n = 1; n <= kMaxParticleLights; ++n) {
        const auto w = particleLightWeights(n);
        REQUIRE(static_cast<int>(w.size()) == n);
        float sum = 0.0f;
        for (size_t i = 0; i < w.size(); ++i) {
            sum += w[i];
            if (i > 0) REQUIRE(w[i] < w[i - 1]);
        }
        REQUIRE(sum == Catch::Approx(1.0f).margin(1e-5));
    }
    REQUIRE(particleLightWeights(0).size() == 1);
    REQUIRE(particleLightWeights(42).size() == static_cast<size_t>(kMaxParticleLights));
}

TEST_CASE("Particle light ages space evenly over the life", "[rt_particle_lights]") {
    REQUIRE(particleLightAge(0, 1, 2.0f) == Catch::Approx(1.0f));
    REQUIRE(particleLightAge(0, 3, 4.0f) == Catch::Approx(1.0f));
    REQUIRE(particleLightAge(1, 3, 4.0f) == Catch::Approx(2.0f));
    REQUIRE(particleLightAge(2, 3, 4.0f) == Catch::Approx(3.0f));
}

TEST_CASE("Mean path matches the client's step", "[rt_particle_lights]") {
    for (float drag : {0.0f, 0.8f, 3.0f}) {
        const glm::vec3 v0(1.0f, 0.5f, 2.0f);
        const float g = 1.5f;
        m2_particle::StepParams p;
        p.gravity = g;
        p.drag = drag;
        glm::vec3 pos(0.0f), vel = v0;
        const float dt = 1.0f / 2000.0f;
        float t = 0.0f;
        while (t < 1.2f - 1e-6f) {
            m2_particle::step(pos, vel, t, dt, p);
            t += dt;
        }
        const glm::vec3 d = meanDisplacement(v0, g, drag, 1.2f);
        REQUIRE(d.x == Catch::Approx(pos.x).margin(5e-3));
        REQUIRE(d.y == Catch::Approx(pos.y).margin(5e-3));
        REQUIRE(d.z == Catch::Approx(pos.z).margin(5e-3));
    }
}

TEST_CASE("Mean launch of a plane emitter leans by its range", "[rt_particle_lights]") {
    const MeanLaunch up = meanLaunch(1, 2.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, false);
    REQUIRE(up.velocity.z == Catch::Approx(2.0f));
    const MeanLaunch wide = meanLaunch(1, 2.0f, 3.14159265f, 0.0f, 1.0f, 1.0f, 0.0f, false);
    REQUIRE(std::fabs(wide.velocity.z) < 1e-3f);
}
