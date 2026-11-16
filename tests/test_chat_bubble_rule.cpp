// Which chat lines put a bubble over their speaker (0x00720010).
#include <catch_amalgamated.hpp>

#include "game/chat_bubble_rule.hpp"

using namespace wowee::game::chat_bubble_rule;

TEST_CASE("chat bubble kinds follow the client's six types", "[chat][bubble]") {
    CHECK(settingFor(0x01) == Setting::ChatBubbles);       // SAY
    CHECK(settingFor(0x06) == Setting::ChatBubbles);       // YELL
    CHECK(settingFor(0x0c) == Setting::ChatBubbles);       // MONSTER_SAY
    CHECK(settingFor(0x0e) == Setting::ChatBubbles);       // MONSTER_YELL
    CHECK(settingFor(0x02) == Setting::ChatBubblesParty);  // PARTY
    CHECK(settingFor(0x33) == Setting::ChatBubblesParty);  // PARTY_LEADER
    CHECK(settingFor(0x03) == Setting::None);              // RAID
    CHECK(settingFor(0x0d) == Setting::None);              // MONSTER_PARTY
    CHECK(settingFor(0x07) == Setting::None);              // WHISPER
}

TEST_CASE("a chat bubble needs its speaker within 25 yards", "[chat][bubble]") {
    CHECK(inRange(0.0f));
    CHECK(inRange(625.0f));
    CHECK_FALSE(inRange(625.5f));
}
