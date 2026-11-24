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

// The character list's equipment, shown until the items are known, is by slot.
TEST_CASE("the player's own model hides them before its items are known too", "[player_display]") {
    CHECK(pd::hideBits(true, true) == 0u);
    CHECK(pd::hideBits(false, true) == 0x400u);
    CHECK(pd::hideBits(true, false) == 0x800u);
    std::vector<wowee::game::EquipmentItem> listed;
    for (uint32_t s = 0; s < 23; ++s)
        listed.push_back({.displayModel = 100 + s, .inventoryType = static_cast<uint8_t>(s + 1), .enchantment = 0u});
    const auto worn = pd::wornByPlayer(false, {}, {}, listed, pd::hideBits(false, false));
    REQUIRE(worn.size() == 23);
    CHECK(worn[0].displayModel == 0u);
    CHECK(worn[14].displayModel == 0u);
    CHECK(worn[1].displayModel == 101u);
    CHECK(pd::wornByPlayer(false, {}, {}, listed, 0u)[0].displayModel == 100u);
    // Known, and nothing left once hidden: nothing, and not the list again.
    CHECK(pd::wornByPlayer(true, {}, {}, listed, pd::hideBits(false, false)).empty());
}
