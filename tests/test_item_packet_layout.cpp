// SMSG_ITEM_QUERY_SINGLE_RESPONSE as TrinityCore and AzerothCore 3.3.5 build
// it, read as wow.exe 12340 reads the record (0x006354d0 -> 0x0067cbd0 ->
// 0x0098d910): Flags, Flags2, BuyPrice, SellPrice, InventoryType - no
// BuyCount - and from there the requirements, StatsCount and that many stats.
#include <catch_amalgamated.hpp>

#include "core/application.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}  // namespace wowee

using namespace wowee::game;
using wowee::network::Packet;

namespace {

struct Template {
    uint32_t entry = 3000;
    uint32_t itemClass = 4;   // armour
    uint32_t subClass = 1;    // cloth
    uint32_t buyPrice = 100;
    uint32_t sellPrice = 25;
    uint32_t inventoryType = 20;  // robe
    uint32_t allowableClass = 16;  // priest
    uint32_t allowableRace = 0xFFFFFFFF;
    uint32_t itemLevel = 10;
    uint32_t requiredLevel = 5;
    uint32_t armor = 21;
};

/// WorldSession::HandleItemQuerySingleOpcode, field by field.
Packet build(const Template& t) {
    Packet p(0x0058);
    p.writeUInt32(t.entry);
    p.writeUInt32(t.itemClass);
    p.writeUInt32(t.subClass);
    p.writeUInt32(0xFFFFFFFF);  // SoundOverrideSubclass
    p.writeString("Robe of the Acolyte");
    p.writeUInt8(0);
    p.writeUInt8(0);
    p.writeUInt8(0);
    p.writeUInt32(12647);       // DisplayInfoID
    p.writeUInt32(1);           // Quality
    p.writeUInt32(0);           // Flags
    p.writeUInt32(0);           // Flags2
    p.writeUInt32(t.buyPrice);
    p.writeUInt32(t.sellPrice);
    p.writeUInt32(t.inventoryType);
    p.writeUInt32(t.allowableClass);
    p.writeUInt32(t.allowableRace);
    p.writeUInt32(t.itemLevel);
    p.writeUInt32(t.requiredLevel);
    for (int i = 0; i < 8; ++i) p.writeUInt32(0);  // RequiredSkill .. MaxCount
    p.writeUInt32(1);           // Stackable
    p.writeUInt32(0);           // ContainerSlots
    p.writeUInt32(1);           // StatsCount
    p.writeUInt32(5);           // ITEM_MOD_INTELLECT
    p.writeUInt32(2);
    p.writeUInt32(0);           // ScalingStatDistribution
    p.writeUInt32(0);           // ScalingStatValue
    for (int i = 0; i < 2; ++i) {
        p.writeFloat(0.0f);
        p.writeFloat(0.0f);
        p.writeUInt32(0);
    }
    p.writeUInt32(t.armor);
    for (int i = 0; i < 6; ++i) p.writeUInt32(0);
    p.writeUInt32(0);           // Delay
    p.writeUInt32(0);           // AmmoType
    p.writeFloat(0.0f);         // RangedModRange
    for (int s = 0; s < 5; ++s) {
        p.writeUInt32(0);
        p.writeUInt32(0);
        p.writeUInt32(0);
        p.writeUInt32(0xFFFFFFFF);
        p.writeUInt32(0);
        p.writeUInt32(0xFFFFFFFF);
    }
    p.writeUInt32(2);           // Bonding: on equip
    p.writeString("");
    for (int i = 0; i < 4; ++i) p.writeUInt32(0);   // PageText .. StartQuest
    for (int i = 0; i < 12; ++i) p.writeUInt32(0);  // LockID .. TotemCategory
    for (int i = 0; i < 6; ++i) p.writeUInt32(0);   // sockets
    p.writeUInt32(0);           // socketBonus
    p.writeUInt32(0);           // GemProperties
    p.writeUInt32(0);           // RequiredDisenchantSkill
    p.writeFloat(0.0f);         // ArmorDamageModifier
    p.writeUInt32(0);           // Duration
    p.writeUInt32(0);           // ItemLimitCategory
    p.writeUInt32(0);           // HolidayId
    p.setReadPos(0);
    return p;
}

}  // namespace

TEST_CASE("item query: a cheap class-restricted item is read with no BuyCount", "[item_query]") {
    // Bought for a silver, sold for a quarter of it, priests only: both
    // readings of the old guess scored the same and the one with a BuyCount
    // won, so the price became the sell price and the robe a priest-only
    // "inventory type 16".
    Template t;
    auto packet = build(t);
    ItemQueryResponseData data;
    REQUIRE(ItemQueryResponseParser::parse(packet, data));
    CHECK(data.entry == t.entry);
    CHECK(data.sellPrice == t.sellPrice);
    CHECK(data.inventoryType == t.inventoryType);
    CHECK(data.allowableClass == t.allowableClass);
    CHECK(data.itemLevel == t.itemLevel);
    CHECK(data.requiredLevel == t.requiredLevel);
    CHECK(data.intellect == 2);
    CHECK(data.armor == static_cast<int32_t>(t.armor));
    CHECK(data.bindType == 2u);
}

TEST_CASE("item query: an ordinary item reads the same", "[item_query]") {
    Template t;
    t.buyPrice = 120000;
    t.sellPrice = 24000;
    t.inventoryType = 1;
    t.allowableClass = 0xFFFFFFFF;
    t.itemLevel = 200;
    t.requiredLevel = 80;
    auto packet = build(t);
    ItemQueryResponseData data;
    REQUIRE(ItemQueryResponseParser::parse(packet, data));
    CHECK(data.sellPrice == 24000u);
    CHECK(data.inventoryType == 1u);
    CHECK(data.itemLevel == 200u);
    CHECK(data.requiredLevel == 80u);
}
