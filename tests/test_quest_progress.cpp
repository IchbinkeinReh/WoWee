#include <catch_amalgamated.hpp>

#include "game/quest_progress.hpp"

using wowee::game::decodeQuestObjectiveCounts;
using wowee::game::normalizeQuestObjectiveEntry;
using wowee::game::isQuestSlotComplete;
using wowee::game::questObjectiveCountFieldOffset;
using wowee::game::questObjectiveLine;

TEST_CASE("Classic quest counters use four packed 6-bit values", "[quest][progress]") {
    const uint32_t packed = 5u | (12u << 6) | (31u << 12) | (63u << 18)
                          | (1u << 24); // state byte must not leak into counts
    REQUIRE(decodeQuestObjectiveCounts(3, packed) ==
            std::array<uint32_t, 4>{5, 12, 31, 63});
    REQUIRE(questObjectiveCountFieldOffset(3) == 1);
}

TEST_CASE("TBC quest counters use four byte values", "[quest][progress]") {
    const uint32_t packed = 7u | (16u << 8) | (42u << 16) | (200u << 24);
    REQUIRE(decodeQuestObjectiveCounts(4, packed) ==
            std::array<uint32_t, 4>{7, 16, 42, 200});
    REQUIRE(questObjectiveCountFieldOffset(4) == 2);
}

TEST_CASE("WotLK quest counters use four uint16 values", "[quest][progress]") {
    const uint32_t first = 5u | (300u << 16);
    const uint32_t second = 16u | (1024u << 16);
    REQUIRE(decodeQuestObjectiveCounts(5, first, second) ==
            std::array<uint32_t, 4>{5, 300, 16, 1024});
    REQUIRE(questObjectiveCountFieldOffset(5) == 2);
}

TEST_CASE("Quest update game-object entries discard the wire marker", "[quest][progress]") {
    REQUIRE(normalizeQuestObjectiveEntry(1234u) == 1234u);
    REQUIRE(normalizeQuestObjectiveEntry(0x80000000u | 5678u) == 5678u);
}

TEST_CASE("Classic completion state does not alias the first kill count", "[quest][progress]") {
    const uint32_t oneKillNotComplete = 1u;
    const uint32_t oneKillComplete = 1u | (1u << 24);
    REQUIRE_FALSE(isQuestSlotComplete(3, oneKillNotComplete));
    REQUIRE(isQuestSlotComplete(3, oneKillComplete));
    REQUIRE(isQuestSlotComplete(4, 1u));
    REQUIRE(isQuestSlotComplete(5, 1u));
}

// The bit beside completion, in the same field.
//
// The server names them QUEST_STATE_COMPLETE = 0x0001 and QUEST_STATE_FAIL =
// 0x0002 (Player.h:619). The complete bit was read for as long as the quest
// log has existed and the fail bit next to it was dropped, so a timed quest
// that ran out looked exactly like one still running - and the tracker, which
// writes `if ( isComplete and isComplete < 0 )`, had no way to reach that
// branch.
//
// Both strides, because Classic packs the state into the top byte of a shared
// field and TBC/WotLK give it one of its own; reading the wrong end answers
// zero for everything, which is indistinguishable from nothing being failed.
TEST_CASE("A failed quest slot reads as failed, not as unfinished",
          "[quest][progress]") {
    SECTION("WotLK and TBC keep the state in its own field") {
        const uint8_t stride = 5;
        CHECK_FALSE(wowee::game::isQuestSlotFailed(stride, 0x0));
        CHECK_FALSE(wowee::game::isQuestSlotFailed(stride, 0x1));   // complete
        CHECK(wowee::game::isQuestSlotFailed(stride, 0x2));         // failed
        CHECK(wowee::game::isQuestSlotFailed(stride, 0x3));         // both
        // And completion still reads the way it did.
        CHECK(wowee::game::isQuestSlotComplete(stride, 0x1));
        CHECK_FALSE(wowee::game::isQuestSlotComplete(stride, 0x2));
        CHECK(wowee::game::isQuestSlotComplete(stride, 0x3));
    }
    SECTION("Classic packs it into the top byte") {
        const uint8_t stride = 3;
        CHECK_FALSE(wowee::game::isQuestSlotFailed(stride, 0x00000002));
        CHECK(wowee::game::isQuestSlotFailed(stride, 0x02000000));
        CHECK(wowee::game::isQuestSlotComplete(stride, 0x01000000));
        CHECK(wowee::game::isQuestSlotFailed(stride, 0x03000000));
    }
}

namespace {
// The interface's strings as a German client has them (GlobalStrings.lua);
// anything not listed answers its enUS wording, as interfaceString does.
std::string germanText(const char* key, const char* enUS) {
    const std::string k = key;
    if (k == "QUEST_MONSTERS_KILLED") return "%1$s getötet: %2$d/%3$d";
    if (k == "QUEST_OBJECTS_FOUND") return "%1$s: %2$d/%3$d";
    if (k == "QUEST_ITEMS_NEEDED") return "%1$s: %2$d/%3$d";
    if (k == "QUEST_PLAYERS_KILLED") return "Spieler besiegt: %d/%d";
    if (k == "QUEST_FACTION_NEEDED") return "%1$s: %2$s / %3$s";
    return enUS;
}
std::string englishText(const char*, const char* enUS) { return enUS; }
}  // namespace

// An objective line names what it is about, in the interface's words
// (GetQuestLogLeaderBoard, 0x005e1460): QUEST_MONSTERS_KILLED for a creature,
// QUEST_OBJECTS_FOUND for a game object.
TEST_CASE("A kill objective is named, counted and worded by the interface", "[quest][objective]") {
    REQUIRE(questObjectiveLine("", "Nightbane Worgen", false, 12, 15, englishText) ==
            "Nightbane Worgen slain: 12/15");
    REQUIRE(questObjectiveLine("", "Nachtsichel-Worg", false, 12, 15, germanText) ==
            "Nachtsichel-Worg getötet: 12/15");
    REQUIRE(questObjectiveLine("", "Bundle of Wood", true, 0, 8, englishText) == "Bundle of Wood: 0/8");
}

TEST_CASE("The quest's own objective text names the slot instead", "[quest][objective]") {
    // ObjectiveText, in QUEST_OBJECTS_FOUND even for a creature.
    REQUIRE(questObjectiveLine("Verseuchte Brut befreit", "Verseuchte Brut", false, 2, 6, germanText) ==
            "Verseuchte Brut befreit: 2/6");
}

TEST_CASE("An objective with no name yet still counts, named by a space", "[quest][objective]") {
    // The client puts a single space where the name is not known yet.
    REQUIRE(questObjectiveLine("", "", false, 3, 8, englishText) == "  slain: 3/8");
    REQUIRE(questObjectiveLine("", "", true, 3, 8, englishText) == " : 3/8");
}

TEST_CASE("Items, players and reputation objectives", "[quest][objective]") {
    // Never more than needed is shown held.
    REQUIRE(wowee::game::questItemObjectiveLine("Wolfsfleisch", 9, 8, germanText) == "Wolfsfleisch: 8/8");
    REQUIRE(wowee::game::questItemObjectiveLine("", 1, 8, germanText) == " : 1/8");
    REQUIRE(wowee::game::questPlayersKilledLine(3, 10, germanText) == "Spieler besiegt: 3/10");
    REQUIRE(wowee::game::questFactionLine("Kirin Tor", "Freundlich", "Wohlwollend", germanText) ==
            "Kirin Tor: Freundlich / Wohlwollend");
}
