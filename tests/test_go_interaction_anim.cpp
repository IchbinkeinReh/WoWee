// The interaction object's OPEN and CLOSE by game object type (0x0070bab0,
// 0x0070bac0 and the type handlers' slots 0x98 and 0x9c).
#include <catch_amalgamated.hpp>

#include "game/go_interaction_anim.hpp"

using namespace wowee::game::go_interaction;

TEST_CASE("A book or text object opens and closes", "[go_interaction]") {
    // 0x0070b560 / 0x0070b570: states 2 and 4, whatever it is in.
    auto open = onOpen(9, nullptr);
    CHECK(open.action == Action::Enter);
    CHECK(open.state == kStateOpen);
    for (uint8_t cur : {0, 1, 2, 3, 4, 8}) {
        auto close = onClose(9, nullptr, cur);
        CHECK(close.action == Action::Enter);
        CHECK(close.state == kStateClose);
    }
}

TEST_CASE("A quest giver object without a custom animation", "[go_interaction]") {
    uint32_t data[24] = {};
    // 0x0070fe10: Open, unless the model already shows it.
    for (const uint32_t* d : {static_cast<const uint32_t*>(data), static_cast<const uint32_t*>(nullptr)}) {
        auto open = onOpen(2, d);
        CHECK(open.action == Action::EnterUnlessShowingOpen);
        CHECK(open.state == kStateOpen);
        // 0x0070ff20: Close only from Open or Opened.
        CHECK(onClose(2, d, kStateOpen).action == Action::Enter);
        CHECK(onClose(2, d, kStateOpen).state == kStateClose);
        CHECK(onClose(2, d, kStateOpened).action == Action::Enter);
        CHECK(onClose(2, d, 1).action == Action::None);
        CHECK(onClose(2, d, kStateClose).action == Action::None);
        CHECK(onClose(2, d, 0).action == Action::None);
    }
}

TEST_CASE("A quest giver object's customAnim is data4", "[go_interaction]") {
    // Type 2's fields by kind (0x00a36f34): 0x16 is the fifth.
    uint32_t data[24] = {};
    data[3] = 7;    // gossipID: not it
    CHECK(onOpen(2, data).action == Action::EnterUnlessShowingOpen);
    data[4] = 1;
    // 0x0070bde0: 0x00a335e8[customAnim + 1] - Custom0 for 1 ... Despawn for 5.
    // With the model loading, 0x0070bde0 notes it in +0x208 for 0x00713f50.
    auto open = onOpen(2, data);
    CHECK(open.action == Action::PlayCustom);
    CHECK(open.state == kStateCustom0);
    data[4] = 4;
    CHECK(onOpen(2, data).state == kStateCustom0 + 3);
    data[4] = 5;
    CHECK(onOpen(2, data).state == kStateDespawn);
    data[4] = 6;
    CHECK(onOpen(2, data).action == Action::None);
    // With one, closing does nothing, whatever state it is in.
    data[4] = 1;
    CHECK(onClose(2, data, kStateOpen).action == Action::None);
    CHECK(onClose(2, data, kStateOpened).action == Action::None);
}

TEST_CASE("A mailbox and a guild vault play Custom0 on open only", "[go_interaction]") {
    // 0x0070c9f0 in slot 0x98, nothing in 0x9c.
    for (uint32_t type : {19u, 34u}) {
        auto open = onOpen(type, nullptr);
        CHECK(open.action == Action::EnterIfModelHas);
        CHECK(open.state == kStateCustom0);
        CHECK(onClose(type, nullptr, kStateCustom0).action == Action::None);
    }
}

TEST_CASE("Every other type does nothing", "[go_interaction]") {
    // 0x005eeb70 in both slots.
    for (uint32_t type : {0u, 1u, 3u, 5u, 8u, 10u, 22u, 33u, 35u}) {
        CHECK(onOpen(type, nullptr).action == Action::None);
        CHECK(onClose(type, nullptr, kStateOpen).action == Action::None);
    }
}

TEST_CASE("Which windows make their object the interaction object", "[go_interaction]") {
    constexpr uint64_t npc = 0xF130000123000001ull, other = 0xF130000456000002ull;
    constexpr uint64_t self = 0x0000000000000042ull;
    // A merchant, a bank, a flight map...: whatever was open before.
    CHECK(windowSetsInteraction(npc, 0));
    CHECK(windowSetsInteraction(npc, other));
    // 0x005a11a0: the stable master the stable is open with already does not.
    CHECK_FALSE(windowSetsInteraction(npc, npc));
    // 0x005940e0: nor the trainer open already, nor the player itself.
    CHECK(windowSetsInteraction(npc, other, self));
    CHECK_FALSE(windowSetsInteraction(npc, npc, self));
    CHECK_FALSE(windowSetsInteraction(self, 0, self));
}
