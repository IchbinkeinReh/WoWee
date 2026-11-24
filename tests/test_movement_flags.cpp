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

TEST_CASE("a landing goes out where the fall was told of, or was one (0x0073d4a0)", "[movementflags]") {
    using wowee::game::fallLandSent;
    CHECK(fallLandSent(true, 0.0f));
    CHECK_FALSE(fallLandSent(false, 0.0f));
    CHECK_FALSE(fallLandSent(false, 0.027f));
    CHECK(fallLandSent(false, 0.028f));
}

TEST_CASE("a fall starts where the mover may fall (0x0098b710, 0x00988370)", "[movementflags]") {
    using wowee::game::mayStartFall;
    CHECK(mayStartFall(0));
    CHECK(mayStartFall(bit(MovementFlags::FORWARD) | bit(MovementFlags::WALKING) | bit(MovementFlags::CAN_FLY)));
    for (auto f : {MovementFlags::FALLING, MovementFlags::SWIMMING, MovementFlags::FLYING, MovementFlags::LEVITATING,
                   MovementFlags::ROOT, MovementFlags::ONTRANSPORT})
        CHECK_FALSE(mayStartFall(bit(f)));
    // What it takes off, and the pitch's two with it unless flags2 0x20.
    const uint32_t before = bit(MovementFlags::FORWARD) | bit(MovementFlags::ASCENDING) |
                            bit(MovementFlags::PITCH_UP) | bit(MovementFlags::FLYING) | bit(MovementFlags::CAN_FLY);
    const uint32_t after = bit(MovementFlags::FORWARD) | bit(MovementFlags::CAN_FLY) | bit(MovementFlags::FALLING);
    CHECK(wowee::game::fallFlags(before, 0) == after);
    CHECK(wowee::game::fallFlags(before, 0x20) == (after | bit(MovementFlags::PITCH_UP)));
}

TEST_CASE("stopping to swim starts a fall the packet carries (0x0098bff0)", "[movementflags]") {
    MovementInfo info = withFlags(bit(MovementFlags::FORWARD) | bit(MovementFlags::SWIMMING) |
                                  bit(MovementFlags::ASCENDING));
    info.pitch = 0.5f;
    CHECK(wowee::game::stopSwimming(info));
    CHECK(info.flags == (bit(MovementFlags::FORWARD) | bit(MovementFlags::FALLING)));
    CHECK(info.pitch == 0.0f);
    // Not swimming: nothing stops, nothing falls.
    info = withFlags(bit(MovementFlags::FORWARD));
    CHECK_FALSE(wowee::game::stopSwimming(info));
    CHECK(info.flags == bit(MovementFlags::FORWARD));
    // On a transport none starts.
    info = withFlags(bit(MovementFlags::SWIMMING) | bit(MovementFlags::ONTRANSPORT));
    CHECK_FALSE(wowee::game::stopSwimming(info));
    CHECK(info.flags == bit(MovementFlags::ONTRANSPORT));
}

TEST_CASE("a root that comes in a fall holds from the landing on (0x0098c4f0, 0x00988490)", "[movementflags]") {
    MovementInfo info = withFlags(bit(MovementFlags::FORWARD) | bit(MovementFlags::FALLING));
    wowee::game::rootMover(info);
    CHECK_FALSE(info.isPlayerRooted());
    CHECK((info.flags & bit(MovementFlags::PENDING_ROOT)) != 0);
    wowee::game::endFall(info, 100u);
    CHECK(info.flags == bit(MovementFlags::ROOT));
    // On the ground at once, and stopped with it.
    info = withFlags(bit(MovementFlags::FORWARD) | bit(MovementFlags::TURN_LEFT));
    wowee::game::rootMover(info);
    CHECK(info.flags == bit(MovementFlags::ROOT));
    wowee::game::unrootMover(info);
    CHECK(info.flags == 0);
    info = withFlags(bit(MovementFlags::FALLING) | bit(MovementFlags::PENDING_ROOT));
    wowee::game::unrootMover(info);
    CHECK(info.flags == bit(MovementFlags::FALLING));
}

TEST_CASE("being put somewhere ends a fall and keeps what is not the mover's doing (0x0098ba90)", "[movementflags]") {
    MovementInfo info = withFlags(bit(MovementFlags::FORWARD) | bit(MovementFlags::WALKING) |
                                  bit(MovementFlags::FALLING) | bit(MovementFlags::FALLINGFAR) |
                                  bit(MovementFlags::ASCENDING) | bit(MovementFlags::CAN_FLY) |
                                  bit(MovementFlags::WATER_WALK) | bit(MovementFlags::HOVER) |
                                  bit(MovementFlags::FEATHER_FALL) | bit(MovementFlags::ROOT));
    info.pitch = 0.3f;
    wowee::game::placeMover(info, 250u);
    CHECK(info.flags == (bit(MovementFlags::WALKING) | bit(MovementFlags::CAN_FLY) | bit(MovementFlags::WATER_WALK) |
                         bit(MovementFlags::HOVER) | bit(MovementFlags::FEATHER_FALL) | bit(MovementFlags::ROOT)));
    CHECK(info.pitch == 0.0f);
    CHECK(info.fallTime == 250u);
}

TEST_CASE("a knockback's fall has the packet's own jump block (0x006e9ff0)", "[movementflags]") {
    MovementInfo info = withFlags(bit(MovementFlags::FORWARD) | bit(MovementFlags::SWIMMING) |
                                  bit(MovementFlags::CAN_FLY));
    info.pitch = 0.4f;
    wowee::game::knockBackMover(info, 0.6f, -0.8f, 15.0f, -10.0f);
    CHECK(info.flags == (bit(MovementFlags::FORWARD) | bit(MovementFlags::FALLING)));
    CHECK(info.pitch == 0.0f);
    CHECK(info.jumpVelocity == -10.0f);
    CHECK(info.jumpSinAngle == 0.6f);
    CHECK(info.jumpCosAngle == -0.8f);
    CHECK(info.jumpXYSpeed == 15.0f);
    // Rooted: held again from the landing on.
    info = withFlags(bit(MovementFlags::ROOT));
    wowee::game::knockBackMover(info, 1.0f, 0.0f, 5.0f, -5.0f);
    CHECK(info.flags == (bit(MovementFlags::FALLING) | bit(MovementFlags::PENDING_ROOT)));
}

// The jump block is the wire's, and so is the facing its direction goes by:
// north (0 there) is a quarter turn back here, and a jump running north went
// out as one to the east.
TEST_CASE("our own jump goes where the wire's facing points", "[movementflags]") {
    MovementInfo info = withFlags(bit(MovementFlags::FORWARD));
    info.orientation = wowee::core::coords::serverToCanonicalYaw(0.0f);
    auto north = wowee::game::ownJumpBlock(info, 2.5f, 7.0f, 4.5f);
    CHECK(north.dirX == Catch::Approx(1.0f));
    CHECK(north.dirY == Catch::Approx(0.0f).margin(1e-6));
    info.orientation = wowee::core::coords::serverToCanonicalYaw(1.5707963f);
    const auto west = wowee::game::ownJumpBlock(info, 2.5f, 7.0f, 4.5f);
    CHECK(west.dirX == Catch::Approx(0.0f).margin(1e-6));
    CHECK(west.dirY == Catch::Approx(1.0f));
}
