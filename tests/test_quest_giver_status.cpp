// What the server says an NPC has for you, and the mark that goes over it.
//
// The numbers below are DIALOG_STATUS_* from the server's own QuestDef.h, read
// off an AzerothCore checkout rather than inferred from what the client did
// with them. This client named six of the eleven and mislabelled two: 7 was
// "available, low level" where the server means available-with-reputation, and
// the real low-level-available is 2.
//
// A status with no name matched no branch in any of the four places that draw
// the mark, so nothing was drawn - and 2, 3 and 4 are exactly what an NPC
// reports once its quests are below your level.
#include <catch_amalgamated.hpp>

#include <string>

#include "game/quest_giver_status.hpp"

using wowee::game::QuestGiverStatus;
using wowee::game::questGiverMarker;

namespace {
QuestGiverStatus wire(uint8_t v) { return static_cast<QuestGiverStatus>(v); }
std::string symbolOf(uint8_t v) {
    const auto m = questGiverMarker(wire(v));
    return m.symbol ? m.symbol : "";
}
}  // namespace

TEST_CASE("the values are the server's DIALOG_STATUS numbers", "[questgiver]") {
    CHECK(static_cast<uint8_t>(QuestGiverStatus::NONE) == 0);
    CHECK(static_cast<uint8_t>(QuestGiverStatus::UNAVAILABLE) == 1);
    CHECK(static_cast<uint8_t>(QuestGiverStatus::LOW_LEVEL_AVAILABLE) == 2);
    CHECK(static_cast<uint8_t>(QuestGiverStatus::LOW_LEVEL_REWARD_REP) == 3);
    CHECK(static_cast<uint8_t>(QuestGiverStatus::LOW_LEVEL_AVAILABLE_REP) == 4);
    CHECK(static_cast<uint8_t>(QuestGiverStatus::INCOMPLETE) == 5);
    CHECK(static_cast<uint8_t>(QuestGiverStatus::REWARD_REP) == 6);
    // 7 is available-with-reputation. It was named AVAILABLE_LOW here, which
    // is what 2 is, so a rep-gated quest was drawn grey and an out-levelled
    // one was drawn not at all.
    CHECK(static_cast<uint8_t>(QuestGiverStatus::AVAILABLE_REP) == 7);
    CHECK(static_cast<uint8_t>(QuestGiverStatus::AVAILABLE) == 8);
    CHECK(static_cast<uint8_t>(QuestGiverStatus::REWARD2) == 9);
    CHECK(static_cast<uint8_t>(QuestGiverStatus::REWARD) == 10);
}

TEST_CASE("every status the server can send draws something or nothing on purpose",
          "[questgiver]") {
    // The bug was silence: a value with no branch drew no mark, and the player
    // saw an NPC with nothing over its head. Only 0 and 1 mean nothing.
    for (uint8_t v = 0; v <= 10; ++v) {
        INFO("status " << static_cast<int>(v));
        const auto m = questGiverMarker(wire(v));
        if (v == 0 || v == 1) {
            CHECK(m.symbol == nullptr);
        } else {
            REQUIRE(m.symbol != nullptr);
            CHECK(m.tooltip != nullptr);
        }
    }
}

TEST_CASE("an exclamation mark means there is something to take", "[questgiver]") {
    CHECK(symbolOf(8) == "!");   // available
    CHECK(symbolOf(7) == "!");   // available, reputation gated
    CHECK(symbolOf(2) == "!");   // available, below your level
    CHECK(symbolOf(4) == "!");   // available, below your level, rep gated

    SECTION("and the low-level ones are grey") {
        CHECK_FALSE(questGiverMarker(wire(8)).dim);
        CHECK_FALSE(questGiverMarker(wire(7)).dim);
        CHECK(questGiverMarker(wire(2)).dim);
        CHECK(questGiverMarker(wire(4)).dim);
    }
}

TEST_CASE("a question mark means something is in progress or ready", "[questgiver]") {
    CHECK(symbolOf(10) == "?");  // ready to turn in
    CHECK(symbolOf(9) == "?");   // ready, no minimap dot
    CHECK(symbolOf(6) == "?");   // ready, reputation
    CHECK(symbolOf(3) == "?");   // ready, below your level
    CHECK(symbolOf(5) == "?");   // started, not finished

    SECTION("in progress is grey, ready is gold") {
        CHECK(questGiverMarker(wire(5)).dim);
        CHECK_FALSE(questGiverMarker(wire(10)).dim);
        CHECK_FALSE(questGiverMarker(wire(9)).dim);
        CHECK(questGiverMarker(wire(3)).dim);
    }
}

TEST_CASE("status 9 draws the same ? as 10", "[questgiver]") {
    CHECK(std::string(questGiverMarker(wire(9)).symbol) ==
          std::string(questGiverMarker(wire(10)).symbol));
}

