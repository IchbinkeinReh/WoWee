// A ship's sway (TransportPhysics.dbc; 0x007f7dd0, 0x007f7b30).
#include <catch_amalgamated.hpp>

#include "game/transport_physics.hpp"

#include <cmath>

namespace tp = wowee::game::transport_physics;

namespace {
// Row 21 as 3.3.5a ships it.
const tp::Physics kRow21{3.3f, 1.1f, 2.0f, 2.5f, 0.88f, 1.9f, 2.0f, 3.5f, 25.0f, 0.1f};
}  // namespace

TEST_CASE("slow ships make no speed, easing in to 30% of the threshold", "[transport_physics]") {
    CHECK(tp::effectiveSpeed(kRow21, 0.0f) == 0.0f);
    CHECK(tp::effectiveSpeed(kRow21, 3.0f) == 0.0f);          // under 3.75
    CHECK(tp::effectiveSpeed(kRow21, 5.625f) == Catch::Approx(0.25f * 5.625f));
    CHECK(tp::effectiveSpeed(kRow21, 7.5f) == Catch::Approx(7.5f));
    CHECK(tp::effectiveSpeed(kRow21, 30.0f) == 30.0f);
}

TEST_CASE("the turn rate sums the heading changes behind", "[transport_physics]") {
    // A steady 0.1 rad per 0.45 s turn.
    const float h[5] = {0.4f, 0.3f, 0.2f, 0.1f, 0.0f};
    CHECK(tp::turnRate(h) == Catch::Approx(4 * 0.1f * 2.2222223f * 0.2f));
    // Across the +-pi seam it is still small.
    const float seam[5] = {-3.1f, 3.1f, 3.1f, 3.1f, 3.1f};
    CHECK(std::fabs(tp::turnRate(seam)) < 0.1f);
}

TEST_CASE("the sway at speed and at rest", "[transport_physics]") {
    // At full speed, time 0: the wave at its zero, the roll and pitch at
    // their phases.
    tp::Sway s = tp::sway(kRow21, 0, 30.0f, 0.0f);
    CHECK(s.lift == Catch::Approx(0.0f).margin(1e-5));
    CHECK(s.roll == Catch::Approx(std::sin(0.5f) * 2.0f));
    CHECK(s.pitch == Catch::Approx(std::sin(0.2f) * 0.88f));
    // A quarter of the wave's period on: the full lift.
    const auto period = static_cast<uint32_t>(std::lround(6283.1855f / 1.1f));
    s = tp::sway(kRow21, period / 4, 30.0f, 0.0f);
    CHECK(s.lift == Catch::Approx(3.3f).epsilon(0.01));
    // At rest everything is SpeedDamp of it.
    const tp::Sway still = tp::sway(kRow21, period / 4, 0.0f, 0.0f);
    CHECK(still.lift == Catch::Approx(3.3f * 0.1f).epsilon(0.01));
    // A turn leans the roll, clamped at MaxBankTurnSpeed.
    const tp::Sway lean = tp::sway(kRow21, 0, 30.0f, 10.0f);
    CHECK(lean.roll == Catch::Approx(std::sin(0.5f) * 2.0f + 2.0f));
    // A zero time scale leaves that part out.
    tp::Physics flat = kRow21;
    flat.pitchTimeScale = 0.0f;
    CHECK(tp::sway(flat, 0, 30.0f, 0.0f).pitch == 0.0f);
}
