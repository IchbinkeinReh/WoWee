// The server's chat spam filter (SMSG_EXPECTED_SPAM_RECORDS, 0x00501c70 ->
// 0x00509dd0): the patterns it hands over mask the matched run in player chat.
#include <catch_amalgamated.hpp>

#include "game/chat_spam_filter.hpp"

using wowee::game::ChatSpamFilter;

TEST_CASE("invalid patterns are dropped, the rest kept", "[chat_spam_filter]") {
    ChatSpamFilter f;
    f.setPatterns({"gold", "buy[0-9]+", "([unclosed", ""});
    // The two that compile; the unclosed group and the empty one are dropped.
    CHECK(f.patternCount() == 2);
}

TEST_CASE("a matched run is masked with rotating symbols", "[chat_spam_filter]") {
    ChatSpamFilter f;
    f.setPatterns({"gold"});
    std::string line = "cheap gold here";
    CHECK(f.censor(line));
    // "gold" (4 chars) becomes the first four of !@#$%^&*, the rest untouched.
    CHECK(line == "cheap !@#$ here");
    // Nothing to match: left alone, no mask reported.
    std::string clean = "hello friends";
    CHECK_FALSE(f.censor(clean));
    CHECK(clean == "hello friends");
}

TEST_CASE("the symbol index carries across calls and matches", "[chat_spam_filter]") {
    ChatSpamFilter f;
    f.setPatterns({"ab"});
    std::string a = "ab";
    f.censor(a);
    CHECK(a == "!@");        // index 0,1
    std::string b = "ab";
    f.censor(b);
    CHECK(b == "#$");        // carries on from 2,3
}

TEST_CASE("case-insensitive, as REG_ICASE", "[chat_spam_filter]") {
    ChatSpamFilter f;
    f.setPatterns({"wow"});
    std::string line = "WoW GoLd";
    CHECK(f.censor(line));
    CHECK(line.substr(0, 3) == "!@#");
}

TEST_CASE("a match inside a colour span is left alone (0x004fcf40)", "[chat_spam_filter]") {
    ChatSpamFilter f;
    f.setPatterns({"item"});
    // The word sits inside a |c..|r hyperlink colour span: not masked.
    std::string link = "|cff00ff00item|r";
    CHECK_FALSE(f.censor(link));
    CHECK(link == "|cff00ff00item|r");
    // The same word outside a span is masked.
    std::string plain = "item";
    CHECK(f.censor(plain));
    CHECK(plain == "!@#$");
}

TEST_CASE("insideColorSpan tracks open and closed spans", "[chat_spam_filter]") {
    const std::string s = "|cff112233link|r after";
    CHECK(ChatSpamFilter::insideColorSpan(s, 12));   // within the link text
    CHECK_FALSE(ChatSpamFilter::insideColorSpan(s, 20));  // after |r
    CHECK_FALSE(ChatSpamFilter::insideColorSpan("plain", 3));
}
