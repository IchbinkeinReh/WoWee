// An elevator's or a tram's turn along its path (TransportRotation.dbc).
//
// 0x0070dc10: with two or more rows for the object's entry, the rows either
// side of the path time slerped (0x00982460) and multiplied by the object's
// GAMEOBJECT_PARENTROTATION (0x004f4320); with fewer, the object keeps its
// own. The rows here are the file's own for entry 194440.
#include <catch_amalgamated.hpp>

#include <cmath>
#include <vector>

#include "game/game_object_rotation.hpp"
#include "game/transport_rotation.hpp"

using namespace wowee::game;

namespace {

// TransportRotation.dbc rows 393-398: entry 194440, times and x y z w.
std::vector<transport_rotation::Key> rows194440() {
    return {
        {0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)},
        {42933, glm::quat(0.9996839761734009f, 0.0f, 0.0f, 0.025143999606370926f)},
        {48033, glm::quat(0.9248849749565125f, 0.0f, 0.0f, 0.38024699687957764f)},
        {52533, glm::quat(0.48375698924064636f, 0.0f, 0.0f, 0.875203013420105f)},
        {56200, glm::quat(0.04246800020337105f, 0.0f, 0.0f, 0.999098002910614f)},
        {105300, glm::quat(-0.0020580000709742308f, 0.0f, 0.0f, 0.9999979734420776f)},
    };
}

float facingOf(const glm::quat& q) { return game_object_rotation::facing(q); }

const glm::quat kIdentity(1.0f, 0.0f, 0.0f, 0.0f);

}  // namespace

TEST_CASE("a transport with rows turns between them", "[transport_rotation]") {
    const auto keys = rows194440();
    const float from = 2.0f * std::atan2(0.025144f, 0.999684f);
    const float to = 2.0f * std::atan2(0.380247f, 0.924885f);

    // On a row, the row.
    const auto atRow = transport_rotation::rotationAt(keys, 42933, 120000, kIdentity);
    REQUIRE(atRow);
    CHECK(facingOf(*atRow) == Catch::Approx(from).margin(1e-5));

    // Half way between two rows, half way round: a slerp about one axis
    // turns evenly.
    const auto mid = transport_rotation::rotationAt(keys, (42933 + 48033) / 2, 120000, kIdentity);
    REQUIRE(mid);
    CHECK(facingOf(*mid) == Catch::Approx((from + to) / 2.0f).margin(1e-4));

    // From the first row at 0, a little of the way to the second.
    const auto early = transport_rotation::rotationAt(keys, 20000, 120000, kIdentity);
    REQUIRE(early);
    CHECK(facingOf(*early) == Catch::Approx(from * 20000.0f / 42933.0f).margin(1e-5));
}

TEST_CASE("the parent rotation turns it besides", "[transport_rotation]") {
    // An object placed at 1.0 (GAMEOBJECT_PARENTROTATION about z): its turn
    // along the path is on top of that.
    const auto keys = rows194440();
    const glm::quat parent = game_object_rotation::fromFacing(1.0f);
    const auto atRow = transport_rotation::rotationAt(keys, 48033, 120000, parent);
    REQUIRE(atRow);
    const float row = 2.0f * std::atan2(0.380247f, 0.924885f);
    CHECK(facingOf(*atRow) == Catch::Approx(row + 1.0f).margin(1e-4));

    // Rows that do not turn leave it at its parent's, not its own placing.
    const std::vector<transport_rotation::Key> still = {{0, kIdentity}, {4000, kIdentity}, {28000, kIdentity}};
    const auto held = transport_rotation::rotationAt(still, 10000, 30000, parent);
    REQUIRE(held);
    CHECK(facingOf(*held) == Catch::Approx(1.0f).margin(1e-5));
}

TEST_CASE("past the last row it holds the last row", "[transport_rotation]") {
    // The last segment runs to the path's end and is weighted against the
    // first row's time taken unsigned, so it barely moves off the last row.
    const auto keys = rows194440();
    const auto late = transport_rotation::rotationAt(keys, 110000, 120000, kIdentity);
    REQUIRE(late);
    CHECK(std::abs(std::abs(glm::dot(*late, keys.back().rotation)) - 1.0f) < 1e-5f);
}

TEST_CASE("one row or none and it keeps its own", "[transport_rotation]") {
    CHECK_FALSE(transport_rotation::rotationAt({}, 0, 1000, kIdentity));
    CHECK_FALSE(transport_rotation::rotationAt({{0, kIdentity}}, 0, 1000, kIdentity));
}

TEST_CASE("the slerp goes the short way and stops at a near-equal pair", "[transport_rotation]") {
    const glm::quat a = game_object_rotation::fromFacing(0.2f);
    // b's negation is the same turn: the slerp takes it the short way.
    const glm::quat b = -game_object_rotation::fromFacing(0.6f);
    CHECK(facingOf(transport_rotation::slerp(a, b, 0.5f)) == Catch::Approx(0.4f).margin(1e-5));
    // Within 2^-21 of each other: a, as it is.
    const glm::quat c(0.0f, 0.0f, 0.0f, 1.0f);
    const glm::quat same = transport_rotation::slerp(c, -c, 0.7f);
    CHECK(same == c);
}
