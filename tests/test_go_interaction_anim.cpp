// The interaction object's OPEN and CLOSE by game object type (0x0070bab0,
// 0x0070bac0 and the type handlers' slots 0x98 and 0x9c).
#include <catch_amalgamated.hpp>

#include "game/go_interaction_anim.hpp"
#include "network/packet.hpp"
#include "rendering/animation/animation_ids.hpp"

#include <vector>

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

TEST_CASE("Open and Close, Destroy and Rebuild reverse each other part-way", "[go_interaction]") {
    // 0x0070d510's pairs; nothing else starts part-way.
    CHECK(reversesTransition(kStateOpen, kStateClose));
    CHECK(reversesTransition(kStateClose, kStateOpen));
    CHECK(reversesTransition(kStateDestroy, kStateRebuild));
    CHECK(reversesTransition(kStateRebuild, kStateDestroy));
    CHECK_FALSE(reversesTransition(kStateOpened, kStateClose));
    CHECK_FALSE(reversesTransition(1, kStateOpen));
    CHECK_FALSE(reversesTransition(kStateOpen, kStateDestroy));
    CHECK_FALSE(reversesTransition(kStateCustom0, kStateClose));
}

TEST_CASE("A reversed transition starts at what is left of the other", "[go_interaction]") {
    // 0x0070d010: (1 - elapsed / length) * 65535, rounded.
    CHECK(reversedStartProgress(0.0f, 1000.0f, 1.0f) == uint16_t{65535});
    CHECK(reversedStartProgress(250.0f, 1000.0f, 1.0f) == uint16_t{49151});   // 49151.25
    CHECK(reversedStartProgress(500.0f, 1000.0f, 1.0f) == uint16_t{32768});   // 32767.5, to even
    CHECK(reversedStartProgress(1000.0f, 1000.0f, 1.0f) == uint16_t{0});
    // Its speed scales elapsed and length alike.
    CHECK(reversedStartProgress(250.0f, 1000.0f, 2.0f) == uint16_t{49151});
    // Past its end, held (speed 0) or of no length: from the start.
    CHECK_FALSE(reversedStartProgress(1000.5f, 1000.0f, 1.0f).has_value());
    CHECK_FALSE(reversedStartProgress(300.0f, 1000.0f, 0.0f).has_value());
    CHECK_FALSE(reversedStartProgress(0.0f, 0.0f, 1.0f).has_value());

    // 0x0070d1e0: duration * progress / 65535 into the new one.
    CHECK(progressStartTime(65535, 2000) == 2000u);
    CHECK(progressStartTime(0, 2000) == 0u);
    CHECK(progressStartTime(49151, 2000) == 1500u);
    // Open three quarters done; a Close of 3000 ms starts a quarter in.
    const auto p = reversedStartProgress(1500.0f, 2000.0f, 1.0f);
    REQUIRE(p.has_value());
    CHECK(progressStartTime(*p, 3000) == 750u);
}

TEST_CASE("Which states hold an object the server takes away", "[go_interaction]") {
    // 0x0070d1e0: all but Closed, Opened and Destroyed.
    for (uint8_t s : {0, 2, 4, 5, 7, 8, 9, 10, 11, 12}) CHECK(stateHoldsObject(s));
    for (uint8_t s : {1, 3, 6}) CHECK_FALSE(stateHoldsObject(s));
}

TEST_CASE("Which types play the server's custom animations", "[go_interaction]") {
    // Slot 0x4c of the handler 0x00714250 builds: 0x0070c480 ...
    for (uint32_t type : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 10u, 12u, 16u, 17u, 18u, 19u,
                          22u, 23u, 24u, 25u, 26u, 27u, 29u, 30u, 34u}) {
        CHECK(playsCustomAnimation(type));
    }
    // ... or 0x00632050, which does nothing - the base handler's (0x00a33618)
    // for the types it has no case for among them.
    for (uint32_t type : {4u, 7u, 11u, 13u, 14u, 15u, 20u, 21u, 28u, 31u, 32u, 33u, 35u, 36u, 255u}) {
        CHECK_FALSE(playsCustomAnimation(type));
    }
}

TEST_CASE("SMSG_GAMEOBJECT_CUSTOM_ANIM's index", "[go_interaction]") {
    // 0x0070be90: under 4, 0x00a335e8[index + 2] - Custom0..3.
    CHECK(customAnimState(0) == std::optional<uint8_t>(kStateCustom0));
    CHECK(customAnimState(3) == std::optional<uint8_t>(kStateCustom0 + 3));
    // Anything else is not played.
    CHECK_FALSE(customAnimState(4).has_value());
    CHECK_FALSE(customAnimState(100).has_value());
}

TEST_CASE("Only Custom0..3 return to the server's pose as they end", "[go_interaction]") {
    // 0x0070d7e0: states 8..0xb call slot 0x30; Despawn (12) has no case,
    // and the model is left playing its sequence on.
    for (uint8_t state = kStateCustom0; state < kStateDespawn; ++state) {
        CHECK(customAnimEndRestoresPose(state));
    }
    CHECK_FALSE(customAnimEndRestoresPose(kStateDespawn));
    CHECK_FALSE(customAnimEndRestoresPose(kStateOpen));
    CHECK_FALSE(customAnimEndRestoresPose(kStateClose));
    // Its hold is let go all the same (0x004d77e0): a Despawn taken away
    // meanwhile then goes.
    CHECK(stateHoldsObject(kStateDespawn));
}

