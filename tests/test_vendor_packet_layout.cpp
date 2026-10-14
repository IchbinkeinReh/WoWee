// SMSG_LIST_INVENTORY as TrinityCore/AzerothCore 3.3.5 build it
// (WorldSession::SendListInventory), read as 0x006d9960 reads it: the vendor
// guid, a count, and eight u32s per item - or, with no items, a reason byte.
#include <catch_amalgamated.hpp>

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
    void item(uint32_t slot, uint32_t id, uint32_t display, int32_t left, uint32_t price,
              uint32_t durability, uint32_t buyCount, uint32_t extendedCost) {
        u32(slot); u32(id); u32(display); u32(static_cast<uint32_t>(left)); u32(price);
        u32(durability); u32(buyCount); u32(extendedCost);
    }
    wowee::network::Packet packet() const { return wowee::network::Packet(0x19F, bytes); }
};

}  // namespace

TEST_CASE("vendor: every item is 32 bytes, extended cost and all", "[vendor]") {
    Writer w;
    w.u64(0xF130000123000045ull);
    w.u8(3);
    w.item(1, 159, 18084, -1, 25, 0, 5, 0);       // Refreshing Spring Water
    w.item(2, 4540, 6399, -1, 25, 0, 5, 0);       // Tough Hunk of Bread
    w.item(3, 40753, 0, 2, 0, 0, 1, 2589);        // a badge-priced item
    auto packet = w.packet();
    ListInventoryData data;
    REQUIRE(ListInventoryParser::parse(packet, data, true));
    CHECK(packet.getRemainingSize() == 0);
    CHECK(data.vendorGuid == 0xF130000123000045ull);
    REQUIRE(data.items.size() == 3);
    CHECK(data.items[0].itemId == 159u);
    CHECK(data.items[0].maxCount == -1);
    CHECK(data.items[1].itemId == 4540u);
    CHECK(data.items[2].itemId == 40753u);
    CHECK(data.items[2].maxCount == 2);
    CHECK(data.items[2].extendedCost == 2589u);
}

TEST_CASE("vendor: bytes after a short list are not taken as a 28-byte layout", "[vendor]") {
    // Guessing the layout from the size behind the count read two 32-byte
    // items as 28-byte ones when a few bytes were missing; the client reads
    // its fixed size and refuses what does not fit.
    Writer w;
    w.u64(0x1);
    w.u8(2);
    w.item(1, 100, 1, -1, 10, 0, 1, 0);
    for (int i = 0; i < 24; ++i) w.u8(0);
    auto packet = w.packet();
    ListInventoryData data;
    CHECK_FALSE(ListInventoryParser::parse(packet, data, true));
}

TEST_CASE("vendor: an empty list carries a reason, and only reason 0 opens", "[vendor]") {
    {
        Writer w;
        w.u64(0x2);
        w.u8(0);
        w.u8(0);  // "Vendor has no inventory"
        auto packet = w.packet();
        ListInventoryData data;
        CHECK(ListInventoryParser::parse(packet, data, true));
        CHECK(data.items.empty());
    }
    {
        Writer w;
        w.u64(0x2);
        w.u8(0);
        w.u8(3);  // "Vendor is dead"
        auto packet = w.packet();
        ListInventoryData data;
        CHECK_FALSE(ListInventoryParser::parse(packet, data, true));
    }
}
