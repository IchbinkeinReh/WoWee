// SMSG_CHANGEPLAYER_DIFFICULTY_RESULT as the client reads it (0x0052c460):
// which result is which GlobalStrings error, and how the cooldown's time left
// is worded (0x0061a9e0 with "INT_SPELL_DURATION", rounding up).
#include <catch_amalgamated.hpp>

#include <string>

#include "game/chat_channels.hpp"
#include "game/difficulty_change.hpp"

namespace dc = wowee::game::difficulty_change;

TEST_CASE("each failure is its own error, and the rest none", "[difficulty]") {
    CHECK(std::string(dc::errorKey(1)) == "ERR_DIFFICULTY_CHANGE_COOLDOWN_S");
    CHECK(std::string(dc::errorKey(2)) == "ERR_DIFFICULTY_CHANGE_WORLDSTATE");
    CHECK(std::string(dc::errorKey(3)) == "ERR_DIFFICULTY_CHANGE_ENCOUNTER");
    CHECK(std::string(dc::errorKey(4)) == "ERR_DIFFICULTY_CHANGE_COMBAT");
    CHECK(std::string(dc::errorKey(5)) == "ERR_DIFFICULTY_CHANGE_PLAYER_BUSY");
    CHECK(std::string(dc::errorKey(7)) == "ERR_DIFFICULTY_CHANGE_ALREADY_STARTED");
    // 0 stores the difficulty, 6 starts the change, 8 prints a
    // MapDifficulty.dbc message, 9 is the change done.
    for (uint32_t quiet : {0u, 6u, 8u, 9u, 10u}) CHECK(dc::errorKey(quiet) == nullptr);
}

TEST_CASE("the cooldown is worded in the largest unit, rounded up", "[difficulty]") {
    auto words = [](uint32_t seconds) {
        const auto d = dc::intDuration(seconds * 1000u);
        return std::string(d.suffix) + " " + std::to_string(d.value);
    };
    CHECK(words(0) == "_SEC 0");
    CHECK(words(59) == "_SEC 59");
    CHECK(words(60) == "_MIN 1");
    CHECK(words(61) == "_MIN 2");
    CHECK(words(3599) == "_HOURS 1");  // 60 minutes rounded up is an hour
    CHECK(words(3600) == "_HOURS 1");
    CHECK(words(3601) == "_HOURS 2");
    CHECK(words(86399) == "_DAYS 1");  // 24 hours is a day
    CHECK(words(86400) == "_DAYS 1");
    CHECK(words(86401) == "_DAYS 2");
}

TEST_CASE("the enUS fallback fills the cooldown into the error", "[difficulty]") {
    using wowee::game::chat_channels::luaFormat;
    const auto left = dc::intDuration(300 * 1000u);
    const std::string duration =
        luaFormat(dc::englishText((std::string("INT_SPELL_DURATION") + left.suffix).c_str()),
                  {std::to_string(left.value)});
    CHECK(duration == "5 min");
    CHECK(luaFormat(dc::englishText(dc::errorKey(1)), {duration}) ==
          "Raid difficulty has changed recently, and may not change again for 5 min.");
    CHECK(luaFormat(dc::englishText("ERR_PLAYER_DIFFICULTY_CHANGED_S"),
                    {dc::englishText("PLAYER_DIFFICULTY2")}) == "Difficulty set to Heroic.");
}

TEST_CASE("only a raid leader on a dynamic raid map may change it", "[difficulty]") {
    // 0x0052cbc0: Map.dbc InstanceType 2 with Flags 0x100, a raid group, and
    // the player its leader.
    constexpr uint64_t me = 0x10, other = 0x20;
    constexpr uint32_t raid = 2, party = 1, dyn = dc::kMapFlagDynamicDifficulty;
    CHECK(dc::mayChange(raid, dyn, true, me, me));
    CHECK(dc::mayChange(raid, dyn | 0x1, true, me, me));
    CHECK_FALSE(dc::mayChange(party, dyn, true, me, me));  // a dungeon
    CHECK_FALSE(dc::mayChange(raid, 0, true, me, me));     // a fixed raid
    CHECK_FALSE(dc::mayChange(raid, dyn, false, me, me));  // not in a raid group
    CHECK_FALSE(dc::mayChange(raid, dyn, true, other, me));
    CHECK_FALSE(dc::mayChange(raid, dyn, true, 0, 0));     // no leader at all
}

