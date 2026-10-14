// SMSG_BATTLEFIELD_STATUS's header, which was read two bytes short.
//
// The server writes a stretch of this packet as one uint64 and says so in a
// comment, which is what makes it easy to read as fewer, wider fields than it
// holds:
//
//     uint32 queueSlot, uint8 arenaType, uint8 isArena, uint32 bgTypeId,
//     uint16 0x1F90, uint8 minLevel, uint8 maxLevel, uint32 instanceId,
//     uint8 isRated, uint32 statusId
//
// Read as `uint32 instanceId, uint8 isRated` - five bytes where the server
// sends seven - the status arrives as the top of the instance id joined to the
// bottom of the status, and is never one of the values it is compared against.
// The packet is the same length either way, so nothing runs short and nothing
// reports anything: a queue that exists reads as no queue at all, which is a
// battleground queue with no tracker, no chat line and no invitation.
#include <catch_amalgamated.hpp>
#include "game/world_packets.hpp"
#include "core/application.hpp"

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}

using namespace wowee::game;

namespace {

// The bytes AzerothCore's BattlegroundMgr::BuildBattlegroundStatusPacket puts
// on the wire, in its order. Built here rather than asserted against our own
// writer so the test fails if we drift from the server rather than from
// ourselves.
std::vector<uint8_t> serverStatusBytes(uint32_t queueSlot, uint8_t arenaType,
                                       uint32_t bgTypeId, uint8_t minLevel,
                                       uint8_t maxLevel, uint32_t instanceId,
                                       bool isRated, uint32_t statusId,
                                       const std::vector<uint32_t>& tail) {
    std::vector<uint8_t> b;
    auto u32 = [&](uint32_t v) {
        b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8));
        b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 24));
    };
    auto u16 = [&](uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); };

    u32(queueSlot);
    b.push_back(arenaType);
    b.push_back(arenaType ? 0x0E : 0x00);
    u32(bgTypeId);
    u16(0x1F90);
    b.push_back(minLevel);
    b.push_back(maxLevel);
    u32(instanceId);
    b.push_back(isRated ? 1 : 0);
    u32(statusId);
    for (uint32_t v : tail) u32(v);
    return b;
}

wowee::network::Packet packetOf(const std::vector<uint8_t>& body) {
    return wowee::network::Packet(0x2D4, body);  // SMSG_BATTLEFIELD_STATUS, 3.3.5a
}

}  // namespace

TEST_CASE("SMSG_BATTLEFIELD_STATUS reads the status the server sent",
          "[battleground][packet]") {
    SECTION("queued for Warsong Gulch") {
        // Time1 is the average wait, Time2 the time already spent in queue.
        auto p = packetOf(serverStatusBytes(0, 0, 2, 10, 19, 1, false, 1,
                                            {120000, 65000}));
        BattlefieldStatusData d;
        REQUIRE(BattlefieldStatusPacket::parse(p, d));
        CHECK(d.queueSlot == 0u);
        CHECK(d.bgTypeId == 2u);
        CHECK(d.statusId == 1u);          // read two bytes early this was 65536
        CHECK(d.minLevel == 10);
        CHECK(d.maxLevel == 19);
        CHECK(d.instanceId == 1u);
        CHECK(d.avgWaitMs == 120000u);
        CHECK(d.timeInQueueMs == 65000u);
    }

    SECTION("an instance id large enough to reach the status") {
        // The misread joined the instance id's top byte to the status's bottom
        // half, so an instance id in the millions moved the status as well.
        auto p = packetOf(serverStatusBytes(1, 0, 30, 71, 80, 0x01020304, false, 1,
                                            {1000, 2000}));
        BattlefieldStatusData d;
        REQUIRE(BattlefieldStatusPacket::parse(p, d));
        CHECK(d.statusId == 1u);
        CHECK(d.instanceId == 0x01020304u);
        CHECK(d.minLevel == 71);
        CHECK(d.maxLevel == 80);
    }

    SECTION("an invitation carries a map id before its timeout") {
        // status 2: uint32 mapId, uint64 unused, uint32 time to remove from
        // queue. The timeout used to be read from the map id.
        auto p = packetOf(serverStatusBytes(2, 0, 3, 10, 19, 5, false, 2,
                                            {529, 0, 0, 80000}));
        BattlefieldStatusData d;
        REQUIRE(BattlefieldStatusPacket::parse(p, d));
        CHECK(d.statusId == 2u);
        CHECK(d.mapId == 529u);
        CHECK(d.inviteTimeoutMs == 80000u);
    }

    SECTION("before Wrath the invitation has no eight unused bytes") {
        auto p = packetOf(serverStatusBytes(0, 0, 3, 10, 19, 5, false, 2,
                                            {529, 80000}));
        BattlefieldStatusData d;
        REQUIRE(BattlefieldStatusPacket::parse(p, d, /*classicFormat=*/false,
                                               /*wotlkFormat=*/false));
        CHECK(d.mapId == 529u);
        CHECK(d.inviteTimeoutMs == 80000u);
    }

    SECTION("an arena carries its team size") {
        auto p = packetOf(serverStatusBytes(0, 3, 4, 70, 80, 2, true, 1,
                                            {30000, 1000}));
        BattlefieldStatusData d;
        REQUIRE(BattlefieldStatusPacket::parse(p, d));
        CHECK(d.arenaType == 3);
        CHECK(d.isRated);
        CHECK(d.statusId == 1u);
    }

    SECTION("a truncated packet is refused rather than half read") {
        std::vector<uint8_t> body = serverStatusBytes(0, 0, 2, 10, 19, 1, false, 1, {});
        body.resize(10);
        auto p = packetOf(body);
        BattlefieldStatusData d;
        CHECK_FALSE(BattlefieldStatusPacket::parse(p, d));
    }
}

