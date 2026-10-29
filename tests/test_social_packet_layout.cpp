// SMSG_MESSAGECHAT and SMSG_RAID_INSTANCE_INFO as TrinityCore and
// AzerothCore 3.3.5 build them (ChatHandler::BuildChatPacket,
// Player::SendRaidInfo), read as wow.exe 12340 reads them: 0x0050be70 for
// the chat bodies, 0x00501030 for the saves.
#include <catch_amalgamated.hpp>

#include <array>
#include <string>
#include <vector>

#include "core/application.hpp"
#include "game/world_packets.hpp"

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}  // namespace wowee

using namespace wowee::game;

namespace {

struct Writer {
    std::vector<uint8_t> bytes;
    void u8(uint8_t v) { bytes.push_back(v); }
    void u16(uint16_t v) {
        bytes.push_back(static_cast<uint8_t>(v & 0xFF));
        bytes.push_back(static_cast<uint8_t>(v >> 8));
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    void u64(uint64_t v) {
        u32(static_cast<uint32_t>(v));
        u32(static_cast<uint32_t>(v >> 32));
    }
    /// uint32 length counting the terminator, then the string and the 0.
    void sized(const std::string& s) {
        u32(static_cast<uint32_t>(s.size() + 1));
        for (char c : s) u8(static_cast<uint8_t>(c));
        u8(0);
    }
    wowee::network::Packet packet() const { return wowee::network::Packet(0x96, bytes); }
};

void header(Writer& w, uint8_t type, uint64_t sender) {
    w.u8(type);
    w.u32(0);  // LANG_UNIVERSAL
    w.u64(sender);
    w.u32(0);
}

}  // namespace

TEST_CASE("chat: a battleground announcement carries a target guid", "[chat]") {
    // "The battle for Warsong Gulch begins in 1 minute." - target 0.
    Writer w;
    header(w, 0x24, 0);
    w.u64(0);
    w.sized("The battle for Warsong Gulch begins in 1 minute.");
    w.u8(0);
    auto packet = w.packet();
    MessageChatData data;
    REQUIRE(MessageChatParser::parse(packet, data));
    CHECK(packet.getRemainingSize() == 0);
    // Read with nothing before the length, the guid's zero was the length
    // and the announcement came out empty.
    CHECK(data.message == "The battle for Warsong Gulch begins in 1 minute.");
    CHECK(data.type == ChatType::BG_SYSTEM_NEUTRAL);
}

TEST_CASE("chat: a battleground announcement naming a creature carries its name", "[chat]") {
    constexpr uint64_t kHerald = 0xF130003A8A000077ull;
    Writer w;
    header(w, 0x26, kHerald);
    w.u64(kHerald);
    w.sized("Herald");
    w.sized("The Horde has taken the Blacksmith!");
    w.u8(0);
    auto packet = w.packet();
    MessageChatData data;
    REQUIRE(MessageChatParser::parse(packet, data));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(data.type == ChatType::BG_SYSTEM_HORDE);
    CHECK(data.receiverGuid == kHerald);
    CHECK(data.receiverName == "Herald");
    CHECK(data.message == "The Horde has taken the Blacksmith!");
}

TEST_CASE("chat: a foreign whisper names its sender and reads as a whisper", "[chat]") {
    Writer w;
    header(w, 0x08, 0x51);
    w.sized("Farfriend");
    w.u64(0x42);
    w.sized("hello from afar");
    w.u8(0);
    auto packet = w.packet();
    MessageChatData data;
    REQUIRE(MessageChatParser::parse(packet, data));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(data.type == ChatType::WHISPER);
    CHECK(data.senderName == "Farfriend");
    CHECK(data.receiverGuid == 0x42u);
    CHECK(data.message == "hello from afar");
}

TEST_CASE("chat: type 0x2F is BATTLENET and reads like a creature's line", "[chat]") {
    // 0x0050be70 puts 0x2F with the creature-spoken types: the sender's
    // name, then a target guid (and its name when it is not a player's).
    Writer w;
    header(w, 0x2F, 0x99);
    w.sized("Friend");
    w.u64(0);
    w.sized("hi");
    w.u8(0);
    auto packet = w.packet();
    MessageChatData data;
    REQUIRE(MessageChatParser::parse(packet, data));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(data.type == ChatType::BATTLENET);
    CHECK(data.senderName == "Friend");
    CHECK(data.message == "hi");
    CHECK(std::string(getChatTypeString(ChatType::BATTLENET)) == "BATTLENET");
    CHECK(std::string(getChatTypeString(ChatType::ARENA_POINTS)) == "ARENA_POINTS");
}

TEST_CASE("chat: a GM's name comes only before the general branch's types", "[chat]") {
    // SMSG_GM_MESSAGECHAT is 0x0050be70 with the GM flag: the name is read
    // for say, whisper, channel and the like, not for creature types, the
    // foreign whisper, battleground announcements or achievements.
    CHECK(gmChatCarriesSenderName(0x01));   // SAY
    CHECK(gmChatCarriesSenderName(0x07));   // WHISPER
    CHECK(gmChatCarriesSenderName(0x11));   // CHANNEL
    CHECK_FALSE(gmChatCarriesSenderName(0x0C));  // MONSTER_SAY
    CHECK_FALSE(gmChatCarriesSenderName(0x08));  // WHISPER_FOREIGN
    CHECK_FALSE(gmChatCarriesSenderName(0x24));  // BG_SYSTEM_NEUTRAL
    CHECK_FALSE(gmChatCarriesSenderName(0x2F));  // BATTLENET
    CHECK_FALSE(gmChatCarriesSenderName(0x30));  // ACHIEVEMENT
}

TEST_CASE("raid info: each save carries its instance id and the seconds left", "[raidinfo]") {
    Writer w;
    w.u32(2);
    w.u32(533);                  // Naxxramas
    w.u32(1);                    // 25 player
    w.u64(0x0000000000001234ull);
    w.u8(1);                     // locked
    w.u8(0);                     // not extended
    w.u32(3 * 86400 + 3600);
    w.u32(615);                  // The Obsidian Sanctum
    w.u32(0);
    w.u64(0x0000000000005678ull);
    w.u8(1);
    w.u8(1);
    w.u32(7200);
    auto packet = w.packet();
    std::vector<RaidInstanceSave> saves;
    REQUIRE(RaidInstanceInfoParser::parse(packet, saves));
    CHECK(packet.getRemainingSize() == 0);
    REQUIRE(saves.size() == 2);
    CHECK(saves[0].mapId == 533u);
    CHECK(saves[0].difficulty == 1u);
    CHECK(saves[0].instanceId == 0x1234u);
    CHECK(saves[0].locked);
    CHECK_FALSE(saves[0].extended);
    CHECK(saves[0].resetSeconds == 3u * 86400u + 3600u);
    // Eighteen bytes a save put this one four bytes early.
    CHECK(saves[1].mapId == 615u);
    CHECK(saves[1].instanceId == 0x5678u);
    CHECK(saves[1].extended);
    CHECK(saves[1].resetSeconds == 7200u);
}

TEST_CASE("inspect: each item carries its property, creator and suffix", "[inspect]") {
    // Player::BuildEnchantmentsInfoData: slot mask, then per item the entry,
    // the enchantment mask and ids, the random property (u16), the creator's
    // packed guid and the suffix factor (0x006cdf30).
    //
    // The enchantment mask is over the item's twelve enchantment **blocks** -
    // 0x006cdf30 writes one id per set bit into successive record dwords from
    // the enchantment slot up, so bit 0 is the permanent enchant, bit 1 the
    // temporary one and bits 2, 3 and 4 the three sockets.  A cleared bit moves
    // the next block up just the same, which is what the chest here shows with
    // its bit 1 clear.
    //
    // The head is the two oddities on the way through: an entry the realm sent
    // negative (the magnitude is the item, the flag beside it says it was
    // flagged), and a property word belonging to a prefix, which the link reads
    // back zero-extended at 0x005ea3a1 while the tooltip sign-extends it at
    // 0x0062e424.  The row stores the word, so the number 65494 is what a link
    // for that item says - and with no ITEM_RANDOM_PROPERTIES row at 65494, the
    // client prints the bare name over it.
    Writer w;
    w.u32((1u << 0) | (1u << 4));   // head and chest
    w.u32(0xFFFF619Eu);             // head: entry -40546
    w.u16(0x0001);                  // enchantment block 0 only
    w.u16(3748);
    w.u16(0xFFD6);                  // random property -42, as a word
    w.u8(0);                        // no creator
    w.u32(0);                       // no suffix factor
    w.u32(40550);                   // chest
    w.u16(0x000D);                  // blocks 0, 2 and 3: the enchant and two gems
    w.u16(2672);                    // ITEM_FIELD_ENCHANTMENT_1_1
    w.u16(3149);                    // the first socket gem (block 2)
    w.u16(3150);                    // the second (block 3)
    w.u16(42);                      // random property 42
    w.u8(0x01); w.u8(0x51);         // made by guid 0x51
    w.u32(1500);                    // suffix factor
    auto packet = w.packet();
    std::array<uint32_t, 19> items{};
    std::array<ItemLinkInstance, 19> gear{};
    REQUIRE(readInspectEnchantments(packet, items, gear));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(items[0] == 40546u);
    CHECK(gear[0].enchantId == 3748u);
    CHECK(gear[0].randomPropertyId == 0xFFD6);
    CHECK(gear[0].gems[0] == 0u);
    CHECK(gear[0].seed == 0u);
    CHECK(items[4] == 40550u);
    CHECK(gear[4].enchantId == 2672u);
    CHECK(gear[4].gems[0] == 3149u);
    CHECK(gear[4].gems[1] == 3150u);
    CHECK(gear[4].gems[2] == 0u);
    CHECK(gear[4].randomPropertyId == 42);
    CHECK(gear[4].seed == 1500u);
}

// ── /who ──────────────────────────────────────────────────────────────
// The filter as 0x006b5630 reads it, the CMSG_WHO it becomes and the
// SMSG_WHO 0x006b8720 reads back.

namespace {

const std::vector<who_filter::Named> kZones = {
    {12, "Elwynn Forest"}, {1519, "Stormwind City"}, {85, "Tirisfal Glades"}, {1497, "Undercity"}};
const std::vector<who_filter::Named> kRaces = {
    {1, "Human"}, {2, "Orc"}, {5, "Undead"}, {10, "Blood Elf"}, {4, "Night Elf"}};
const std::vector<who_filter::Named> kClasses = {{1, "Warrior"}, {2, "Paladin"}, {8, "Mage"}};

who_filter::Request parseWho(const std::string& text) {
    return who_filter::parse(text, who_filter::Tags{}, kZones, kRaces, kClasses);
}

}  // namespace

TEST_CASE("An empty /who asks for everyone, levels 0 to 100", "[who]") {
    const auto r = parseWho("");
    CHECK(r.minLevel == 0u);
    CHECK(r.maxLevel == 100u);
    CHECK(r.name.empty());
    CHECK(r.guild.empty());
    CHECK(r.raceMask == 0xFFFFFFFFu);
    CHECK(r.classMask == 0xFFFFFFFFu);
    CHECK(r.zones.empty());
    CHECK(r.strings.empty());
}

TEST_CASE("A /who's levels: one, a range, open above, open below", "[who]") {
    auto r = parseWho("80");
    CHECK(r.minLevel == 80u);
    CHECK(r.maxLevel == 80u);
    CHECK(r.strings.empty());
    r = parseWho("70-80");
    CHECK(r.minLevel == 70u);
    CHECK(r.maxLevel == 80u);
    r = parseWho("70-");
    CHECK(r.minLevel == 70u);
    CHECK(r.maxLevel == 100u);
    r = parseWho("-15");
    CHECK(r.minLevel == 0u);
    CHECK(r.maxLevel == 15u);
    // A lone dash is a word.
    r = parseWho("-");
    CHECK(r.maxLevel == 100u);
    REQUIRE(r.strings.size() == 1);
    CHECK(r.strings[0] == "-");
}

TEST_CASE("A /who's plain words go to the server to match, four at most", "[who]") {
    auto r = parseWho("Locki");
    CHECK(r.name.empty());
    REQUIRE(r.strings.size() == 1);
    CHECK(r.strings[0] == "Locki");
    r = parseWho("a b c d e f");
    REQUIRE(r.strings.size() == 4);
    CHECK(r.strings[3] == "d");
    // Quotes keep a space in one word and go themselves.
    r = parseWho("\"Night Elf\" 80");
    REQUIRE(r.strings.size() == 1);
    CHECK(r.strings[0] == "Night Elf");
    CHECK(r.minLevel == 80u);
}

TEST_CASE("A /who's tags: name, guild, zone, race and class", "[who]") {
    auto r = parseWho("n-Locki g-\"The Order\" 10-20");
    CHECK(r.name == "Locki");
    CHECK(r.guild == "The Order");
    CHECK(r.minLevel == 10u);
    CHECK(r.maxLevel == 20u);
    CHECK(r.strings.empty());
    // Case does not matter, in the tag or the name it looks for.
    r = parseWho("Z-stormwind");
    REQUIRE(r.zones.size() == 1);
    CHECK(r.zones[0] == 1519u);
    // Part of a name takes every zone holding it.
    r = parseWho("z-city");
    REQUIRE(r.zones.size() == 2);
    CHECK(r.zones[1] == 1497u);
    // A zone nothing is called is zone 0: nobody, not everybody.
    r = parseWho("z-Nowhere");
    REQUIRE(r.zones.size() == 1);
    CHECK(r.zones[0] == 0u);
    // A race or class sets its bit; "elf" is both elves.
    r = parseWho("r-elf c-mage");
    CHECK(r.raceMask == ((1u << 10) | (1u << 4)));
    CHECK(r.classMask == (1u << 8));
    r = parseWho("r-Gnome");
    CHECK(r.raceMask == 0u);
    // The tags in the client's language.
    who_filter::Tags de;
    de.name = "n-";
    de.race = "v-";
    r = who_filter::parse("v-Untot", de, kZones, {{5, "Untoter"}}, kClasses);
    CHECK(r.raceMask == (1u << 5));
    // Umlauts fold too.
    CHECK(who_filter::containsFolded("Ödland", "öd"));
}

TEST_CASE("CMSG_WHO carries the filter as 0x006b5630 writes it", "[who]") {
    auto r = parseWho("n-Locki z-Undercity 80 mage");
    auto packet = WhoPacket::build(r);
    CHECK(packet.readUInt32() == 80u);
    CHECK(packet.readUInt32() == 80u);
    CHECK(packet.readString() == "Locki");
    CHECK(packet.readString().empty());
    CHECK(packet.readUInt32() == 0xFFFFFFFFu);
    CHECK(packet.readUInt32() == 0xFFFFFFFFu);
    REQUIRE(packet.readUInt32() == 1u);
    CHECK(packet.readUInt32() == 1497u);
    REQUIRE(packet.readUInt32() == 1u);
    CHECK(packet.readString() == "mage");
    CHECK(!packet.hasRemaining(1));
}

TEST_CASE("SMSG_WHO's rows read as 0x006b8720 reads them", "[who]") {
    Writer w;
    w.u32(2);   // rows
    w.u32(37);  // matched
    const auto row = [&](const std::string& name, const std::string& guild, uint32_t level, uint32_t cls,
                         uint32_t race, uint32_t zone) {
        for (char c : name) w.u8(static_cast<uint8_t>(c));
        w.u8(0);
        for (char c : guild) w.u8(static_cast<uint8_t>(c));
        w.u8(0);
        w.u32(level);
        w.u32(cls);
        w.u32(race);
        w.u8(1);  // gender
        w.u32(zone);
    };
    row("Locki", "", 80, 8, 10, 1497);
    row("Palala", "Die Gilde", 79, 2, 10, 3487);
    auto packet = w.packet();
    std::vector<WhoEntry> rows;
    uint32_t shown = 0, matched = 0;
    REQUIRE(WhoResultParser::parse(packet, true, rows, shown, matched));
    CHECK(shown == 2u);
    CHECK(matched == 37u);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].name == "Locki");
    CHECK(rows[0].guildName.empty());
    CHECK(rows[0].level == 80u);
    CHECK(rows[0].classId == 8u);
    CHECK(rows[0].raceId == 10u);
    CHECK(rows[0].zoneId == 1497u);
    CHECK(rows[1].guildName == "Die Gilde");
    CHECK(rows[1].zoneId == 3487u);

    // Cut short: what was read stands, and it says so.
    Writer cut;
    cut.u32(1);
    cut.u32(1);
    for (char c : std::string("Locki")) cut.u8(static_cast<uint8_t>(c));
    cut.u8(0);
    cut.u8(0);
    cut.u32(80);
    auto shortPacket = cut.packet();
    CHECK_FALSE(WhoResultParser::parse(shortPacket, true, rows, shown, matched));
    CHECK(rows.empty());
}