TEST_CASE("Replacing the interaction object closes the window open with the old one", "[go_interaction]") {
    constexpr uint64_t npc = 0xF130000123000001ull, other = 0xF130000456000002ull;
    std::array<uint64_t, kWindowCount> with{};
    auto at = [&](Window w) -> uint64_t& { return with[static_cast<size_t>(w)]; };
    // Nothing was set, or nothing is open with it: nothing closes.
    CHECK_FALSE(windowClosedWith(with, 0).has_value());
    CHECK_FALSE(windowClosedWith(with, npc).has_value());
    // Gossip with the NPC, then its merchant opens: the gossip closes
    // (0x00512e60 -> 0x0058a550).
    at(Window::Gossip) = npc;
    at(Window::Merchant) = npc;
    CHECK(windowClosedWith(with, npc, Window::Merchant) == std::optional<Window>(Window::Gossip));
    // Only the first in 0x00512e60's order: a trade before a book before
    // gossip ...
    at(Window::Trade) = npc;
    CHECK(windowClosedWith(with, npc, Window::Merchant) == std::optional<Window>(Window::Trade));
    // ... and never the one being opened, which wowee has already given its
    // object.
    at(Window::Trade) = 0;
    CHECK(windowClosedWith(with, npc, Window::Gossip) == std::optional<Window>(Window::Merchant));
    // A window open with another object stays.
    at(Window::Gossip) = other;
    at(Window::Merchant) = 0;
    CHECK_FALSE(windowClosedWith(with, npc, Window::Merchant).has_value());
    CHECK(windowClosedWith(with, other) == std::optional<Window>(Window::Gossip));
}

TEST_CASE("Which trade statuses end the trade the window is set with", "[go_interaction]") {
    // 0x007044a0 clears the partner and who asked for these ...
    for (uint32_t status : {0u, 3u, 5u, 6u, 8u, 10u, 11u, 12u, 13u, 14u, 15u, 18u, 21u}) {
        CHECK(tradeStatusEndsTrade(status));
    }
    // ... and keeps them for the request, the window opening, an accept and
    // its withdrawal, and the rest.
    for (uint32_t status : {1u, 2u, 4u, 7u, 9u, 22u, 23u}) {
        CHECK_FALSE(tradeStatusEndsTrade(status));
    }
}

TEST_CASE("A TrinityCore / AzerothCore fishing bite plays the bobber's Custom0", "[go_interaction]") {
    // GameObject::Update, FISHINGNODE, once the bobber is ready:
    // SendCustomAnim(GetGoAnimProgress()) - `data << GetGUID(); data <<
    // uint32(anim);` - and GameObject::Create sets a fishing node's animation
    // progress to 0. The bobber here: HighGuid::GameObject (0xF110), entry
    // 35591 (Fishing Bobber, 0x8B07), counter 0x123.
    const std::vector<uint8_t> body = {
        0x23, 0x01, 0x00, 0x07, 0x8b, 0x00, 0x10, 0xf1,  // guid 0xF110008B07000123
        0x00, 0x00, 0x00, 0x00,                          // anim 0
    };
    wowee::network::Packet packet(0x00B3, body);   // SMSG_GAMEOBJECT_CUSTOM_ANIM
    // 0x0070be90: the guid (0x0047b400, 8 bytes), the index (0x0047b3c0, 4).
    REQUIRE(packet.getSize() == 12);
    CHECK(packet.readUInt64() == 0xF110008B07000123ull);
    const auto state = customAnimState(packet.readUInt32());
    // 0x00a335e8[0 + 2] = 8, Custom0; 0x00ada938[8] = AnimationData 153,
    // which type 17's slot 0x4c (0x0070c480) plays if the model has it.
    REQUIRE(state.has_value());
    CHECK(*state == kStateCustom0);
    CHECK(wowee::rendering::anim::CUSTOM_0 == 153u);
    CHECK(playsCustomAnimation(17));
}

TEST_CASE("A trade request with a partner set, or within a second", "[go_interaction]") {
    // 0x00703cf0: a partner (0xca0fe8) refuses it - ERR_ALREADY_TRADING ...
    CHECK(tradeInitiate(0x42, 5000, 0) == TradeInitiate::AlreadyTrading);
    CHECK(tradeInitiate(0x42, 5000, 9000) == TradeInitiate::AlreadyTrading);
    // ... the first goes, and the next a second on (0xca0ff8), not before.
    CHECK(tradeInitiate(0, 5000, 0) == TradeInitiate::Send);
    CHECK(tradeInitiate(0, 5999, 6000) == TradeInitiate::TooSoon);
    CHECK(tradeInitiate(0, 6000, 6000) == TradeInitiate::Send);
    // The clock's wrap, as the client's signed difference takes it.
    CHECK(tradeInitiate(0, 100, 0xFFFFFF00u) == TradeInitiate::Send);
    CHECK(tradeInitiate(0, 0xFFFFFF00u, 100) == TradeInitiate::TooSoon);
}

TEST_CASE("A door collides only in Closed", "[go_interaction]") {
    // 0x0070d8d0, type 0's state entry: +0x20c = (state == Closed).
    CHECK(collidesAfterEntering(0, 1) == std::optional<bool>(true));
    for (uint8_t s : {0, 2, 3, 4, 5, 6, 7}) CHECK(collidesAfterEntering(0, s) == std::optional<bool>(false));
    for (uint8_t s = kStateCustom0; s <= kStateDespawn; ++s)
        CHECK(collidesAfterEntering(0, s) == std::optional<bool>(false));
    // Other types' entry (0x0070d510) leaves it.
    for (uint32_t type : {1u, 3u, 5u, 35u}) CHECK_FALSE(collidesAfterEntering(type, kStateCustom0).has_value());
}
