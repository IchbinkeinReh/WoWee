// What SendChatMessage does to a line before sending it (0x0050d170): the
// %t/%f tokens (0x00577d00) and a drunk speaker's slurring (0x00577f60).
#include <catch_amalgamated.hpp>

#include <string>
#include <vector>

#include "game/chat_outgoing.hpp"

namespace co = wowee::game::chat_outgoing;

namespace {
/// Rolls handed out in order, then 0.99 (no) for every roll after them.
struct Rolls {
    std::vector<float> values;
    size_t next = 0;
    float operator()() { return next < values.size() ? values[next++] : 0.99f; }
};
}  // namespace

TEST_CASE("Target and focus tokens become names", "[chat_outgoing]") {
    CHECK(co::replaceTokens("heal %t now", "Bob", "Ann") == "heal Bob now");
    CHECK(co::replaceTokens("%T %n %N", "Bob", "Ann") == "Bob Bob Bob");
    CHECK(co::replaceTokens("sheep %f!", "Bob", "Ann") == "sheep Ann!");
    CHECK(co::replaceTokens("%F", "Bob", "<no focus>") == "<no focus>");
}

TEST_CASE("Other percent signs are kept as written", "[chat_outgoing]") {
    CHECK(co::replaceTokens("100%", "Bob", "Ann") == "100%");
    CHECK(co::replaceTokens("50% off", "Bob", "Ann") == "50% off");
    CHECK(co::replaceTokens("%%t", "Bob", "Ann") == "%Bob");  // the second % is a token
    CHECK(co::replaceTokens("no tokens", "Bob", "Ann") == "no tokens");
}

TEST_CASE("Token replacement stops at 255 bytes", "[chat_outgoing]") {
    const std::string longName(300, 'x');
    CHECK(co::replaceTokens("%t", longName, "") == std::string(255, 'x'));
}

TEST_CASE("A sober roll leaves the line alone", "[chat_outgoing]") {
    Rolls r;
    CHECK(co::slur("so this is it", 0.5f, std::ref(r), nullptr, "%s ...hic!") == "so this is it");
}

TEST_CASE("An s starting a word or ending one before a space gains an h", "[chat_outgoing]") {
    // Rolls: "So" (start), "this" (end, before a space), "is" (end), "stop" - start.
    Rolls r{{0.0f, 0.0f, 0.0f, 0.0f}};
    CHECK(co::slur("So this is stop", 1.0f, std::ref(r), nullptr, "%s ...hic!") == "Sho thish ish shtop");
    // An s ending the line has no space after it, and "ss" does not start
    // one: every roll would say yes, and only the hiccup's is drawn.
    Rolls r2{{0.0f, 0.0f, 0.0f, 0.0f, 0.0f}};
    CHECK(co::slur("ssh yes", 1.0f, std::ref(r2), nullptr, "%s ...hic!") == "ssh yes ...hic!");
    CHECK(r2.next == 1);
}

TEST_CASE("A slurred word the filter would mask keeps its s", "[chat_outgoing]") {
    Rolls r{{0.0f}};
    std::string asked;
    auto profane = [&](const std::string& w) { asked = w; return true; };
    CHECK(co::slur("sit", 1.0f, std::ref(r), profane, "%s") == "sit");
    CHECK(asked == "shit");
}

TEST_CASE("Coloured runs are copied untouched", "[chat_outgoing]") {
    Rolls r{{0.0f}};
    CHECK(co::slur("|cff00ff00|Hitem:1|h[Sword]|h|r s", 1.0f, std::ref(r), nullptr, "%s") ==
          "|cff00ff00|Hitem:1|h[Sword]|h|r sh");
}

TEST_CASE("A quarter of the drunkenness puts the line into SLURRED_SPEECH", "[chat_outgoing]") {
    Rolls hic{{0.1f}};   // the only roll: under 0.5 * 0.25
    CHECK(co::slur("hello", 0.5f, std::ref(hic), nullptr, "%s ...hic!") == "hello ...hic!");
    Rolls noHic{{0.2f}};
    CHECK(co::slur("hello", 0.5f, std::ref(noHic), nullptr, "%s ...hic!") == "hello");
}
