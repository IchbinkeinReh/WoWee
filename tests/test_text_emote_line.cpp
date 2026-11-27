// A text emote's chat line, picked from its EmotesText slots as 0x00503660 does.
#include <catch_amalgamated.hpp>

#include "game/text_emote_line.hpp"

using wowee::game::EmoteTextSlots;
using wowee::game::textEmoteLine;

namespace {
// ANGRY of the deDE client: slots 0, 1, 2, 4, 6 and the female 8, 9, 12.
EmoteTextSlots angry() {
    EmoteTextSlots s;
    s[0] = "%1$s erhebt erbost seine Faust gegen %2$s.";
    s[1] = "%s erhebt erbost seine Faust gegen Euch.";
    s[2] = "Ihr erhebt erbost Eure Faust gegen %s.";
    s[4] = "%s erhebt erbost seine Faust.";
    s[6] = "Ihr erhebt erbost Eure Faust.";
    s[8] = "%1$s erhebt erbost ihre Faust gegen %2$s.";
    s[9] = "%s erhebt erbost ihre Faust gegen Euch.";
    s[12] = "%s erhebt erbost ihre Faust.";
    return s;
}
}  // namespace

TEST_CASE("a text emote takes the slot for sender, target and gender", "[text_emote]") {
    const auto s = angry();
    CHECK(textEmoteLine(s, false, "Bob", "Tom", "Locki", false) == "Bob erhebt erbost seine Faust gegen Tom.");
    CHECK(textEmoteLine(s, false, "Ann", "Tom", "Locki", true) == "Ann erhebt erbost ihre Faust gegen Tom.");
    // At the player: slot 1 / 9 with the sender alone.
    CHECK(textEmoteLine(s, false, "Bob", "Locki", "Locki", false) == "Bob erhebt erbost seine Faust gegen Euch.");
    CHECK(textEmoteLine(s, false, "Ann", "Locki", "Locki", true) == "Ann erhebt erbost ihre Faust gegen Euch.");
    CHECK(textEmoteLine(s, false, "Ann", "", "Locki", true) == "Ann erhebt erbost ihre Faust.");
    // The player's own: no female self slot, so the plain one.
    CHECK(textEmoteLine(s, true, "Locki", "Tom", "Locki", true) == "Ihr erhebt erbost Eure Faust gegen Tom.");
    CHECK(textEmoteLine(s, true, "Locki", "", "Locki", false) == "Ihr erhebt erbost Eure Faust.");
    // The player at itself is not 'at me' (0x00503660 compares only another's).
    CHECK(textEmoteLine(s, true, "Locki", "Locki", "Locki", false) == "Ihr erhebt erbost Eure Faust gegen Locki.");
}

TEST_CASE("a text emote with no target text falls back to the no-target one, else no line", "[text_emote]") {
    // SLEEP: only slots 4 and 6 (T453).
    EmoteTextSlots s;
    s[4] = "%s schläft ein. Zzzzzzz.";
    s[6] = "Ihr schlaft ein. Zzzzzzz.";
    CHECK(textEmoteLine(s, true, "Locki", "Grunzer", "Locki", false) == "Ihr schlaft ein. Zzzzzzz.");
    CHECK(textEmoteLine(s, false, "Bob", "Locki", "Locki", true) == "Bob schläft ein. Zzzzzzz.");
    CHECK(textEmoteLine(s, false, "Bob", "Tom", "Locki", false) == "Bob schläft ein. Zzzzzzz.");
    // STAND: no text at all.
    CHECK(textEmoteLine(EmoteTextSlots{}, true, "Locki", "", "Locki", false).empty());
    CHECK(textEmoteLine(EmoteTextSlots{}, false, "Bob", "Tom", "Locki", false).empty());
}
