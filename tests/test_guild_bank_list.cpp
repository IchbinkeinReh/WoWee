// SMSG_GUILD_BANK_LIST, whose tab-name block is optional.
//
// 0x005a7250 reads the tab names only when the list is a full update of tab
// zero, which is when TrinityCore and AzerothCore 3.3.5 write them
// (`sendAllSlots && !tabId`). A full update of any other tab has the item
// count straight after the flag.
//
// The shapes are pinned here because nothing about a mis-parse raises - the
// bytes are all valid, they are just read a block early or late.
#include <catch_amalgamated.hpp>

#include <cstdint>
#include <iterator>
#include <cstring>
#include <string>
#include <vector>

#include "core/application.hpp"
#include "game/world_packets.hpp"

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}

using namespace wowee::game;

namespace {

struct Writer {
    std::vector<uint8_t> bytes;
    void u8(uint8_t v) { bytes.push_back(v); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    void cstr(const std::string& s) {
        bytes.insert(bytes.end(), s.begin(), s.end());
        bytes.push_back(0);
    }
    /// One occupied slot, in the order the reader expects it.
    void item(uint8_t slotId, uint32_t entry, uint32_t stack) {
        u8(slotId);
        u32(entry);
        u32(0);        // flags
        u32(0);        // randomPropertyId - zero, so no seed follows
        u32(stack);
        u32(0);        // permanent enchantment
        u8(0);         // charges
        u8(0);         // sockets
    }
};

/// money, tabId, remaining withdrawals, then the optional tab block.
Writer header(uint8_t tabId, bool withTabInfo,
              const std::vector<std::string>& tabNames) {
    Writer w;
    w.u64(123456);
    w.u8(tabId);
    w.u32(7);                    // withdrawals remaining
    w.u8(withTabInfo ? 1 : 0);
    if (withTabInfo) {
        w.u8(static_cast<uint8_t>(tabNames.size()));
        for (const std::string& name : tabNames) {
            w.cstr(name);
            w.cstr("Interface\\Icons\\INV_Misc_Bag_08");
        }
    }
    return w;
}

GuildBankData parse(Writer& w) {
    wowee::network::Packet packet(0, std::move(w.bytes));
    GuildBankData data;
    REQUIRE(GuildBankListParser::parse(packet, data));
    return data;
}

}  // namespace

TEST_CASE("the opening list carries the tabs and the first tab's items") {
    Writer w = header(0, true, {"Tab One", "Tab Two"});
    w.u8(2);
    w.item(0, 6948, 1);
    w.item(3, 4306, 20);

    const GuildBankData data = parse(w);
    CHECK(data.money == 123456u);
    CHECK(data.tabId == 0);
    CHECK(data.withdrawAmount == 7);
    CHECK(data.tabsIncluded);
    REQUIRE(data.tabs.size() == 2);
    CHECK(data.tabs[0].tabName == "Tab One");
    CHECK(data.tabs[1].tabName == "Tab Two");
    REQUIRE(data.tabItems.size() == 2);
    CHECK(data.tabItems[0].slotId == 0);
    CHECK(data.tabItems[0].itemEntry == 6948u);
    CHECK(data.tabItems[1].slotId == 3);
    CHECK(data.tabItems[1].itemEntry == 4306u);
    CHECK(data.tabItems[1].stackCount == 20u);
}

TEST_CASE("a full update of a later tab carries no names") {
    // Taking the flag alone as the names' marker read the item count as a tab
    // count and the items as names, and the tab drew empty.
    Writer w = header(2, false, {});
    w.bytes.back() = 1;          // full update, no names: tab 2
    w.u8(1);
    w.item(5, 2589, 12);

    const GuildBankData data = parse(w);
    CHECK(data.tabId == 2);
    CHECK_FALSE(data.tabsIncluded);
    REQUIRE(data.tabItems.size() == 1);
    CHECK(data.tabItems[0].slotId == 5);
    CHECK(data.tabItems[0].itemEntry == 2589u);
    CHECK(data.tabItems[0].stackCount == 12u);
}

TEST_CASE("a slots-only update leaves the tabs it found") {
    Writer first = header(0, true, {"Tab One", "Tab Two"});
    first.u8(0);
    wowee::network::Packet firstPacket(0, std::move(first.bytes));
    GuildBankData data;
    REQUIRE(GuildBankListParser::parse(firstPacket, data));
    REQUIRE(data.tabs.size() == 2);

    Writer second = header(1, false, {});
    second.u8(1);
    second.item(9, 1234, 3);
    wowee::network::Packet secondPacket(0, std::move(second.bytes));
    REQUIRE(GuildBankListParser::parse(secondPacket, data));

    CHECK_FALSE(data.tabsIncluded);
    CHECK(data.tabs.size() == 2);          // kept, not cleared
    CHECK(data.tabId == 1);
    REQUIRE(data.tabItems.size() == 1);
    CHECK(data.tabItems[0].itemEntry == 1234u);
}

TEST_CASE("an empty slot carries nothing after its entry") {
    Writer w = header(0, true, {"Only Tab"});
    w.u8(2);
    w.u8(4);
    w.u32(0);          // empty: no flags, no stack, no enchant
    w.item(6, 858, 5);

    const GuildBankData data = parse(w);
    REQUIRE(data.tabItems.size() == 2);
    CHECK(data.tabItems[0].itemEntry == 0u);
    CHECK(data.tabItems[1].slotId == 6);
    CHECK(data.tabItems[1].itemEntry == 858u);
    CHECK(data.tabItems[1].stackCount == 5u);
}

// MSG_GUILD_BANK_MONEY_WITHDRAWN's answer: one int32, the money the player may
// still take today, -1 for no limit (0x005a4ab0 reads it into 0x00c1dc34).
TEST_CASE("the money left to withdraw is one int32, -1 for no limit") {
    const auto read = [](std::vector<uint8_t> bytes, int32_t& out) {
        wowee::network::Packet packet(0, std::move(bytes));
        const bool ok = GuildBankMoneyWithdrawnParser::parse(packet, out);
        if (ok) CHECK_FALSE(packet.hasData());   // all four bytes, nothing else
        return ok;
    };
    int32_t remaining = 0;
    REQUIRE(read({0x40, 0x0D, 0x03, 0x00}, remaining));   // 200000 copper, 20g
    CHECK(remaining == 200000);
    REQUIRE(read({0xFF, 0xFF, 0xFF, 0xFF}, remaining));
    CHECK(remaining == -1);

    // Cut short: nothing read, the last answer kept.
    remaining = 5;
    CHECK_FALSE(read({0x01, 0x02, 0x03}, remaining));
    CHECK(remaining == 5);
}

// MSG_GUILD_PERMISSIONS's answer (0x005cb9f0): rank id, rights, gold a day,
// the tabs bought (one byte), then six pairs of tab flags and items a day.
TEST_CASE("the guild permissions are the rank, its rights and six tab pairs") {
    std::vector<uint8_t> bytes = {
        0x02, 0x00, 0x00, 0x00,   // rank 2
        0x43, 0x00, 0x0C, 0x00,   // rights 0x000C0043
        0x40, 0x0D, 0x03, 0x00,   // 200000 copper a day
        0x03,                     // three tabs bought
    };
    for (uint8_t t = 0; t < 6; ++t) {
        const uint8_t flags[] = {static_cast<uint8_t>(t + 1), 0x00, 0x00, 0x00};
        const uint8_t items[] = {static_cast<uint8_t>(10 * t), 0x00, 0x00, 0x00};
        bytes.insert(bytes.end(), std::begin(flags), std::end(flags));
        bytes.insert(bytes.end(), std::begin(items), std::end(items));
    }
    REQUIRE(bytes.size() == 61u);
    {
        wowee::network::Packet packet(0, bytes);
        GuildPermissionsData data;
        REQUIRE(GuildPermissionsParser::parse(packet, data));
        CHECK_FALSE(packet.hasData());   // all 61 bytes, nothing else
        CHECK(data.rankId == 2u);
        CHECK(data.rank.rights == 0x000C0043u);
        CHECK(data.rank.goldLimit == 200000u);
        CHECK(data.purchasedTabs == 3u);
        for (size_t t = 0; t < 6; ++t) {
            CHECK(data.rank.bankTabRights[t] == t + 1);
            CHECK(data.rank.bankTabSlotsPerDay[t] == 10 * t);
        }
    }
    // Cut short by a byte: nothing read, what was there kept.
    bytes.pop_back();
    wowee::network::Packet packet(0, bytes);
    GuildPermissionsData data;
    data.rankId = 7;
    CHECK_FALSE(GuildPermissionsParser::parse(packet, data));
    CHECK(data.rankId == 7u);
}