TEST_CASE("SMSG_BATTLEFIELD_STATUS: a zero header is the queue gone", "[battleground]") {
    // BuildBattlegroundStatusPacket for STATUS_NONE: the slot and a uint64 0,
    // and nothing else (0x0054ae40 clears the slot on it). Read as a header,
    // the fields that are not there failed the parse and the queue stayed.
    std::vector<uint8_t> b = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    wowee::network::Packet packet(0x2D4, b);
    BattlefieldStatusData data;
    data.statusId = 1;
    REQUIRE(BattlefieldStatusPacket::parse(packet, data, false, true));
    CHECK(data.queueSlot == 1u);
    CHECK(data.statusId == 0u);
    CHECK(packet.getRemainingSize() == 0);
}

TEST_CASE("SMSG_BATTLEFIELD_LIST as the cores build it", "[battleground]") {
    // BattlegroundMgr::BuildBattlegroundListPacket: battlemaster guid, from
    // where, type, two unknown bytes, the reward block, isRandom (and a
    // second block when it is), then the instance ids (0x0054e390).
    std::vector<uint8_t> b;
    auto u8 = [&](uint8_t v) { b.push_back(v); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); };
    auto u64 = [&](uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); };
    u64(0xF13000384F000ABCull);
    u8(0);           // from the battlemaster
    u32(2);          // Warsong Gulch
    u8(0); u8(0);
    u8(1);           // has won today
    u32(1240); u32(0); u32(310);
    u8(0);           // not random
    u32(3);
    u32(11); u32(12); u32(15);
    wowee::network::Packet packet(0x23D, b);
    BattlefieldListData data;
    REQUIRE(BattlefieldListParser::parse(packet, data));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(data.battlemasterGuid == 0xF13000384F000ABCull);
    CHECK(data.bgTypeId == 2u);
    REQUIRE(data.instanceIds.size() == 3);
    CHECK(data.instanceIds[2] == 15u);
}

TEST_CASE("SMSG_BATTLEFIELD_LIST for the random battleground has a second block", "[battleground]") {
    std::vector<uint8_t> b;
    auto u8 = [&](uint8_t v) { b.push_back(v); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); };
    auto u64 = [&](uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); };
    u64(0);
    u8(1);           // asked for from elsewhere
    u32(32);
    u8(0); u8(0);
    u8(0); u32(0); u32(0); u32(0);
    u8(1);           // random
    u8(1); u32(2000); u32(25); u32(500);
    u32(0);
    wowee::network::Packet packet(0x23D, b);
    BattlefieldListData data;
    REQUIRE(BattlefieldListParser::parse(packet, data));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(data.fromWhere == 1);
    CHECK(data.bgTypeId == 32u);
    CHECK(data.instanceIds.empty());
}

TEST_CASE("MSG_PVP_LOG_DATA: an arena's ratings come before both names", "[battleground]") {
    // Battleground::BuildPvPLogDataPacket for an arena: each team's rating
    // lost, gained and matchmaker rating, then the two names (0x0054d280).
    std::vector<uint8_t> b;
    auto u8 = [&](uint8_t v) { b.push_back(v); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); };
    auto u64 = [&](uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); };
    auto str = [&](const char* s) { while (*s) b.push_back(uint8_t(*s++)); b.push_back(0); };
    u8(1);
    u32(0); u32(14); u32(1532);      // team 0: won 14
    u32(14); u32(0); u32(1490);      // team 1: lost 14
    str("Gold Team");
    str("Green Team");
    u8(1); u8(0);                    // ended, team 0 won
    u32(1);
    u64(0x51);
    u32(2);                          // killing blows
    u8(0);                           // team
    u32(81234); u32(4021);           // damage, healing
    u32(0);                          // no objective values
    wowee::network::Packet packet(0x2E0, b);
    BgScoreboardData data;
    REQUIRE(PvpLogDataParser::parse(packet, data));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(data.isArena);
    CHECK(data.arenaTeams[0].newRating == 14u);
    CHECK(data.arenaTeams[0].matchmakerRating == 1532u);
    CHECK(data.arenaTeams[1].rating == 14u);
    CHECK(data.arenaTeams[1].matchmakerRating == 1490u);
    CHECK(data.arenaTeams[0].teamName == "Gold Team");
    CHECK(data.arenaTeams[1].teamName == "Green Team");
    CHECK(data.hasWinner);
    REQUIRE(data.players.size() == 1);
    CHECK(data.players[0].killingBlows == 2u);
    CHECK(data.players[0].damageDone == 81234u);
    CHECK(data.players[0].healingDone == 4021u);
}
