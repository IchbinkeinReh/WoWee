// A unit drawn as a player character (game/mirror_image.hpp): the answer to
// CMSG_GET_MIRRORIMAGE_DATA, which the server's player-like bots are dressed
// by. Only the display id used to be read, so a bot came out as whatever its
// placeholder display was.
#include <catch_amalgamated.hpp>

#include "game/mirror_image.hpp"

#include <vector>

using wowee::game::MirrorImageLook;
using wowee::game::parseMirrorImageData;

namespace {
void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// One answer as the server writes it: 68 bytes.
std::vector<uint8_t> answer() {
    std::vector<uint8_t> b;
    const uint64_t guid = 0xF13007F22100C120ull;
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(guid >> (8 * i)));
    put32(b, 2123);              // the race's display
    b.push_back(6);              // tauren
    b.push_back(1);              // female
    b.push_back(11);             // druid
    b.push_back(9);              // skin
    b.push_back(0);              // face
    b.push_back(0);              // hair style
    b.push_back(1);              // hair colour
    b.push_back(1);              // facial hair
    put32(b, 0);                 // guild
    for (uint32_t i = 0; i < 11; ++i) put32(b, 1000 + i);  // head .. tabard
    return b;
}
}  // namespace

TEST_CASE("the answer is read field by field", "[mirror_image]") {
    const auto b = answer();
    REQUIRE(b.size() == 68);
    const auto look = parseMirrorImageData(b.data(), b.size());
    REQUIRE(look);
    CHECK(look->guid == 0xF13007F22100C120ull);
    CHECK(look->displayId == 2123);
    CHECK(look->raceId == 6);
    CHECK(look->gender == 1);
    CHECK(look->classId == 11);
    CHECK(look->skin == 9);
    CHECK(look->face == 0);
    CHECK(look->hairStyle == 0);
    CHECK(look->hairColor == 1);
    CHECK(look->facialHair == 1);
    CHECK(look->items[0] == 1000);   // head
    CHECK(look->items[10] == 1010);  // last on the wire
    CHECK(look->hasCharacter());
}

TEST_CASE("back and tabard swap into CreatureDisplayInfoExtra's order", "[mirror_image]") {
    const auto b = answer();
    const auto look = parseMirrorImageData(b.data(), b.size());
    REQUIRE(look);
    const auto extra = look->itemsInExtraOrder();
    // Wire: ..., hands (8), back (9), tabard (10). Extra: ..., hands, tabard, cape.
    for (int i = 0; i < 9; ++i) CHECK(extra[i] == look->items[i]);
    CHECK(extra[9] == look->items[10]);
    CHECK(extra[10] == look->items[9]);
}

TEST_CASE("a creature caster's answer has no character to draw", "[mirror_image]") {
    std::vector<uint8_t> b(68, 0);
    b[0] = 1;  // a guid
    const auto look = parseMirrorImageData(b.data(), b.size());
    REQUIRE(look);
    CHECK_FALSE(look->hasCharacter());
}

TEST_CASE("an answer too short to name its race is refused", "[mirror_image]") {
    const auto b = answer();
    CHECK_FALSE(parseMirrorImageData(b.data(), 14));
    // Cut after the race: read, the rest left at zero.
    const auto look = parseMirrorImageData(b.data(), 15);
    REQUIRE(look);
    CHECK(look->raceId == 6);
    CHECK(look->items[10] == 0);
}

TEST_CASE("model paths match whatever their case, slashes and extension", "[mirror_image]") {
    using wowee::game::mirror_model_path::samePath;
    CHECK(samePath(R"(Character\Tauren\Female\TaurenFemale.m2)", "character/tauren/female/taurenfemale.mdx"));
    CHECK(samePath(R"(CHARACTER\Troll\Male\TrollMale.MDX)", R"(Character\Troll\Male\TrollMale.m2)"));
    CHECK_FALSE(samePath(R"(Character\Troll\Male\TrollMale.m2)", R"(Character\Troll\Female\TrollFemale.m2)"));
    CHECK_FALSE(samePath(R"(Character\Orc\Male\OrcMale.m2)", R"(Creature\Orc\OrcMale.m2)"));
}

TEST_CASE("a changed look is told from the same one", "[mirror_image]") {
    const auto b = answer();
    auto a = *parseMirrorImageData(b.data(), b.size());
    auto c = a;
    CHECK(a.sameLookAs(c));
    c.guildId = 7;  // draws nothing
    CHECK(a.sameLookAs(c));
    c.items[3] = 42;  // another chest
    CHECK_FALSE(a.sameLookAs(c));
}

TEST_CASE("the displays made for these units lie past every DBC row", "[mirror_image]") {
    CHECK(wowee::game::kMirrorDisplayIdBase > 100000u);
}
