// What the movement flag word means.
//
// GameHandler and MovementHandler each spelled these out against their own
// copy of the flags, seven predicates twice, which is one definition of
// "flying" in two places. They now both ask MovementInfo, and this pins what
// it answers.
//
// Nothing here raises when it is wrong. A predicate reading the wrong bit
// picks the wrong animation, or lets the ground checks run while the player is
// in the air, and the report that comes back is that the character sometimes
// swims through a hillside.
#include <catch_amalgamated.hpp>

#include "game/mover_mode.hpp"
#include "game/world_packets.hpp"

using wowee::game::MovementFlags;
using wowee::game::MovementInfo;

namespace {

MovementInfo withFlags(uint32_t flags) {
    MovementInfo info;
    info.flags = flags;
    return info;
}

constexpr uint32_t bit(MovementFlags f) { return static_cast<uint32_t>(f); }

}  // namespace

TEST_CASE("nothing set means none of them are true", "[movementflags]") {
    const MovementInfo info;
    CHECK_FALSE(info.isPlayerRooted());
    CHECK_FALSE(info.isGravityDisabled());
    CHECK_FALSE(info.isFeatherFalling());
    CHECK_FALSE(info.isWaterWalking());
    CHECK_FALSE(info.isHovering());
    CHECK_FALSE(info.isSwimming());
    CHECK_FALSE(info.isPlayerFlying());
}

TEST_CASE("each single-flag predicate reads its own bit", "[movementflags]") {
    CHECK(withFlags(bit(MovementFlags::ROOT)).isPlayerRooted());
    CHECK(withFlags(bit(MovementFlags::LEVITATING)).isGravityDisabled());
    CHECK(withFlags(bit(MovementFlags::FEATHER_FALL)).isFeatherFalling());
    CHECK(withFlags(bit(MovementFlags::WATER_WALK)).isWaterWalking());
    CHECK(withFlags(bit(MovementFlags::HOVER)).isHovering());
    CHECK(withFlags(bit(MovementFlags::SWIMMING)).isSwimming());
}

TEST_CASE("a predicate answers only for its own bit", "[movementflags]") {
    // Every other flag set, and each predicate still says no. This is what
    // catches a predicate pointed at the wrong constant: with one flag at a
    // time, a wrong bit that happens to be clear looks correct.
    const uint32_t everythingElse =
        ~(bit(MovementFlags::ROOT) | bit(MovementFlags::LEVITATING) |
          bit(MovementFlags::FEATHER_FALL) | bit(MovementFlags::WATER_WALK) |
          bit(MovementFlags::HOVER) | bit(MovementFlags::SWIMMING));
    const MovementInfo info = withFlags(everythingElse);

    CHECK_FALSE(info.isPlayerRooted());
    CHECK_FALSE(info.isGravityDisabled());
    CHECK_FALSE(info.isFeatherFalling());
    CHECK_FALSE(info.isWaterWalking());
    CHECK_FALSE(info.isHovering());
    CHECK_FALSE(info.isSwimming());
}

TEST_CASE("flying wants both flags, not either", "[movementflags]") {
    // CAN_FLY alone is permission: a player mounted on a flying mount and
    // standing on the ground has it. FLYING alone shows up mid-transition.
    // Either one on its own is not flying.
    CHECK_FALSE(withFlags(bit(MovementFlags::CAN_FLY)).isPlayerFlying());
    CHECK_FALSE(withFlags(bit(MovementFlags::FLYING)).isPlayerFlying());
    CHECK(withFlags(bit(MovementFlags::CAN_FLY) | bit(MovementFlags::FLYING))
              .isPlayerFlying());
}

TEST_CASE("predicates do not interfere with each other", "[movementflags]") {
    // Swimming while feather falling is a real combination, and reading one
    // must not depend on the other.
    const MovementInfo info = withFlags(bit(MovementFlags::SWIMMING) |
                                        bit(MovementFlags::FEATHER_FALL));
    CHECK(info.isSwimming());
    CHECK(info.isFeatherFalling());
    CHECK_FALSE(info.isPlayerFlying());
    CHECK_FALSE(info.isPlayerRooted());
}

TEST_CASE("starting to swim ends a fall and a flight, and keeps how long it fell (0x00989660)", "[movementflags]") {
    MovementInfo info = withFlags(bit(MovementFlags::FORWARD) | bit(MovementFlags::FALLING) |
                                  bit(MovementFlags::FALLINGFAR) | bit(MovementFlags::CAN_FLY) |
                                  bit(MovementFlags::FLYING));
    info.jumpVelocity = -7.955547f;
    info.jumpXYSpeed = 14.0f;
    wowee::game::startSwimming(info, 1439u);
    CHECK(info.flags == (bit(MovementFlags::FORWARD) | bit(MovementFlags::CAN_FLY) | bit(MovementFlags::SWIMMING)));
    CHECK(info.fallTime == 1439u);
    CHECK(info.jumpVelocity == 0.0f);
    CHECK(info.jumpXYSpeed == 0.0f);
    // Waded in: the last fall's time is left as it was.
    info = withFlags(bit(MovementFlags::BACKWARD));
    info.fallTime = 700;
    wowee::game::startSwimming(info, std::nullopt);
    CHECK(info.flags == (bit(MovementFlags::BACKWARD) | bit(MovementFlags::SWIMMING)));
    CHECK(info.fallTime == 700u);
}

TEST_CASE("taking to the air ends a fall and a swim (0x00989890)", "[movementflags]") {
    MovementInfo info = withFlags(bit(MovementFlags::FORWARD) | bit(MovementFlags::ASCENDING) |
                                  bit(MovementFlags::FALLING) | bit(MovementFlags::CAN_FLY));
    info.jumpVelocity = -9.096748f;
    wowee::game::startFlying(info, 12u);
    CHECK(info.flags == (bit(MovementFlags::FORWARD) | bit(MovementFlags::ASCENDING) | bit(MovementFlags::CAN_FLY) |
                         bit(MovementFlags::FLYING)));
    CHECK(info.fallTime == 12u);
    CHECK(info.jumpVelocity == 0.0f);
    info = withFlags(bit(MovementFlags::SWIMMING) | bit(MovementFlags::CAN_FLY));
    wowee::game::startFlying(info, std::nullopt);
    CHECK(info.flags == (bit(MovementFlags::CAN_FLY) | bit(MovementFlags::FLYING)));
    // Not allowed to fly: nothing.
    info = withFlags(bit(MovementFlags::FALLING));
    wowee::game::startFlying(info, 12u);
    CHECK(info.flags == bit(MovementFlags::FALLING));
}
