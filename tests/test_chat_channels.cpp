// The client's chat channels (game/chat_channels.hpp): the names it joins the
// zone channels by (0x00507a80, 0x00508090), the slots that number them
// (0x005076b0, 0x00507880, 0x00507900, 0x00507a30) and the notice words
// SMSG_CHANNEL_NOTIFY becomes (0x0050e120).
#include <catch_amalgamated.hpp>

#include "game/chat_channels.hpp"

namespace cc = wowee::game::chat_channels;

namespace {
// A German 3.3.5 client's ChatChannels.dbc, as it ships.
const std::vector<cc::Row> kRows = {
    {1, 0x80003, "Allgemein - %s", "Allgemein"},
    {2, 0x3b, "Handel - %s", "Handel"},
    {22, 0x10003, "LokaleVerteidigung - %s", "LokaleVerteidigung"},
    {23, 0x10004, "WeltVerteidigung", "WeltVerteidigung"},
    {25, 0x20032, "Gildenrekrutierung - %s", "Gildenrekrutierung"},
    {26, 0x40039, "SucheNachGruppe", "SucheNachGruppe"},
};
const cc::Place kDalaran{"Dalaran", true, "Hauptst\xc3\xa4" "dte"};
const cc::Place kElwynn{"Wald von Elwynn", false, "Hauptst\xc3\xa4" "dte"};
}  // namespace

TEST_CASE("a zone channel is joined by id under its localized name with the zone in it", "[chat_channels]") {
    auto j = cc::resolveJoin(kRows, "Allgemein", kElwynn);
    REQUIRE(j);
    CHECK(j->zoneChannelId == 1u);
    CHECK(j->name == "Allgemein - Wald von Elwynn");
    CHECK(j->shortcut == "Allgemein");
    // The shortcut is matched without regard to case, as 0x0076ea40 compares.
    j = cc::resolveJoin(kRows, "allgemein", kElwynn);
    REQUIRE(j);
    CHECK(j->zoneChannelId == 1u);
}

TEST_CASE("Trade and Guild Recruitment take the capitals' name in a city", "[chat_channels]") {
    auto j = cc::resolveJoin(kRows, "Handel", kDalaran);
    REQUIRE(j);
    CHECK(j->name == "Handel - Hauptst\xc3\xa4" "dte");
    j = cc::resolveJoin(kRows, "Gildenrekrutierung", kDalaran);
    REQUIRE(j);
    CHECK(j->name == "Gildenrekrutierung - Hauptst\xc3\xa4" "dte");
    // General has no 0x20: the zone's name even in a city.
    j = cc::resolveJoin(kRows, "Allgemein", kDalaran);
    REQUIRE(j);
    CHECK(j->name == "Allgemein - Dalaran");
    // Outside a city Trade is asked for under the zone's name.
    j = cc::resolveJoin(kRows, "Handel", kElwynn);
    REQUIRE(j);
    CHECK(j->name == "Handel - Wald von Elwynn");
}

TEST_CASE("a row without %s is its own name, and an unnamed zone joins nothing", "[chat_channels]") {
    auto j = cc::resolveJoin(kRows, "SucheNachGruppe", kElwynn);
    REQUIRE(j);
    CHECK(j->name == "SucheNachGruppe");
    CHECK(j->zoneChannelId == 26u);
    CHECK_FALSE(cc::resolveJoin(kRows, "Allgemein", cc::Place{}));
}

TEST_CASE("any other name is a channel of the player's own", "[chat_channels]") {
    auto j = cc::resolveJoin(kRows, "MeinKanal", kElwynn);
    REQUIRE(j);
    CHECK(j->zoneChannelId == 0u);
    CHECK(j->name == "MeinKanal");
    CHECK(j->shortcut.empty());
    // The English names are not shortcuts on a German client.
    j = cc::resolveJoin(kRows, "General", kElwynn);
    REQUIRE(j);
    CHECK(j->zoneChannelId == 0u);
}

TEST_CASE("the initial set is General, Trade, LocalDefense and LookingForGroup", "[chat_channels]") {
    const uint32_t mask = cc::initialMask(kRows);
    CHECK(mask == (cc::maskBit(1) | cc::maskBit(2) | cc::maskBit(22) | cc::maskBit(26)));
    CHECK_FALSE(mask & cc::maskBit(23));
    CHECK_FALSE(mask & cc::maskBit(25));
}

