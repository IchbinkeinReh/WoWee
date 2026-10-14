// SMSG_LOOT_RESPONSE as 0x006d53b0 reads it: guid, loot type, gold, a count
// and that many 22-byte items. Nothing after them - the client reads no
// quest-item list, and TrinityCore/AzerothCore 3.3.5 send none.
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

struct Bytes {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u32(uint32_t v) {
        b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8));
        b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 24));
    }
    void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
    /// One loot item, 22 bytes, the same on every expansion.
    void item(uint8_t slot, uint32_t itemId, uint32_t count) {
        u8(slot); u32(itemId); u32(count);
        u32(0);      // displayInfo
        u32(0);      // randomSuffix
        u32(0);      // randomPropertyId
        u8(0);       // lootSlotType
    }
};

wowee::network::Packet packetOf(const std::vector<uint8_t>& body) {
    return wowee::network::Packet(0x160, body);  // SMSG_LOOT_RESPONSE
}

/// One chest holding one stack, as the cores build it.
std::vector<uint8_t> chest(uint64_t guid) {
    Bytes p;
    p.u64(guid);
    p.u8(3);            // lootType
    p.u32(120);         // gold
    p.u8(1);            // one item
    p.item(0, 1673, 2);
    return p.b;
}

}  // namespace

TEST_CASE("a chest's gold and items are read", "[loot]") {
    const auto body = chest(0xF110000689000699ull);
    auto packet = packetOf(body);
    LootResponseData data;
    REQUIRE(LootResponseParser::parse(packet, data));
    CHECK(data.lootGuid == 0xF110000689000699ull);
    CHECK(data.gold == 120);
    REQUIRE(data.items.size() == 1);
    CHECK(data.items[0].itemId == 1673);
    CHECK(data.items[0].count == 2);
}

TEST_CASE("bytes after the items are not a quest list", "[loot]") {
    // The client reads nothing past the items, so neither does WoWee: a byte
    // there is not taken for a count and adds no items.
    auto body = chest(0x99ull);
    body.push_back(1);
    for (int i = 0; i < 22; ++i) body.push_back(0x11);
    auto packet = packetOf(body);
    LootResponseData data;
    REQUIRE(LootResponseParser::parse(packet, data));
    REQUIRE(data.items.size() == 1);
    CHECK(data.items[0].itemId == 1673);
}

TEST_CASE("a refusal is not loot", "[loot]") {
    // guid + lootType and nothing else: the realm answered the use and offered
    // nothing. The caller must not open a window on it.
    Bytes p;
    p.u64(0x1234ull);
    p.u8(0);

    auto packet = packetOf(p.b);
    LootResponseData data;
    CHECK_FALSE(LootResponseParser::parse(packet, data));
}

TEST_CASE("a truncated regular list is refused outright", "[loot]") {
    // Nothing trustworthy was read, so there is nothing to keep.
    Bytes p;
    p.u64(0x1ull);
    p.u8(3);
    p.u32(0);
    p.u8(2);              // claims two
    p.item(0, 777, 1);    // sends one

    auto packet = packetOf(p.b);
    LootResponseData data;
    CHECK_FALSE(LootResponseParser::parse(packet, data));
}