TEST_CASE("the cooldown is when it ends, not how long it has left", "[difficulty]") {
    // 0xbd1984 holds time() plus result 6's seconds; 0 is none.
    constexpr int64_t now = 1'700'000'000;
    CHECK_FALSE(dc::coolingDown(0, now));
    CHECK(dc::coolingDown(now + 1, now));
    CHECK(dc::coolingDown(now + 300, now));
    CHECK_FALSE(dc::coolingDown(now, now));  // over the second it ends
    CHECK_FALSE(dc::coolingDown(now - 5, now));
}

TEST_CASE("a console line splits at its first space", "[difficulty]") {
    // 0x007658a0 skips the spaces before it; 0x00768900 trims the argument.
    auto line = dc::splitConsoleLine("  changeplayerdifficulty   1  ");
    CHECK(line.command == "changeplayerdifficulty");
    CHECK(line.args == "1");
    line = dc::splitConsoleLine("changeplayerdifficulty");
    CHECK(line.command == "changeplayerdifficulty");
    CHECK(line.args.empty());
    line = dc::splitConsoleLine("changeplayerdifficulty 1 2");
    CHECK(line.args == "1 2");
}

TEST_CASE("changePlayerDifficulty reads its argument as 0x0076f0d0 does", "[difficulty]") {
    // 0 and 1 are sent; nothing to read is 0.
    CHECK(dc::consoleDifficulty("0") == 0u);
    CHECK(dc::consoleDifficulty("1") == 1u);
    CHECK(dc::consoleDifficulty("") == 0u);
    CHECK(dc::consoleDifficulty("heroic") == 0u);
    CHECK(dc::consoleDifficulty("1x") == 1u);
    CHECK(dc::consoleDifficulty("1 2") == 1u);
    CHECK(dc::consoleDifficulty("-0") == 0u);
    CHECK(dc::consoleDifficulty("-") == 0u);
    CHECK(dc::consoleDifficulty("+1") == 0u);
    // 2 and up, and any negative, are "Invalid difficulty".
    CHECK_FALSE(dc::consoleDifficulty("2").has_value());
    CHECK_FALSE(dc::consoleDifficulty("10").has_value());
    CHECK_FALSE(dc::consoleDifficulty("-1").has_value());
    // 2^32 + 1 wraps to 1, as the client's unsigned arithmetic does.
    CHECK(dc::consoleDifficulty("4294967297") == 1u);
}

TEST_CASE("MSG_SET_RAID_DIFFICULTY's own and group words, as 0x005255a0 keeps them", "[difficulty]") {
    dc::RaidDifficulty r;
    // Out of a group the player's own is in force.
    auto a = dc::applyRaidDifficulty(r, 1, true, false, false, false);
    CHECK(a.changed);
    CHECK_FALSE(a.askPlayer.has_value());
    CHECK(r.effective(false) == 1);
    CHECK(r.effective(true) == 0);
    // The same again changes nothing and says nothing.
    CHECK_FALSE(dc::applyRaidDifficulty(r, 1, true, false, false, false).changed);
    // In a group the group's is in force; AzerothCore sends own=1, group=1.
    a = dc::applyRaidDifficulty(r, 3, true, true, false, true);
    CHECK(a.changed);
    CHECK(r.group == 3);
    CHECK(r.personal == 3);
    // On a map that switches in place the group's is not stored: the player's
    // difficulty is asked for instead - heroic past 25 normal.
    dc::RaidDifficulty d;
    a = dc::applyRaidDifficulty(d, 2, false, true, true, true);
    CHECK_FALSE(a.changed);
    CHECK(d.group == 0);
    REQUIRE(a.askPlayer.has_value());
    CHECK(*a.askPlayer == 1u);
    a = dc::applyRaidDifficulty(d, 1, false, true, true, true);
    REQUIRE(a.askPlayer.has_value());
    CHECK(*a.askPlayer == 0u);
}

TEST_CASE("a raid difficulty change is worded by the map it is said on", "[difficulty]") {
    CHECK(dc::raidChangeWords(true, 2) == dc::RaidChangeWords::Lockout);
    CHECK(dc::raidChangeWords(false, 2) == dc::RaidChangeWords::MapDifficulty);
    CHECK(dc::raidChangeWords(false, 1) == dc::RaidChangeWords::RaidDifficulty);
    CHECK(dc::raidChangeWords(false, 0) == dc::RaidChangeWords::RaidDifficulty);
}
