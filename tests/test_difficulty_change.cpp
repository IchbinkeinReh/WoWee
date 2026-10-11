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
