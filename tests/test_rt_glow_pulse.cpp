// A glow card's pulse for its traced light (rt_glow_pulse.hpp): a bone that
// only turns and moves gives 1; one scaled by s draws its card at s^2; a
// bone pulses when its own or a parent's scale track has more than one key.
#include <catch_amalgamated.hpp>

#include "rendering/rt_glow_pulse.hpp"

#include <glm/gtc/matrix_transform.hpp>

using namespace wowee;
using namespace wowee::rendering::rt_lights;

TEST_CASE("Bone area factor", "[rt_glow_pulse]") {
    glm::mat4 m = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(3, 4, 5)), 0.7f, glm::vec3(0, 0, 1));
    REQUIRE(boneAreaFactor(m) == Catch::Approx(1.0f));
    REQUIRE(boneAreaFactor(glm::scale(m, glm::vec3(1.12f))) == Catch::Approx(1.12f * 1.12f).epsilon(1e-4));
    REQUIRE(boneAreaFactor(glm::mat4(0.0f)) == Catch::Approx(1.0f));
}

TEST_CASE("Pulsing bones follow their parents", "[rt_glow_pulse]") {
    std::vector<pipeline::M2Bone> bones(3);
    bones[0].parentBone = -1;
    bones[1].parentBone = 0;
    bones[2].parentBone = -1;
    pipeline::M2AnimationTrack::SequenceKeys keys;
    keys.timestamps = {0, 233, 500};
    keys.vec3Values = {glm::vec3(1), glm::vec3(1.12f), glm::vec3(0.96f)};
    bones[0].scale.sequences.push_back(keys);
    const auto p = pulsingBones(bones);
    REQUIRE(p[0] == 1);
    REQUIRE(p[1] == 1);
    REQUIRE(p[2] == 0);
}
