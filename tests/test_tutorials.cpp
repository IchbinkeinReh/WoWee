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