TEST_CASE("a status the server never sends draws nothing", "[questgiver]") {
    // DIALOG_STATUS_SCRIPTED_NO_STATUS is 0x1000 and does not fit a uint8, so
    // the wire cannot carry it; anything above 10 is a value this client does
    // not know and must not guess at.
    CHECK(questGiverMarker(wire(11)).symbol == nullptr);
    CHECK(questGiverMarker(wire(255)).symbol == nullptr);
}

namespace {
std::string modelOf(uint8_t v, bool trivial = false) {
    const char* m = wowee::game::questGiverMarkerModel(wire(v), trivial);
    return m ? m : "";
}
}  // namespace

TEST_CASE("the mark over the head is the client's TalkToMe model for the status",
          "[questgiver]") {
    // 0x00745da0 through 0x00a34f5c into 0x00adac98.
    CHECK(modelOf(0).empty());
    CHECK(modelOf(1) == "Interface\\Buttons\\TalkToMeGrey.m2");
    CHECK(modelOf(5) == "Interface\\Buttons\\TalkToMeQuestion_Grey.m2");
    CHECK(modelOf(6) == "Interface\\Buttons\\TalkToMeQuestion_LTBlue.m2");
    CHECK(modelOf(7) == "Interface\\Buttons\\TalkToMeBlue.m2");
    CHECK(modelOf(8) == "Interface\\Buttons\\TalkToMe.m2");
    CHECK(modelOf(9) == "Interface\\Buttons\\TalkToMeQuestionMark.m2");
    CHECK(modelOf(10) == "Interface\\Buttons\\TalkToMeQuestionMark.m2");
    CHECK(modelOf(11).empty());
}

TEST_CASE("the low-level marks hang only while trivial quests are tracked", "[questgiver]") {
    // 0x0057bf30 gates statuses 2 to 4.
    CHECK(modelOf(2).empty());
    CHECK(modelOf(3).empty());
    CHECK(modelOf(4).empty());
    CHECK(modelOf(2, true) == "Interface\\Buttons\\TalkToMe.m2");
    CHECK(modelOf(3, true) == "Interface\\Buttons\\TalkToMeQuestion_LTBlue.m2");
    CHECK(modelOf(4, true) == "Interface\\Buttons\\TalkToMeBlue.m2");
    // The others do not depend on it.
    CHECK(modelOf(8, true) == modelOf(8));
    CHECK(modelOf(1, true) == modelOf(1));
}

// When the marks are asked for again (game/quest_status_requery.hpp). The
// server answers for the player as they are and never repeats itself: a Horde
// GM who reached Goldshire before `.gm on` kept the "nothing" an enemy is
// told, and the costumed orphan matron stood there with no mark.
#include "game/quest_status_requery.hpp"

namespace rq = wowee::game::quest_status_requery;

TEST_CASE("an available quest under GM mode is drawn as the yellow mark", "[questgiver]") {
    // What the server answered for the matron with .gm on.
    CHECK(wowee::game::questGiverMarkerModel(wire(8), false) != nullptr);
    CHECK(symbolOf(8) == "!");
    // And what it answers an enemy: nothing to draw, which is why the stale
    // answer was invisible.
    CHECK(wowee::game::questGiverMarkerModel(wire(0), false) == nullptr);
}

TEST_CASE(".gm on and off ask the questgivers again", "[questgiver]") {
    CHECK(rq::gmFlagChanged(0x0, rq::kPlayerFlagGm));
    CHECK(rq::gmFlagChanged(rq::kPlayerFlagGm | 0x20, 0x20));
    // Other player flags (resting, PvP) change nothing the server answers here.
    CHECK_FALSE(rq::gmFlagChanged(0x20, 0x220));
    CHECK_FALSE(rq::gmFlagChanged(rq::kPlayerFlagGm, rq::kPlayerFlagGm | 0x200));
}

TEST_CASE("a change of the player's faction asks the questgivers again", "[questgiver]") {
    // GM mode makes the player friendly (template 35) to everyone.
    CHECK(rq::playerFactionChanged(1610, 35));
    CHECK_FALSE(rq::playerFactionChanged(1610, 1610));
}

TEST_CASE("a unit that becomes a questgiver is asked once", "[questgiver]") {
    CHECK(rq::becameQuestGiver(0x0, 0x2));
    CHECK(rq::becameQuestGiver(0x80, 0x82));
    CHECK_FALSE(rq::becameQuestGiver(0x2, 0x3));   // already one
    CHECK_FALSE(rq::becameQuestGiver(0x2, 0x0));   // stops being one
    CHECK_FALSE(rq::becameQuestGiver(0x1, 0x81));  // never one
}
