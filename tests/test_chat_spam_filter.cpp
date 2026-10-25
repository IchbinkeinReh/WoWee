// The chat spam filter (0x00509bd0): SpamMessages.dbc's patterns and the
// server's (SMSG_EXPECTED_SPAM_RECORDS), compiled caseless as PCRE does, and
// a line any of them finds is dropped. The patterns below are SpamMessages.dbc
// rows 4 and 8 of the 3.3.5a client, verbatim - Perl syntax, \s* between every
// letter, which POSIX extended cannot even compile.
#include <catch_amalgamated.hpp>

#include "game/chat_spam_filter.hpp"

using wowee::game::ChatSpamFilter;

namespace {
const std::string kItembay = R"((w\s*w\s*w\s*\.)?\s*i\s*t\s*e\s*m\s*b\s*a\s*y\s*(\.\s*c\s*a))";
const std::string kGamebmo = R"((w\s*w\s*w\s*\.)?\s*g\s*a\s*m\s*e\s*b\s*m\s*o\s*(\.\s*c\s*o\s*m))";
}  // namespace

TEST_CASE("the client's own Perl-syntax patterns compile and catch spaced-out spam", "[chat_spam_filter]") {
    ChatSpamFilter f;
    f.setClientPatterns({kItembay, kGamebmo});
    CHECK(f.clientPatternCount() == 2);
    CHECK(f.matches("buy gold at w w w . itembay . c a now"));
    CHECK(f.matches("WWW.GAMEBMO.COM cheap"));       // caseless
    CHECK(f.matches("g a m e b m o . c o m"));
    CHECK_FALSE(f.matches("hello friend, want to quest?"));
    CHECK_FALSE(f.matches(""));
}

TEST_CASE("the server's list is a second list beside the client's", "[chat_spam_filter]") {
    ChatSpamFilter f;
    f.setClientPatterns({kItembay});
    f.setServerPatterns({R"(c\s*h\s*e\s*a\s*p\s*g\s*o\s*l\s*d)"});
    CHECK(f.matches("c h e a p g o l d"));
    CHECK(f.matches("itembay.ca"));
    // A new server list replaces the old one, not the client's.
    f.setServerPatterns({});
    CHECK_FALSE(f.matches("c h e a p g o l d"));
    CHECK(f.matches("itembay.ca"));
}

TEST_CASE("an expression that does not compile is dropped and the rest kept", "[chat_spam_filter]") {
    ChatSpamFilter f;
    f.setServerPatterns({"gold", "([unclosed", ""});
    CHECK(f.serverPatternCount() == 1);
    CHECK(f.matches("GOLD"));
}
