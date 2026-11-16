// An NPC's dialog emotes played one after another (0x0071a260, 0x0073adc0).
#include <catch_amalgamated.hpp>

#include "game/dialog_emotes.hpp"
#include "game/npc_text.hpp"

using wowee::game::DialogEmote;
using wowee::game::DialogEmoteQueue;

namespace {
// Emote 10 is a state emote here; the rest are one-shots.
bool oneShot(uint32_t emote) { return emote != 10; }
uint32_t length(uint32_t) { return 1000; }
}  // namespace

TEST_CASE("dialog emotes wait their delay, then the one before them and half a second", "[dialog_emotes]") {
    DialogEmoteQueue q;
    q.start({{100, 1}, {200, 2}}, 1000, oneShot);
    CHECK(q.active());
    CHECK_FALSE(q.take(1099, length));
    CHECK(q.take(1100, length) == 1u);
    // The second: its delay 200 from 1100, plus 1000 of animation and 500.
    CHECK_FALSE(q.take(2799, length));
    CHECK(q.take(2800, length) == 2u);
    CHECK_FALSE(q.active());
    CHECK_FALSE(q.take(99999, length));
}

TEST_CASE("only one-shot emotes are queued, four at most", "[dialog_emotes]") {
    DialogEmoteQueue q;
    q.start({{0, 10}, {0, 0}, {0, 3}, {0, 4}, {0, 5}, {0, 6}, {0, 7}}, 0, oneShot);
    CHECK(q.take(0, length) == 3u);
    CHECK(q.take(1500, length) == 4u);
    CHECK(q.take(3000, length) == 5u);
    CHECK(q.take(4500, length) == 6u);
    CHECK_FALSE(q.active());
}

TEST_CASE("nothing to play ends the queue", "[dialog_emotes]") {
    DialogEmoteQueue q;
    q.start({}, 0, oneShot);
    CHECK_FALSE(q.active());
    q.start({{0, 10}}, 0, oneShot);
    CHECK(q.active());
    CHECK_FALSE(q.take(0, length));
    CHECK_FALSE(q.active());
}

TEST_CASE("a greeting option's emotes run to the first empty one (0x0058ac30)", "[dialog_emotes]") {
    wowee::game::npc_text::Option o;
    o.emotes = {DialogEmote{0, 1}, DialogEmote{0, 0}, DialogEmote{0, 3}};
    CHECK(wowee::game::npc_text::optionEmotes(o).size() == 1);
    o.emotes[1].emote = 2;
    CHECK(wowee::game::npc_text::optionEmotes(o).size() == 3);
}
