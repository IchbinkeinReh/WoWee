// The auction house's filter tree: classes, subclasses and slots, and the
// indices FrameXML hands back through QueryAuctionItems.
//
// The three tiers follow the real client: twelve classes (0x00a14f70), every
// visible ItemSubClass.dbc row of a class in record order (0x0059c480), and a
// fourteen-row slot tier under armour's first five subclasses (0x0059c590).
// QueryAuctionItems (0x0059bcf0) resolves each index by the same walk - so a
// list that offers one thing and a lookup that reads another searches the
// wrong items, and nothing says so.
#include <catch_amalgamated.hpp>

#include "game/auction_filters.hpp"

#include <set>
#include <string>

using namespace wowee::game;

namespace {

std::vector<const AuctionSubClassRow*> listed(int classIndex) {
    const auto* cls = auctionClassAt(classIndex);
    if (!cls || !cls->hasSubClasses) return {};
    return auctionSubClassRows(kItemSubClassFallback, std::size(kItemSubClassFallback), cls->classId);
}

int classIndexOf(uint32_t classId) {
    for (int i = 1; i < kNumAuctionClasses; ++i) {
        if (kAuctionClasses[i].classId == classId) return i;
    }
    return 0;
}

std::set<uint32_t> offeredUnder(uint32_t classId, int subPosition) {
    std::set<uint32_t> out;
    for (int i = 0; i < kNumAuctionInvTypes; ++i) {
        if (auctionInvTypeOffered(classId, subPosition, i)) out.insert(kAuctionInvTypes[i].invType);
    }
    return out;
}

}  // namespace

TEST_CASE("eleven of the twelve classes are divided", "[auction-filter]") {
    int divided = 0;
    for (int i = 1; i < kNumAuctionClasses; ++i) {
        INFO(kAuctionClasses[i].label);
        if (!listed(i).empty()) ++divided;
    }
    CHECK(divided == 11);
    CHECK(listed(classIndexOf(12)).empty());   // Quest
    CHECK(listed(0).empty());
    CHECK(listed(kNumAuctionClasses).empty());
}

TEST_CASE("subclasses are listed in record order, hidden rows left out", "[auction-filter]") {
    // Consumables in the file: 0 (hidden), 5, 1, 2, 3, 7, 6, 4, 8.
    const auto cons = listed(classIndexOf(0));
    REQUIRE(cons.size() == 8);
    CHECK(cons[0]->subId == 5);   // Food & Drink comes first, not Potion
    CHECK(cons[1]->subId == 1);
    CHECK(cons[7]->subId == 8);

    // Weapons: 9, 11, 12 and 17 are hidden; 14 (Miscellaneous) and 20
    // (Fishing Poles) are listed.
    const auto weap = listed(classIndexOf(2));
    std::set<uint32_t> ids;
    for (const auto* r : weap) ids.insert(r->subId);
    CHECK(weap.size() == 17);
    CHECK(ids.count(9) == 0);
    CHECK(ids.count(17) == 0);
    CHECK(ids.count(14) == 1);
    CHECK(ids.count(20) == 1);

    // Armour: the buckler row is hidden, so Shields is the sixth.
    const auto armor = listed(classIndexOf(4));
    REQUIRE(armor.size() == 10);
    CHECK(armor[5]->subId == 6);
}

TEST_CASE("only the subclasses flagged 0x200 have a slot tier", "[auction-filter]") {
    for (int ci = 1; ci < kNumAuctionClasses; ++ci) {
        for (const auto* r : listed(ci)) {
            const bool hasSlots = (r->flags & kAuctionSubClassHasSlots) != 0;
            INFO(kAuctionClasses[ci].label << " / " << r->label);
            CHECK(hasSlots == (r->classId == 4 && r->subId <= 4));
        }
    }
}

TEST_CASE("armour's slots are split the way the original splits them", "[auction-filter]") {
    // Miscellaneous: what is worn but not made of cloth or plate.
    CHECK(offeredUnder(4, 0) == std::set<uint32_t>{1, 2, 4, 11, 12, 23});
    // Cloth: the worn slots and the back.
    CHECK(offeredUnder(4, 1) == std::set<uint32_t>{1, 3, 5, 6, 7, 8, 9, 10, 16});
    // Leather, Mail and Plate: the worn slots without the back.
    for (int sub : {2, 3, 4}) {
        INFO("subclass position " << sub);
        CHECK(offeredUnder(4, sub) == std::set<uint32_t>{1, 3, 5, 6, 7, 8, 9, 10});
    }
}

TEST_CASE("a slot index is a row of all fourteen", "[auction-filter]") {
    // FrameXML numbers a slot by its pair position in GetAuctionInvTypes'
    // answer, hidden rows included, so Cloth > Chest comes back as 5 and
    // Cloth > Back as 13.
    CHECK(auctionInvTypeIdAt(1) == 1);
    CHECK(auctionInvTypeIdAt(5) == 5);
    CHECK(auctionInvTypeIdAt(11) == 11);
    CHECK(auctionInvTypeIdAt(13) == 16);
    CHECK(auctionInvTypeIdAt(14) == 23);
    CHECK(auctionInvTypeIdAt(0) == kAuctionAny);
    CHECK(auctionInvTypeIdAt(15) == kAuctionAny);
    CHECK(auctionInvTypeIdAt(-1) == kAuctionAny);
    for (int i = 0; i < kNumAuctionInvTypes; ++i) {
        CHECK(std::string(kAuctionInvTypes[i].token).rfind("INVTYPE_", 0) == 0);
    }
}

TEST_CASE("the class list is the original's", "[auction-filter]") {
    constexpr uint32_t want[] = {2, 4, 1, 0, 16, 7, 6, 11, 9, 3, 15, 12};
    for (int i = 0; i < 12; ++i) CHECK(kAuctionClasses[i + 1].classId == want[i]);
    CHECK(auctionClassAt(0) == nullptr);
    CHECK(auctionClassAt(13) == nullptr);
    CHECK(auctionClassAt(12)->classId == 12);
}
