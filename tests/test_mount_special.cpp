// What the jump key does on a mount (game/mount_special.hpp, 0x0072eb80): a
// standing ground mount jumped, and nothing was told of its rearing.
#include <catch_amalgamated.hpp>

#include "game/mount_special.hpp"
#include "game/mover_keys.hpp"

using namespace wowee::game;

namespace {
JumpKeyInput riding(uint32_t moveFlags = 0) {
    JumpKeyInput in;
    in.mounted = true;
    in.moveFlags = moveFlags;
    return in;
}
}  // namespace

TEST_CASE("on foot the key jumps", "[mount_special]") {
    JumpKeyInput in;
    CHECK(jumpKey(in) == JumpKey::Jump);
    in.deep = true;
    in.moveFlags = 0x10;
    CHECK(jumpKey(in) == JumpKey::Jump);
}

TEST_CASE("a mount rears where it stands and jumps as it goes", "[mount_special]") {
    CHECK(jumpKey(riding()) == JumpKey::MountSpecial);
    for (const uint32_t going : {0x1u, 0x2u, 0x4u, 0x8u}) CHECK(jumpKey(riding(going)) == JumpKey::Jump);
    CHECK(jumpKey(riding(0x100)) == JumpKey::MountSpecial);  // walking is how, not that
}

TEST_CASE("turning on the spot or deep in water it does nothing", "[mount_special]") {
    CHECK(jumpKey(riding(0x10)) == JumpKey::Nothing);
    CHECK(jumpKey(riding(0x20)) == JumpKey::Nothing);
    auto in = riding();
    in.deep = true;
    CHECK(jumpKey(in) == JumpKey::Nothing);
    in.moveFlags = 0x1;
    CHECK(jumpKey(in) == JumpKey::Jump);
    CHECK(deepInLiquid(1.1f, 2.0f));
    CHECK_FALSE(deepInLiquid(1.0f, 2.0f));
}

TEST_CASE("one that can fly, or is flagged for it, jumps", "[mount_special]") {
    CHECK(jumpKey(riding(0x1000000)) == JumpKey::Jump);
    auto in = riding();
    in.mountFlags = 0x400;
    CHECK(jumpKey(in) == JumpKey::Jump);
}

TEST_CASE("without gravity, going is no leave to jump", "[mount_special]") {
    auto in = riding(0x1);
    in.floating = true;
    CHECK(jumpKey(in) == JumpKey::MountSpecial);
}

// A dead character walked on another map, where the server's root had gone.
TEST_CASE("the dead are not moved by their keys", "[mover_keys]") {
    using wowee::game::keysMayMove;
    CHECK(keysMayMove(1, 0));
    CHECK(keysMayMove(100, 1));
    CHECK_FALSE(keysMayMove(0, 0));
    CHECK_FALSE(keysMayMove(100, 7));
}
