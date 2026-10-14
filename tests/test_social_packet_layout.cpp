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
    Writer w;
    w.u32((1u << 0) | (1u << 4));   // head and chest
    w.u32(40546);                   // head
    w.bytes.push_back(0x01); w.bytes.push_back(0x00);   // enchant slot 0
    w.bytes.push_back(0xA4); w.bytes.push_back(0x0E);   // 3748
    w.bytes.push_back(0x00); w.bytes.push_back(0x00);   // random property
    w.u8(0);                        // no creator
    w.u32(0);                       // suffix
    w.u32(40550);                   // chest
    w.bytes.push_back(0x00); w.bytes.push_back(0x00);   // no enchants
    w.bytes.push_back(0x2A); w.bytes.push_back(0x00);   // random property 42
    w.u8(0x01); w.u8(0x51);         // made by guid 0x51
    w.u32(1500);                    // suffix factor
    auto packet = w.packet();
    std::array<uint32_t, 19> items{};
    std::array<uint16_t, 19> enchants{};
    REQUIRE(readInspectEnchantments(packet, items, enchants));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(items[0] == 40546u);
    CHECK(enchants[0] == 3748u);
    CHECK(items[4] == 40550u);
    CHECK(enchants[4] == 0u);
}
