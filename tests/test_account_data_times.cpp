// SMSG_ACCOUNT_DATA_TIMES, read the fixed way wow.exe reads it (0x006b94c0):
// server time, a byte, the slot mask, then one time per bit set in the mask -
// taken only when exactly that many are left. The bytes are laid out as
// AzerothCore's WorldSession::SendAccountDataTimes writes them.
#include <catch_amalgamated.hpp>

#include <cstdint>

#include "core/application.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}

using namespace wowee::game;
using wowee::network::Packet;

TEST_CASE("The per-character mask carries a time for each of its slots", "[account_data]") {
    // GLOBAL_CACHE_MASK 0x15 at login: slots 0, 2 and 4.
    Packet p(0x209);
    p.writeUInt32(1300000000u);
    p.writeUInt8(1);
    p.writeUInt32(0x15);
    p.writeUInt32(111);
    p.writeUInt32(222);
    p.writeUInt32(333);
    AccountDataTimesData data;
    REQUIRE(AccountDataTimesParser::parse(p, data));
    CHECK(data.serverTime == 1300000000u);
    CHECK(data.unknown == 1);
    CHECK(data.mask == 0x15u);
    CHECK(data.accountDataTimes[0] == 111);
    CHECK(data.accountDataTimes[1] == 0);
    CHECK(data.accountDataTimes[2] == 222);
    CHECK(data.accountDataTimes[3] == 0);
    CHECK(data.accountDataTimes[4] == 333);
    CHECK(p.getRemainingSize() == 0);
}

TEST_CASE("Times that do not match the mask are not read at all", "[account_data]") {
    // A mask of all eight with only two times behind it: the client compares
    // the remainder with the mask's count and reads none of them, where a
    // length guess read the first two into slots 0 and 1.
    Packet p(0x209);
    p.writeUInt32(5);
    p.writeUInt8(0);
    p.writeUInt32(0xFF);
    p.writeUInt32(7);
    p.writeUInt32(8);
    AccountDataTimesData data;
    REQUIRE(AccountDataTimesParser::parse(p, data));
    CHECK(data.mask == 0xFFu);
    for (uint32_t t : data.accountDataTimes) CHECK(t == 0);
}

TEST_CASE("No mask is a packet too short to read", "[account_data]") {
    Packet p(0x209);
    p.writeUInt32(5);
    p.writeUInt8(0);
    AccountDataTimesData data;
    CHECK_FALSE(AccountDataTimesParser::parse(p, data));
}
