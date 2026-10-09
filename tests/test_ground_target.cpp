// Casting at a place on the ground: the cast packet's destination targets
// (0x009ab8b0), the circle's radius (0x008019c0, 0x004f66c0, 0x004f8a40) and
// when a spell waits for a place (0x0080c790).
#include <catch_amalgamated.hpp>

#include "test_support.hpp"
#include "core/application.hpp"
#include "game/ground_target.hpp"
#include "game/world_packets.hpp"
#include "rendering/spell_target_circle.hpp"

#include <cstring>
#include <string>

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}

using namespace wowee::game;
namespace gt = wowee::game::ground_target;

namespace {
float readF32(const std::vector<uint8_t>& b, size_t at) {
    float f;
    std::memcpy(&f, b.data() + at, 4);
    return f;
}
}  // namespace

TEST_CASE("CMSG_CAST_SPELL carries the destination as the client writes it", "[ground_target][packet]") {
    auto p = CastSpellPacket::buildDestination(10, 1.5f, -2.25f, 30.0f, 7);
    const auto& b = p.getData();
    REQUIRE(b.size() == 1 + 4 + 1 + 4 + 1 + 12);
    CHECK(b[0] == 7);                                // cast count
    CHECK(wowee::test::readU32(b, 1) == 10u);        // spell
    CHECK(b[5] == 0);                                // cast flags
    CHECK(wowee::test::readU32(b, 6) == 0x40u);      // TARGET_FLAG_DEST_LOCATION only
    CHECK(b[10] == 0);                               // no transport: an empty packed guid
    CHECK(readF32(b, 11) == 1.5f);
    CHECK(readF32(b, 15) == -2.25f);
    CHECK(readF32(b, 19) == 30.0f);
}

TEST_CASE("which spells wait for a place", "[ground_target]") {
    CHECK(gt::wantsLocation(gt::requiredTargets(0x40, 16, false)));   // Blizzard, Flamestrike
    CHECK(gt::wantsLocation(gt::requiredTargets(0x20, 0, false)));
    CHECK_FALSE(gt::wantsLocation(gt::requiredTargets(0x0, 22, false)));  // Consecration: at the caster
    CHECK_FALSE(gt::wantsLocation(gt::requiredTargets(0x2, 6, false)));
    CHECK_FALSE(gt::wantsLocation(gt::requiredTargets(0x40, 89, true)));  // a trajectory picks it
    CHECK(gt::locationFilledByClick(0x60) == 0x20);
    CHECK(gt::locationFilledByClick(0x40) == 0x40);
}

TEST_CASE("the circle's radius", "[ground_target]") {
    const float r[2] = {8.0f, 10.0f};
    const float none[2] = {0.0f, 0.0f};
    CHECK(gt::spellRadius(r, none, 80) == 10.0f);
    const float per[2] = {0.5f, 0.0f};
    CHECK(gt::spellRadius(r, per, 10) == 13.0f);
    // Held to 20 where the place is good, the small circle elsewhere or
    // for a spell with no area.
    CHECK(gt::circleRadius(gt::Placement::Acceptable, 8.0f) == 8.0f);
    CHECK(gt::circleRadius(gt::Placement::Acceptable, 30.0f) == 20.0f);
    CHECK(gt::circleRadius(gt::Placement::Acceptable, 0.0f) == Catch::Approx(1.3888888f));
    CHECK(gt::circleRadius(gt::Placement::Unacceptable, 8.0f) == Catch::Approx(1.3888888f));
}

TEST_CASE("the place against the spell's range", "[ground_target]") {
    CHECK(gt::placement(30.0f * 30.0f, 0.0f, 30.0f) == gt::Placement::Acceptable);
    CHECK(gt::placement(31.0f * 31.0f, 0.0f, 30.0f) == gt::Placement::Unacceptable);
    CHECK(gt::placement(4.0f, 5.0f, 30.0f) == gt::Placement::Unacceptable);
    CHECK(gt::placement(121.0f * 121.0f, 0.0f, 30.0f) == gt::Placement::TooFar);
    CHECK(std::string(gt::cursorFor(gt::Placement::Acceptable)) == "Interface\\Cursor\\Cast.blp");
    CHECK(std::string(gt::cursorFor(gt::Placement::TooFar)) == "Interface\\Cursor\\UnableCast.blp");
}

TEST_CASE("the circle's box is +-r across and +-2 up", "[ground_target]") {
    const auto p = wowee::rendering::spell_target_circle::project(glm::vec3(10.0f, 20.0f, 5.0f), 8.0f);
    REQUIRE(p);
    CHECK(p->boxMin == glm::vec3(2.0f, 12.0f, 3.0f));
    CHECK(p->boxMax == glm::vec3(18.0f, 28.0f, 7.0f));
    const glm::vec4 centre(10.0f, 20.0f, 5.0f, 1.0f);
    CHECK(glm::dot(p->uRow, centre) == Catch::Approx(0.5f));
    CHECK(glm::dot(p->vRow, centre) == Catch::Approx(0.5f));
    CHECK(glm::dot(p->hRow, centre) == Catch::Approx(0.5f));
    CHECK(glm::dot(p->hRow, glm::vec4(10.0f, 20.0f, 7.0f, 1.0f)) == Catch::Approx(1.0f));
}
