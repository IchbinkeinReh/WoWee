// SMSG_MESSAGECHAT and SMSG_RAID_INSTANCE_INFO as TrinityCore and
// AzerothCore 3.3.5 build them (ChatHandler::BuildChatPacket,
// Player::SendRaidInfo), read as wow.exe 12340 reads them: 0x0050be70 for
// the chat bodies, 0x00501030 for the saves.
#include <catch_amalgamated.hpp>

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