TEST_CASE("slots number channels in the order they are asked for and keep the numbers", "[chat_channels]") {
    cc::Slots slots;
    REQUIRE(slots.reserve("Allgemein - Dalaran", 1));
    REQUIRE(slots.reserve("Handel - Hauptst\xc3\xa4" "dte", 2));
    REQUIRE(slots.reserve("LokaleVerteidigung - Dalaran", 22));
    REQUIRE(slots.reserve("SucheNachGruppe", 26));
    // Asking again for one held sends nothing.
    CHECK_FALSE(slots.reserve("Allgemein - Dalaran", 1));
    // None is joined until the server says so.
    CHECK_FALSE(slots.joinedByNumber(1));

    // LocalDefense answers first; it is still number three.
    REQUIRE(slots.joined(22, "LokaleVerteidigung - Dalaran", 0, 0));
    REQUIRE(slots.joined(1, "Allgemein - Dalaran", 0, 0x18));
    REQUIRE(slots.joinedByNumber(3));
    CHECK(slots.joinedByNumber(3)->name == "LokaleVerteidigung - Dalaran");
    CHECK(slots.joinedByNumber(1)->name == "Allgemein - Dalaran");
    // Trade, never answered, holds number two without being listed.
    CHECK_FALSE(slots.joinedByNumber(2));
    REQUIRE(slots.byZoneId(2));

    // NOT_IN_LFG frees four, and the next channel asked for takes it.
    slots.clear(26, "SucheNachGruppe");
    CHECK_FALSE(slots.byZoneId(26));
    REQUIRE(slots.reserve("MeinKanal", 0));
    CHECK(slots.byName("MeinKanal")->number == 4);
}

TEST_CASE("a zone change renames the zone channel's slot to the server's name", "[chat_channels]") {
    cc::Slots slots;
    slots.reserve("Allgemein - Wald von Elwynn", 1);
    slots.joined(1, "Allgemein - Wald von Elwynn", 0, 0);
    const cc::Slot* before = slots.byZoneId(1);
    REQUIRE(before);
    // Joined while joined: YOU_CHANGED.
    CHECK(std::string(cc::noticeFor(0x02, !before->pending).word) == "YOU_CHANGED");
    slots.joined(1, "Allgemein - Sturmwind", 0, 0);
    CHECK(slots.joinedByNumber(1)->name == "Allgemein - Sturmwind");
    CHECK(slots.byName("allgemein - sturmwind"));
}

TEST_CASE("leaving: a constant channel lost with the zone is suspended, one left is gone", "[chat_channels]") {
    // The server takes Trade away on leaving a city.
    auto n = cc::noticeFor(0x03, false, true, false, true);
    CHECK(std::string(n.word) == "SUSPENDED");
    CHECK_FALSE(n.freesSlot);
    // The player left it.
    n = cc::noticeFor(0x03, false, true, true, true);
    CHECK(std::string(n.word) == "YOU_LEFT");
    CHECK(n.freesSlot);
    // A channel of the player's own.
    n = cc::noticeFor(0x03, false, true, false, false);
    CHECK(std::string(n.word) == "YOU_LEFT");
    // No slot: no word.
    CHECK(cc::noticeFor(0x03, false, false) .word == nullptr);
    // A first join.
    CHECK(std::string(cc::noticeFor(0x02, false).word) == "YOU_JOINED");
}

TEST_CASE("notice words and which event carries them", "[chat_channels]") {
    CHECK(std::string(cc::noticeFor(0x21).word) == "NOT_IN_LFG");
    CHECK_FALSE(cc::noticeFor(0x21).user);
    CHECK(cc::noticeFor(0x21).freesSlot);
    CHECK(std::string(cc::noticeFor(0x04).word) == "WRONG_PASSWORD");
    CHECK(cc::noticeFor(0x04).freesSlot);
    CHECK(std::string(cc::noticeFor(0x08).word) == "OWNER_CHANGED");
    CHECK(cc::noticeFor(0x08).user);
    CHECK(std::string(cc::noticeFor(0x12).word) == "PLAYER_KICKED");
    CHECK(cc::noticeFor(0x12).user);
    CHECK(std::string(cc::noticeFor(0x20).word) == "NOT_IN_AREA");
    CHECK_FALSE(cc::noticeFor(0x20).user);
    CHECK(cc::noticeFor(0x0C).word == nullptr);  // MODE_CHANGE is no line
}

