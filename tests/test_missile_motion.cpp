// Spell missile motion scripts as the client runs them: the chunk it wraps a
// SpellMissileMotion.dbc body in (0x007587d0), the call with its twelve
// inputs, and what the outputs do to the drawn place and the speed
// (0x00700550, 0x00700350).
#include <catch_amalgamated.hpp>

#include "rendering/missile_motion.hpp"

namespace mm = wowee::rendering::missile_motion;

namespace {
// SpellMissileMotion.dbc row 13 ("Parabola") as 3.3.5 ships it.
constexpr const char* kParabola =
    "local angle = 0\n"
    "local maxMagnitude = startDistance * .15\n"
    "\n"
    "transAngle = angle\n"
    "transMag = (progress * 2) - 1\n"
    "transMag = (1 - (transMag * transMag)) * maxMagnitude\n";
}  // namespace

TEST_CASE("A body is wrapped as the client wraps it", "[missile_motion]") {
    const std::string chunk = mm::wrapScript("transUp = 1");
    CHECK(chunk.rfind("return function(progress, time, missileIndex, missileCount, distanceToFirePos, "
                      "distanceToImpactPos, startDistance, totalDistance, rand1, rand2, rand3, spellID)\n"
                      "local transAngle = 0.0\n",
                      0) == 0);
    CHECK(chunk.find("transUp = 1\nreturn transAngle, transMag, transRight, transFront, transUp, modelYaw, "
                     "modelPitch, modelRoll, speedAbs, speedScalar, speedOffset, scale\nend\n") !=
          std::string::npos);
}

TEST_CASE("A script runs with the missile's inputs", "[missile_motion]") {
    mm::Scripts scripts;
    REQUIRE(scripts.compile(13, kParabola));
    mm::Inputs in;
    in.progress = 0.5;
    in.startDistance = 20.0;
    auto out = scripts.run(13, in);
    REQUIRE(out);
    CHECK(out->transMag == Catch::Approx(3.0));
    in.progress = 0.0;
    out = scripts.run(13, in);
    REQUIRE(out);
    CHECK(out->transMag == Catch::Approx(0.0));
    // Degrees, as WoW's globals take them; and nothing finite is 0.
    REQUIRE(scripts.compile(1, "transRight = sin(90)\ntransUp = 0/0"));
    out = scripts.run(1, in);
    REQUIRE(out);
    CHECK(out->transRight == Catch::Approx(1.0));
    CHECK(out->transUp == 0.0);
    // One that does not compile is no motion; one that raises ends it.
    CHECK_FALSE(scripts.compile(2, "transMag = ("));
    CHECK_FALSE(scripts.has(2));
    REQUIRE(scripts.compile(3, "error('x')"));
    CHECK_FALSE(scripts.run(3, in));
}

TEST_CASE("The outputs move the drawn place in the way's frame", "[missile_motion]") {
    const glm::vec3 fire(0.0f), target(10.0f, 0.0f, 5.0f), path(5.0f, 0.0f, 2.0f);
    mm::Outputs out;
    out.transMag = 2.0;  // angle 0: up
    glm::vec3 p = mm::drawnPosition(path, fire, target, out);
    CHECK(p.z == Catch::Approx(4.0f));
    out = {};
    out.transAngle = 90.0;
    out.transMag = 2.0;
    p = mm::drawnPosition(path, fire, target, out);
    CHECK(p.y == Catch::Approx(2.0f));
    CHECK(p.z == Catch::Approx(2.0f).margin(1e-5));
    out = {};
    out.transRight = 1.0;
    out.transFront = 3.0;
    out.transUp = -1.0;
    p = mm::drawnPosition(path, fire, target, out);
    CHECK(p.x == Catch::Approx(8.0f));
    CHECK(p.y == Catch::Approx(1.0f));
    CHECK(p.z == Catch::Approx(1.0f));
}

TEST_CASE("The outputs change the speed", "[missile_motion]") {
    mm::Outputs out;
    CHECK(mm::speedAfter(20.0f, 20.0f, out) == 20.0f);
    out.speedScalar = 1.5;
    CHECK(mm::speedAfter(20.0f, 20.0f, out) == Catch::Approx(30.0f));
    out = {};
    out.speedAbs = 7.0;
    CHECK(mm::speedAfter(20.0f, 20.0f, out) == Catch::Approx(7.0f));
    out = {};
    out.speedOffset = -50.0;
    CHECK(mm::speedAfter(20.0f, 20.0f, out) == Catch::Approx(0.01f));
}

TEST_CASE("The outputs turn and size the model", "[missile_motion]") {
    mm::Outputs out;
    CHECK(mm::modelTurn(out) == glm::mat4(1.0f));
    // Yaw 90 turns the nose to the client's left, which is this client's -y.
    out.modelYaw = 90.0;
    glm::vec4 nose = mm::modelTurn(out) * glm::vec4(1, 0, 0, 0);
    CHECK(nose.x == Catch::Approx(0.0f).margin(1e-6));
    CHECK(nose.y == Catch::Approx(-1.0f));
    // Pitch 90 about the side axis points the nose down, as the client's does.
    out = {};
    out.modelPitch = 90.0;
    nose = mm::modelTurn(out) * glm::vec4(1, 0, 0, 0);
    CHECK(nose.z == Catch::Approx(-1.0f));
    // A scale at or under 0.01 is 0.01; none at all leaves the size.
    out = {};
    out.scale = 0.001;
    CHECK(mm::modelTurn(out)[0][0] == Catch::Approx(0.01f));
    out.scale = 2.0;
    CHECK(mm::modelTurn(out)[1][1] == Catch::Approx(2.0f));
}
