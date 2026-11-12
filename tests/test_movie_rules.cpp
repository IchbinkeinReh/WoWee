// The MovieFrame's rules that need no decoder: the .sbt subtitle file
// (0x0095e880) and the resolution a movie is played at (0x00518700).

#include <catch_amalgamated.hpp>

#include "ui/movie_rules.hpp"

using namespace wowee::ui::movie;

TEST_CASE("sbt lines: timecodes in 41 ms frames, the text on the next line") {
    const std::string text =
        "\xEF\xBB\xBF" "00:00:14:12 - 00:00:17:08\r\n"
        "Hochlord Bolvar! Dem Licht sei Dank!\r\n"
        "\r\n"
        "00:01:02:08 - 00:01:03:12\r\n"
        "F\xC3\x9CR DIE HORDE!\r\n";
    const auto subs = parseSubtitles(text);
    REQUIRE(subs.size() == 2);
    REQUIRE(subs[0].startMs == 14000 + 12 * 41);
    REQUIRE(subs[0].endMs == 17000 + 8 * 41);
    REQUIRE(subs[0].text == "Hochlord Bolvar! Dem Licht sei Dank!");
    REQUIRE(subs[1].startMs == 62000 + 8 * 41);
    REQUIRE(subs[1].endMs == 63000 + 12 * 41);
    REQUIRE(subs[1].text == "F\xC3\x9CR DIE HORDE!");
}

TEST_CASE("resolution: exact width, else the largest below it, else the smallest") {
    // Wrathgate: MovieVariation 61 -> 800, 62 -> 1024, in that order.
    REQUIRE(pickResolution({800, 1024}, 1024) == 1);
    REQUIRE(pickResolution({800, 1024}, 1920) == 1);
    REQUIRE(pickResolution({1024, 800}, 1920) == 0);
    REQUIRE(pickResolution({1024, 800}, 900) == 1);
    REQUIRE(pickResolution({1024, 800}, 640) == 1);
    REQUIRE(pickResolution({800, 1024}, 640) == 0);
    REQUIRE(pickResolution({}, 1920) == -1);
}