TEST_CASE("the interface's line, as ChatFrame_MessageEventHandler formats it", "[chat_channels]") {
    cc::Slot s;
    s.number = 1;
    const std::string arg4 = cc::numberedName(&s, "Allgemein - Dalaran");
    CHECK(arg4 == "1. Allgemein - Dalaran");
    CHECK(cc::numberedName(nullptr, "X") == "X");
    CHECK(cc::noticeLine("Channel beigetreten: |Hchannel:%d|h[%s]|h", false, "YOU_JOINED", 1, arg4, "", "", 0) ==
          "Channel beigetreten: |Hchannel:1|h[1. Allgemein - Dalaran]|h");
    // The instance after the name.
    CHECK(cc::noticeLine("[%s]", false, "X", 1, "2. Foo", "", "", 3) == "[1]");
    CHECK(cc::noticeLine("%d [%s]", false, "X", 1, "2. Foo", "", "", 3) == "1 [2. Foo 3]");
    // Two players.
    CHECK(cc::noticeLine("|Hchannel:%d|h[%s]|h Spieler %s wurde von %s gebannt.", true, "PLAYER_BANNED", 2,
                         "2. Foo", "Ann", "Bob", 0) ==
          "|Hchannel:2|h[2. Foo]|h Spieler Ann wurde von Bob gebannt.");
    // INVITE's positional arguments.
    CHECK(cc::noticeLine("%2$s hat Euch eingeladen, dem Channel '%1$s' beizutreten.", true, "INVITE", 0,
                         "Foo", "Ann", "", 0) == "Ann hat Euch eingeladen, dem Channel 'Foo' beizutreten.");
}

TEST_CASE("names saved in English find their rows by what the rows are", "[chat_channels]") {
    CHECK(cc::rowForLegacyName(kRows, "General")->shortcut == "Allgemein");
    CHECK(cc::rowForLegacyName(kRows, "Trade")->shortcut == "Handel");
    CHECK(cc::rowForLegacyName(kRows, "LocalDefense")->shortcut == "LokaleVerteidigung");
    CHECK(cc::rowForLegacyName(kRows, "WorldDefense")->shortcut == "WeltVerteidigung");
    CHECK(cc::rowForLegacyName(kRows, "LookingForGroup")->shortcut == "SucheNachGruppe");
    CHECK(cc::rowForLegacyName(kRows, "GuildRecruitment")->shortcut == "Gildenrekrutierung");
    CHECK(cc::rowForLegacyName(kRows, "MeinKanal") == nullptr);
}

TEST_CASE("the name pattern takes one place and a literal percent", "[chat_channels]") {
    CHECK(cc::formatName("Allgemein - %s", "Dalaran") == "Allgemein - Dalaran");
    CHECK(cc::formatName("100%% - %s", "X") == "100% - X");
    CHECK(cc::formatName("Plain", "X") == "Plain");
}

TEST_CASE("a German file's names are read from the deDE column of their block", "[chat_channels]") {
    // Row 1 of a German ChatChannels.dbc as the loader leaves it: FactionGroup
    // (column 2) zero, the enUS columns 3 and 20 empty - promotion took column
    // 2 for the Name block's enUS column and missed it - and the text in deDE,
    // columns 6 and 23.
    const auto column = [](uint32_t f) -> std::string {
        if (f == 6) return "Allgemein - %s";
        if (f == 23) return "Allgemein";
        return {};
    };
    CHECK(column(3).empty());
    const uint32_t width = cc::localeWidth(37);
    CHECK(width == 16u);
    CHECK(cc::localizedText(column, 3, width) == "Allgemein - %s");
    CHECK(cc::localizedText(column, 20, width) == "Allgemein");
    CHECK(cc::localeWidth(21) == 8u);
    CHECK(cc::localeWidth(5) == 1u);
}

TEST_CASE("rows read with empty names cannot fill the slots", "[chat_channels]") {
    // What the first in-game run did: every row's Name read empty, so every
    // zone channel resolved to "" and the first took a slot under that name;
    // each next one then found "" held and was refused.
    const std::vector<cc::Row> broken = {
        {1, 0x80003, "", "Allgemein"},
        {2, 0x3b, "", "Handel"},
    };
    auto j = cc::resolveJoin(broken, "Allgemein", kElwynn);
    REQUIRE(j);
    CHECK(j->name.empty());
    cc::Slots slots;
    // An empty name is never given a slot.
    CHECK_FALSE(slots.reserve(j->name, j->zoneChannelId));
    CHECK(slots.all().empty());
    // Asking for one channel twice takes one slot.
    CHECK(slots.reserve("Allgemein - Wald von Elwynn", 1));
    CHECK_FALSE(slots.reserve("Allgemein - Wald von Elwynn", 1));
    CHECK_FALSE(slots.reserve("Allgemein - Anderswo", 1));
    CHECK(slots.all().size() == 1u);
}
