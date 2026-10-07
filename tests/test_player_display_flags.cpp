// PLAYER_FLAGS' hide-helm (0x400) and hide-cloak (0x800) undress the head and
// back slots of every player's model, as CGPlayer_C does (0x006e0fd0). Other
// players used to show the helm and cloak their owners had hidden.
#include <catch_amalgamated.hpp>

#include "game/player_display_flags.hpp"

namespace pd = wowee::game::player_display;

TEST_CASE("the hide flags clear the head and back slots", "[player_display]") {
    std::array<uint32_t, 19> ids{};
    std::array<uint8_t, 19> types{};
    ids.fill(7);
    types.fill(1);
    pd::applyHidden(0x400, ids, types);
    CHECK(ids[0] == 0);
    CHECK(types[0] == 0);
    CHECK(ids[14] == 7);
    pd::applyHidden(0x800, ids, types);
    CHECK(ids[14] == 0);
    CHECK(types[14] == 0);
    CHECK(ids[4] == 7);
}

TEST_CASE("other flags dress everything", "[player_display]") {
    std::array<uint32_t, 19> ids{};
    std::array<uint8_t, 19> types{};
    ids.fill(3);
    pd::applyHidden(0x20 | 0x10, ids, types);
    for (uint32_t v : ids) CHECK(v == 3);
    CHECK(pd::hideBits(0xffffffffu) == 0xc00u);
    CHECK(pd::hideBits(0x20u) == 0u);
}
