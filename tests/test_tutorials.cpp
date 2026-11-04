// The tutorial state, as 0x00530100..0x00530920 keep it.
#include <catch_amalgamated.hpp>

#include "game/tutorials.hpp"

using wowee::game::TutorialState;

TEST_CASE("the server's flags fill both sets", "[tutorials]") {
    TutorialState t;
    std::array<uint32_t, TutorialState::kWords> words{};
    words[0] = 0x5;  // ids 0 and 2
    t.loadFlags(words);
    CHECK(t.flagged(0));
    CHECK_FALSE(t.flagged(1));
    CHECK(t.flagged(2));
    CHECK(t.anyTriggered());
    CHECK_FALSE(t.flag(2));  // already flagged: nothing to send
    CHECK(t.trigger(0, true) == TutorialState::Trigger::Nothing);
}

TEST_CASE("flagging is once, and the session list keeps the order seen", "[tutorials]") {
    TutorialState t;
    CHECK(t.flag(7));
    CHECK_FALSE(t.flag(7));
    CHECK(t.flag(3));
    CHECK(t.flag(12));
    CHECK(t.nextSeen(7) == 4);   // 3, counted from 1
    CHECK(t.nextSeen(3) == 13);
    CHECK(t.nextSeen(12) == 0);
    CHECK(t.prevSeen(12) == 4);
    CHECK(t.prevSeen(7) == 0);
}

TEST_CASE("a trigger pops out once, or is flagged with tutorials off", "[tutorials]") {
    TutorialState t;
    CHECK(t.trigger(5, true) == TutorialState::Trigger::Nothing);  // no flags from the server yet
    t.loadFlags({});
    CHECK(t.trigger(5, true) == TutorialState::Trigger::Popup);
    CHECK(t.trigger(5, true) == TutorialState::Trigger::Nothing);
    CHECK_FALSE(t.flagged(5));  // triggered is not flagged
    CHECK(t.trigger(6, false) == TutorialState::Trigger::Flag);
}

TEST_CASE("clear sets everything, reset empties everything", "[tutorials]") {
    TutorialState t;
    CHECK(t.flag(1));
    t.setAll();
    CHECK(t.flagged(59));
    CHECK(t.nextSeen(1) == 0);
    CHECK(t.prevSeen(1) == 0);
    t.reset();
    CHECK_FALSE(t.flagged(1));
    CHECK_FALSE(t.anyTriggered());
    CHECK(t.flag(1));
}

TEST_CASE("an item pushed to the player asks for its tutorials in the client's order", "[tutorials]") {
    using wowee::game::itemPushTutorials;
    wowee::game::ItemPushTutorialInput in;
    in.self = true;
    in.itemId = 6948;  // the Hearthstone: a use spell, class 15
    in.itemClass = 15;
    in.hasUseSpell = true;
    in.bagSlot = 0xff;
    in.slot = 23;
    in.playerLevel = 1;
    CHECK(itemPushTutorials(in) == std::vector<uint32_t>{7, 8, 0x1e});

    // Bread after level 1: no use tutorial, the food ones.
    in.itemId = 4540;
    in.itemClass = 0;
    in.subClass = 5;
    in.playerLevel = 2;
    CHECK(itemPushTutorials(in) == std::vector<uint32_t>{7, 10, 11});

    // A key on the key ring.
    in = {};
    in.self = true;
    in.itemClass = 13;
    in.bagSlot = 0xff;
    in.slot = 86;
    CHECK(itemPushTutorials(in) == std::vector<uint32_t>{7, 0x31});

    // A bag that can be worn, once tutorial 0x37 was seen.
    in = {};
    in.self = true;
    in.itemClass = 1;
    in.inventoryType = 18;
    CHECK(itemPushTutorials(in) == std::vector<uint32_t>{7, 9});
    in.equipTutorialSeen = true;
    CHECK(itemPushTutorials(in) == std::vector<uint32_t>{7, 0x17, 9});
}

TEST_CASE("a ranged weapon asks for 0x2b unless the player is a hunter; a wand always", "[tutorials]") {
    using wowee::game::itemPushTutorials;
    wowee::game::ItemPushTutorialInput in;
    in.self = true;
    in.itemClass = 2;
    in.subClass = 2;  // a bow
    in.inventoryType = 15;
    in.playerClass = 1;
    CHECK(itemPushTutorials(in) == std::vector<uint32_t>{7, 0x2b});
    in.playerClass = 3;
    CHECK(itemPushTutorials(in) == std::vector<uint32_t>{7});
    in = {};
    in.itemClass = 2;
    in.subClass = 0x13;
    in.inventoryType = 26;
    CHECK(itemPushTutorials(in) == std::vector<uint32_t>{0x2b});
}

TEST_CASE("one awaits only once the flags are in and until it is triggered", "[tutorials]") {
    TutorialState t;
    CHECK_FALSE(t.awaiting(0x12));
    t.loadFlags({});
    CHECK(t.awaiting(0x12));
    (void)t.trigger(0x12, true);
    CHECK_FALSE(t.awaiting(0x12));
}

TEST_CASE("more than ten attacks in a row after tutorial 5 trigger 0x3b", "[tutorials]") {
    wowee::game::AttackStreak s;
    for (int i = 0; i < 20; ++i) CHECK_FALSE(s.note(false, true));  // 5 not flagged: armed at nought
    for (int i = 0; i < 10; ++i) CHECK_FALSE(s.note(true, true));
    CHECK(s.note(true, true));                                       // the eleventh
    wowee::game::AttackStreak t;
    for (int i = 0; i < 5; ++i) CHECK_FALSE(t.note(true, true));
    CHECK_FALSE(t.note(true, false));                                // another spell disarms it
    for (int i = 0; i < 20; ++i) CHECK_FALSE(t.note(true, true));
}
