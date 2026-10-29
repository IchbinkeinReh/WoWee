#include <catch_amalgamated.hpp>

#include "game/update_field_table.hpp"
#include "game/known_currencies.hpp"
#include "game/currency_categories.hpp"
#include "game/known_titles.hpp"
#include "game/quest_rewards.hpp"

#include <filesystem>
#include <string>
#include <vector>

// The client reads update-field values by a logical name, resolved through a
// per-expansion offset file. If a name in the JSON stops matching the enum, the
// loader silently skips it and fieldIndex returns 0xFFFF forever after - the
// field reads as absent and its stat shows zero, with nothing to fail. These
// are the offsets the character sheet depends on; pin the ones a wrong value
// would silently zero. The mana-regen pair is derived as OBJECT_END(6)+0x22 and
// +0x29, cross-checked against MAXPOWER1 = OBJECT_END+0x1B = 33.

using wowee::game::UpdateFieldTable;
using wowee::game::UF;

namespace {
std::string fieldsPath(const std::string& expansion) {
    return (std::filesystem::path(WOWEE_SOURCE_DIR) /
            "Data" / "expansions" / expansion / "update_fields.json").string();
}
} // namespace

TEST_CASE("Every expansion ships a loadable update-field table", "[updatefields]") {
    for (const auto& expansion : {"classic", "tbc", "wotlk", "turtle"}) {
        INFO("expansion: " << expansion);
        UpdateFieldTable table;
        REQUIRE(table.loadFromJson(fieldsPath(expansion)));
    }
}

TEST_CASE("WotLK mana-regen fields resolve to their wire offsets", "[updatefields]") {
    UpdateFieldTable table;
    REQUIRE(table.loadFromJson(fieldsPath("wotlk")));

    // Anchors: if these drift the file changed shape and the derivation below
    // no longer holds.
    REQUIRE(table.index(UF::UNIT_FIELD_MAXPOWER1) == 33);          // OBJECT_END + 0x1B
    REQUIRE(table.index(UF::UNIT_FIELD_ATTACK_POWER) == 123);

    // The pair GetManaRegen reads. Mana is power index 0, so each sits at the
    // base of its seven-wide array.
    REQUIRE(table.index(UF::UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER) == 40);              // + 0x22
    REQUIRE(table.index(UF::UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER) == 47);  // + 0x29
}

// The token tab draws its rows out of PLAYER_FIELD_KNOWN_CURRENCIES, and a wrong
// offset there does not raise either: the mask reads as zero and the tab comes up
// empty, or it reads inside some other field's bits and every currency in
// CurrencyTypes.dbc is listed as known. The offset was read off the client's own
// blob position - the pair sits at +0x790 and +0x794, blob dwords 484 and 485,
// and this layout puts a blob dword 148 further along on the wire. The same
// arithmetic put the honour counter beside it at 1277 (blob +0x11a4), and the
// client names the pair with one name, not two: PLAYER_FIELD_KNOWN_CURRENCIES is
// the only currency field in its own name table.
TEST_CASE("The known-currency pair is where the client reads it",
          "[updatefields][currency]") {
    UpdateFieldTable table;
    REQUIRE(table.loadFromJson(fieldsPath("wotlk")));

    const uint16_t known = table.index(UF::PLAYER_FIELD_KNOWN_CURRENCIES);
    REQUIRE(known != 0xFFFF);
    REQUIRE(known == 632);
    // The pair's other half is the next dword, which nothing here claims: PLAYER_XP
    // begins two past it, so 632 and 633 are the only free indices beside it.
    REQUIRE(table.index(UF::PLAYER_XP) == known + 2);

    // No earlier layout names the field, and on those the token tab falls back to
    // the currencies held. That fallback has to stay reachable, which means the
    // name has to stay absent rather than resolve to something nearby.
    for (const auto* expansion : {"classic", "tbc", "turtle"}) {
        INFO("expansion: " << expansion);
        UpdateFieldTable other;
        REQUIRE(other.loadFromJson(fieldsPath(expansion)));
        REQUIRE(other.index(UF::PLAYER_FIELD_KNOWN_CURRENCIES) == 0xFFFF);
    }
}

// Which bit of which half a currency id uses is the whole of what the mask is
// worth. Off by one in either direction and one particular currency is hidden
// from the tab, or one is listed that the character never earned, with every
// other row looking exactly as it should.
TEST_CASE("The known-currency bits are one-based across a 64-wide pair",
          "[updatefields][currency]") {
    using wowee::game::currencyIsKnown;

    // The ends of each half, and each half keeping to its own ids.
    CHECK(currencyIsKnown(0x00000001u, 0x00000000u, 1));
    CHECK(currencyIsKnown(0x80000000u, 0x00000000u, 32));
    CHECK(currencyIsKnown(0x00000000u, 0x00000001u, 33));
    CHECK(currencyIsKnown(0x00000000u, 0x80000000u, 64));
    CHECK_FALSE(currencyIsKnown(0x80000000u, 0x00000000u, 33));
    CHECK_FALSE(currencyIsKnown(0x00000000u, 0x00000001u, 32));

    // A bit that is not the one asked for.
    CHECK_FALSE(currencyIsKnown(0x00000002u, 0x00000000u, 1));

    // Currency ids begin at 1, and an id outside the pair is not known however
    // full the masks are. The client's own test bounds only the low half, so an
    // id past 0x40 shifts the high half's mask past its own width; asking for an
    // id inside the pair is what keeps a row that no mask can mean out.
    CHECK_FALSE(currencyIsKnown(0xFFFFFFFFu, 0xFFFFFFFFu, 0));
    CHECK_FALSE(currencyIsKnown(0xFFFFFFFFu, 0xFFFFFFFFu, 65));
}

// The currencies watched beside the bags are a second pair of the same masks, and
// they are the only thing deciding which currency stands beside the bags: the
// token window reads the flag back to tick its own box, and
// blizzard_tokenui.lua counts what GetBackpackCurrencyInfo answers for the slots
// it draws. A bit landing in the wrong half names a different currency for that
// slot, and a set that clears the half instead of the bit empties the other rows.
TEST_CASE("The watched pair sets one currency at a time", "[updatefields][currency]") {
    using wowee::game::CurrencyWatch;

    CurrencyWatch watch;
    CHECK(watch.low == 0);
    CHECK(watch.high == 0);

    // The ends of both halves, addressed as the known pair addresses them - which
    // is how one currency's bit means the same row in either mask.
    watch.set(1, true);
    watch.set(32, true);
    watch.set(33, true);
    watch.set(64, true);
    CHECK(watch.low == 0x80000001u);   // id 1 opens the low half, id 32 closes it
    CHECK(watch.high == 0x80000001u);  // id 33 opens the other half, id 64 closes it
    CHECK(watch.has(1));
    CHECK(watch.has(32));
    CHECK(watch.has(33));
    CHECK(watch.has(64));
    CHECK_FALSE(watch.has(2));
    CHECK_FALSE(watch.has(63));

    // Clearing one leaves its neighbours standing.
    watch.set(32, false);
    CHECK(watch.low == 0x00000001u);
    CHECK(watch.high == 0x80000001u);
    CHECK(watch.has(64));

    // Setting an id twice is still one row; an id outside the pair has no bit to
    // set and must not disturb what is already there; and clearing an id that was
    // never watched changes nothing. The client enforces no cap of its own
    // (0x005b0590 asks nothing of the rest of the pair), so nothing here either.
    watch.set(64, true);
    watch.set(0, true);
    watch.set(65, true);
    CHECK(watch.high == 0x80000001u);
    watch.set(7, false);
    CHECK(watch.low == 0x00000001u);
}

// Which field of a CurrencyTypes.dbc row holds the currency's id is not a naming
// question: it decides whether the realm's mask names any row at all. The client
// keys all three of its masks - the realm's known pair, the watched pair it writes
// through currencyTokensBackpack1/2, and the unused pair - on the record's fourth
// field (0x005afe30, 0x005b0030 and 0x005b04f0/0x005b0590 each compute the bit from
// it), takes the item the amount comes from out of the second (0x005b0a90 compares
// it against 43307 and 43308 (0xa92b, 0xa92c), and 0x005affd0 finds a table row by matching it), and
// files the row by the third (0x005afe30 against the category store's first field).
// The record's first field is read by none of them, and it cannot be the id: the
// masks are two dwords wide, so every id one of them addresses is inside 1..64.
TEST_CASE("A CurrencyTypes row is keyed by its fourth field", "[updatefields][currency]") {
    using wowee::game::currencyBitOf;
    using wowee::game::kCurrencyTypeFieldCategory;
    using wowee::game::kCurrencyTypeFieldId;
    using wowee::game::kCurrencyTypeFieldItem;
    using wowee::game::kCurrencyTypeFields;

    static_assert(kCurrencyTypeFields == 4);
    CHECK(kCurrencyTypeFieldId == 3);
    CHECK(kCurrencyTypeFieldItem == 1);
    CHECK(kCurrencyTypeFieldCategory == 2);
    // The offsets those routines read, in the bytes the client's record view uses.
    CHECK(kCurrencyTypeFieldId * 4 == 0x0c);
    CHECK(kCurrencyTypeFieldItem * 4 == 0x04);
    CHECK(kCurrencyTypeFieldCategory * 4 == 0x08);

    // The reach that rules the first field out, without naming what is in this or
    // any other build's table: 64 is the last id a pair of dwords has a bit for,
    // and an id one past it is answered not-known rather than read. A field whose
    // values run past that is not a mask key - it is a lookup the mask cannot make.
    CHECK(currencyBitOf(64).valid);
    CHECK_FALSE(currencyBitOf(65).valid);
    CHECK_FALSE(currencyBitOf(341).valid);
    CHECK_FALSE(currencyBitOf(0).valid);
}

// The token tab's list is a walk of the categories, not a flat list, and two of
// its rules are position rules rather than name rules: the category carrying flag
// bit 0 is the one the client files the unused currencies under, and it is found by
// being last (0x005afe30 tests param_1 == count - 1). So the store's order is what
// decides which category plays that role, and 0x005afc40 puts a flagged category
// after every unflagged one and compares the rest by name.
TEST_CASE("The category that sorts last is the unused bucket", "[updatefields][currency]") {
    using wowee::game::categorySortsLast;
    using wowee::game::CurrencyCategory;
    using wowee::game::currencyCategoryLess;
    using wowee::game::currencyBelongsHere;
    using wowee::game::categoryIsDrawn;

    CHECK_FALSE(categorySortsLast(0));
    CHECK(categorySortsLast(1));
    CHECK(categorySortsLast(3));   // the value this build's table actually carries

    // Names first, the flagged one after them whatever its name would sort to.
    const CurrencyCategory classic{.id = 4,  .flags = 0, .name = "Classic"};
    const CurrencyCategory burning{.id = 41, .flags = 0, .name = "Burning Crusade"};
    const CurrencyCategory unused{.id = 3,  .flags = 3, .name = "Aardvark"};
    CHECK(currencyCategoryLess(burning, classic));
    CHECK(currencyCategoryLess(classic, unused));
    CHECK(currencyCategoryLess(burning, unused));
    CHECK_FALSE(currencyCategoryLess(unused, classic));

    // Which category a currency lands in (0x005b0030's two legs). An ordinary
    // category takes its own id with the unused bit clear; the bucket takes the
    // unused bit and nothing else. A currency filed under the bucket's own id but
    // not marked unused belongs to neither leg, which is what the client does too -
    // it has no row.
    CHECK(currencyBelongsHere(false, 4, 4, false));
    CHECK_FALSE(currencyBelongsHere(false, 4, 4, true));    // marked: not here any more
    CHECK_FALSE(currencyBelongsHere(false, 22, 4, false));  // someone else's
    CHECK(currencyBelongsHere(true, 4, 3, true));           // the bucket takes any id
    CHECK_FALSE(currencyBelongsHere(true, 3, 3, false));

    // And whether a header is drawn at all (0x005afe30, which returns as soon as
    // one row answers). A category draws for a known currency filed under it; the
    // bucket draws for a known currency marked unused; an unknown currency draws
    // nothing anywhere.
    CHECK(categoryIsDrawn(true, true, false, false));
    CHECK_FALSE(categoryIsDrawn(true, false, false, false));
    CHECK_FALSE(categoryIsDrawn(false, true, false, true));   // marked, but not this one
    CHECK(categoryIsDrawn(false, true, true, true));
    CHECK_FALSE(categoryIsDrawn(false, true, true, false));   // known, unmarked: not the bucket
    // The bucket's leg never looks at the known pair at all (0x005afe30 asks
    // `bVar7 && (unused & bit)` on its own), so a currency marked unused draws the
    // bucket's header even if the realm says nothing about it. The rows under it
    // are a different test - the builder requires the known bit there - which is
    // why the client can draw a heading with nothing below it.
    CHECK(categoryIsDrawn(false, false, true, true));
}

// Which columns of CurrencyCategory.dbc the loader reads, pinned because two of
// them are not the obvious ones: the flags are their own column, and the name is
// the locale block's first column only after pipeline/dbc_loader.cpp has promoted
// the active locale's offset into it. Verified through that loader against this
// build's table - the promoted name lands at 2 and the flags column keeps its
// value, which only one row of eight has.
TEST_CASE("CurrencyCategory's flags and promoted name columns", "[updatefields][currency]") {
    using wowee::game::kCurrencyCategoryFieldFlag;
    using wowee::game::kCurrencyCategoryFieldId;
    using wowee::game::kCurrencyCategoryFieldName;
    using wowee::game::kCurrencyCategoryFields;

    CHECK(kCurrencyCategoryFieldId == 0);
    CHECK(kCurrencyCategoryFieldFlag == 1);
    // Not 1: the block starts after the flags, so promotion lands on 2. Reading 1
    // would hand back the flags word as a name and the name as nothing.
    CHECK(kCurrencyCategoryFieldName == 2);
    static_assert(kCurrencyCategoryFields == 3);

    // The unused pair is a pair of the same masks as the watched one, keyed by the
    // same ids - so marking a currency must not disturb what is watched, or the
    // token row beside the bags would move when a currency is hidden.
    wowee::game::CurrencyWatch watched, unusedSet;
    watched.set(7, true);
    unusedSet.set(7, true);
    CHECK(watched.has(7));
    CHECK(unusedSet.has(7));
    unusedSet.set(7, false);
    CHECK(watched.has(7));
    CHECK_FALSE(unusedSet.has(7));
}

// The character sheet's title dropdown is built by walking 1..GetNumTitles() and
// asking IsTitleKnown about each, and that answers out of
// PLAYER_FIELD_KNOWN_TITLES: six dwords holding three 64-bit groups, at blob
// +0x778 through +0x78f - the run ends exactly where the known-currency pair
// above begins. A wrong offset fails quietly in both directions: the field reads
// as absent and the dropdown lists no title however many were earned, or it
// reads inside some other field and the list is full of titles the character
// never had. Blob dword 478 is wire 626 in this layout; the two dwords before
// the run are the farsight unit, and the currency pair is the two after it.
TEST_CASE("The known-titles run is six dwords before the known-currency pair",
          "[updatefields][titles]") {
    // The size the sheet sizes its loop to (GetNumTitles) and the size the
    // addressing below divides the run into are one number.
    static_assert(wowee::game::kKnownTitleCount == 192);

    UpdateFieldTable table;
    REQUIRE(table.loadFromJson(fieldsPath("wotlk")));

    const uint16_t titles = table.index(UF::PLAYER_FIELD_KNOWN_TITLES);
    REQUIRE(titles != 0xFFFF);
    REQUIRE(titles == 626);

    // The run's own ends, which is the check worth having: the client's field
    // table names the run's three pairs separately (PLAYER__FIELD_KNOWN_TITLES,
    // _TITLES1 and _TITLES2, at 626, 628 and 630), while this layout names only
    // its first dword - so the five after it belong to nothing here and have to
    // stay free rather than resolve into a neighbouring field.
    CHECK(table.index(UF::PLAYER_FARSIGHT) == titles - 2);
    CHECK(table.index(UF::PLAYER_FIELD_KNOWN_CURRENCIES) == titles + 6);

    // No earlier layout names the run, and on those the titles earned in this
    // session stay the answer. As with the currency pair, that fallback stays
    // reachable only while the name stays absent: a nearby name would resolve,
    // and the mask would be read out of some other field's bits.
    for (const auto* expansion : {"classic", "tbc", "turtle"}) {
        INFO("expansion: " << expansion);
        UpdateFieldTable other;
        REQUIRE(other.loadFromJson(fieldsPath(expansion)));
        REQUIRE(other.index(UF::PLAYER_FIELD_KNOWN_TITLES) == 0xFFFF);
    }
}

// Which bit of which dword a title id is decides one row of the dropdown at a
// time. A group off by one moves every title in the list by 64 places, and the
// list still looks like a list of titles - which is why the ends are pinned
// rather than a handful of ids in the middle.
TEST_CASE("A title id is one bit in one of three groups", "[updatefields][titles]") {
    using wowee::game::KnownTitleWords;
    using wowee::game::titleIsKnown;

    // The first group's low dword carries ids 1..31, and its high dword the
    // ids 32..63 after them - which is what the client's second read, at +0x77c
    // rather than +0x778, is for.
    CHECK(titleIsKnown({0x00000002u, 0u, 0u, 0u, 0u, 0u}, 1));
    CHECK(titleIsKnown({0x80000000u, 0u, 0u, 0u, 0u, 0u}, 31));
    CHECK(titleIsKnown({0u, 0x00000001u, 0u, 0u, 0u, 0u}, 32));
    CHECK(titleIsKnown({0u, 0x80000000u, 0u, 0u, 0u, 0u}, 63));
    // The second group begins at id 64, in the third dword, and the third ends
    // at id 191, in the last bit of the sixth: the run is six dwords and no
    // further.
    CHECK(titleIsKnown({0u, 0u, 0x00000001u, 0u, 0u, 0u}, 64));
    CHECK(titleIsKnown({0u, 0u, 0u, 0u, 0u, 0x80000000u}, 191));

    // A word of another group answers nothing, and so does the neighbour of the
    // bit that is set.
    CHECK_FALSE(titleIsKnown({0x00000002u, 0u, 0u, 0u, 0u, 0u}, 33));
    CHECK_FALSE(titleIsKnown({0u, 0u, 0x00000001u, 0u, 0u, 0u}, 63));
    CHECK_FALSE(titleIsKnown({0x00000004u, 0u, 0u, 0u, 0u, 0u}, 1));

    // Id 0 is the client's own rejection (its guard reads `id == 0` and answers
    // not-known), and an id past the run is answered not-known rather than read.
    // The client does stride on past its own six dwords, into the known-currency
    // pair that follows them: an id of 192 there is a currency's bit, and 193 the
    // next currency's. Those ids are outside what GetNumTitles names.
    CHECK_FALSE(titleIsKnown({~0u, ~0u, ~0u, ~0u, ~0u, ~0u}, 0));
    CHECK_FALSE(titleIsKnown({~0u, ~0u, ~0u, ~0u, ~0u, ~0u}, 192));
    CHECK_FALSE(titleIsKnown({~0u, ~0u, ~0u, ~0u, ~0u, ~0u}, 193));
}

// The level the realm says the character stops at. Every one of the client's four
// readers asks it the same way - against UNIT_FIELD_LEVEL - and the answer decides
// whether a quest pays experience at all, so an index that lands in a neighbouring
// field reads another character's cap and the panel keeps or drops the line at the
// wrong moment. The index came from the client's own blob position: +0x11ac of the
// player's field array, blob dword 1131, and this layout puts a blob dword 148
// further along on the wire - the same arithmetic as the honour counter (blob
// 1129 → 1277) and the arena counter (1130 → 1278) on either side of it.
TEST_CASE("The level the realm sets as this character's cap",
          "[updatefields][questxp]") {
    UpdateFieldTable table;
    REQUIRE(table.loadFromJson(fieldsPath("wotlk")));

    const uint16_t maxLevel = table.index(UF::PLAYER_FIELD_MAX_LEVEL);
    REQUIRE(maxLevel != 0xFFFF);
    REQUIRE(maxLevel == 1279);
    CHECK(table.index(UF::PLAYER_FIELD_ARENA_CURRENCY) + 1 == maxLevel);
    CHECK(table.index(UF::PLAYER_FIELD_HONOR_CURRENCY) + 2 == maxLevel);

    // No earlier layout names it. The absence has to stay an absence: a caller
    // that reads 0xFFFF has to answer "no cap to apply", and a nearby index would
    // instead hand it some other field's value as a cap - which for a quest's
    // reward XP means no experience line for a character that has plenty to earn.
    for (const auto* expansion : {"classic", "tbc", "turtle"}) {
        INFO("expansion: " << expansion);
        UpdateFieldTable other;
        REQUIRE(other.loadFromJson(fieldsPath(expansion)));
        REQUIRE(other.index(UF::PLAYER_FIELD_MAX_LEVEL) == 0xFFFF);
    }
}

// What QuestXP.dbc's number becomes on the way to the panel. The table's value is
// only a starting point: the client damps it by how far the quest sits from the
// character, rounds it by size, and answers nothing at all once the character is
// at its cap. Skipping any of the three leaves a number that is plausibly the
// table's and visibly wrong next to the quest's own level.
TEST_CASE("A quest pays experience damped, rounded, and not at the cap",
          "[updatefields][questxp]") {
    using namespace wowee::game;

    // A quest with no level of its own (-1) is read at the character's level,
    // which is also what the damping then measures - zero difference, full pay.
    CHECK(questXpRowLevel(-1, 80) == 80);
    CHECK(questXpRowLevel(75, 80) == 75);
    CHECK(questXpRowLevel(0, 80) == 0);   // a quest with no level has no row

    // Full pay for a quest at or above the character, and for one up to five
    // levels below; then a fifth of it per level past that, to a tenth at the
    // floor rather than to nothing.
    CHECK(questXpDampFactor(80, 80) == 10);
    CHECK(questXpDampFactor(85, 80) == 10);
    CHECK(questXpDampFactor(75, 80) == 10);
    CHECK(questXpDampFactor(74, 80) == 8);
    CHECK(questXpDampFactor(70, 80) == 1);
    CHECK(questXpDampFactor(40, 80) == 1);

    // The client's own rounding buckets, at their edges. The three boundaries are
    // 0x65, 0x1f5 and 0x3e9 - 101, 501 and 1001 - so a hundred, five hundred and a
    // thousand each still fall in the *smaller* bucket; they also each land on a
    // number both buckets would give, which is why the constants are quoted in the
    // header rather than depended on here.
    CHECK(questXpRound(0) == 0);
    CHECK(questXpRound(1) == 0);   // the buckets truncate: 3 becomes 5, 1 does not
    CHECK(questXpRound(3) == 5);
    CHECK(questXpRound(99) == 100);
    CHECK(questXpRound(100) == 100);
    CHECK(questXpRound(105) == 110);
    CHECK(questXpRound(495) == 500);
    CHECK(questXpRound(500) == 500);
    CHECK(questXpRound(505) == 500);
    CHECK(questXpRound(999) == 1000);
    CHECK(questXpRound(1000) == 1000);
    CHECK(questXpRound(1026) == 1050);

    // Damping first, rounding second. 232 at eight tenths is 185.6, which the
    // client's integer divide takes as 185 and whose rounding bucket lifts to
    // 190 - whereas rounding the table's number first (232 → 230) and damping
    // afterwards would answer 184. The pair keeps that order stated. A level-75
    // character on a quest of 69 is six above it, which is where the eighths come
    // from; below the cap, so the gate below does not swallow the number.
    CHECK(questRewardXp(245, 69, 75, 80) == 200);
    CHECK(questRewardXp(232, 69, 75, 80) == 190);
    CHECK(questRewardXp(1000, 69, 75, 80) == 800);

    // At the cap the answer is nothing whatever the table says - and a layout that
    // names no cap field leaves the gate closed rather than open at level zero.
    CHECK(questRewardXp(1000, 80, 80, 80) == 0);
    CHECK(questRewardXp(1000, 80, 79, 80) == 1000);
    CHECK(questRewardXp(1000, 80, 80, 0) == 1000);

    // A scaling quest answers at the character's own level, so it never damps.
    CHECK(questRewardXp(1234, questXpRowLevel(-1, 75), 75, 80) == 1250);
}

TEST_CASE("A quest at the cap pays at least its max-level amount",
          "[updatefields][questxp]") {
    using namespace wowee::game;

    // Below the cap the quest's own amount is the whole answer, and the negative
    // half of that field is a cost - GetQuestLogRequiredMoney's, not this one.
    CHECK(questRewardMoney(47400, 19800, 79, 80) == 47400);
    CHECK(questRewardMoney(47400, 19800, 75, 80) == 47400);
    CHECK(questRewardMoney(-5000, 0, 80, 80) == 0);

    // At the cap the second amount takes over only when it is at least as large,
    // which is the direction of the client's compare (0x005e48f0). That is what
    // makes it a floor rather than a replacement: the well-paying quest keeps its
    // 47400, and the quest that pays nothing of its own is raised to the max-level
    // figure. Equal amounts substitute each other, which changes nothing.
    CHECK(questRewardMoney(47400, 19800, 80, 80) == 47400);
    CHECK(questRewardMoney(0, 19800, 80, 80) == 19800);
    CHECK(questRewardMoney(5000, 20000, 80, 80) == 20000);
    CHECK(questRewardMoney(5000, 5000, 80, 80) == 5000);
    CHECK(questRewardMoney(0, 0, 80, 80) == 0);
    // A negative reading of the second word could never raise a payout, so the
    // parser's non-negative gate costs nothing the client would have used.
    CHECK(questRewardMoney(0, -5000, 80, 80) == 0);

    // One level off the cap is not at it, and a layout that names no cap - or that
    // never read the second word, as classic and TBC do not - leaves the floor
    // unapplied rather than paying a quest that pays nothing at all.
    CHECK(questRewardMoney(0, 19800, 79, 80) == 0);
    CHECK(questRewardMoney(0, 19800, 80, 0) == 0);
}

TEST_CASE("The quest giver's dialog copies its numbers and shuts the XP at the cap",
          "[updatefields][questxp]") {
    using namespace wowee::game;

    // The cap test the client spells identically in both of its places: the level
    // is not below the field. A layout that names no cap is never at one.
    CHECK_FALSE(atLevelCap(79, 80));
    CHECK(atLevelCap(80, 80));
    CHECK(atLevelCap(81, 80));
    CHECK_FALSE(atLevelCap(0, 0));

    // The offer packet's money word is one field for a payout or a cost, and the
    // sign says which; the charge belongs to GetQuestMoneyToGet, not here. Read
    // unsigned, a charging quest answered four billion copper.
    CHECK(questDialogMoney(47400) == 47400);
    CHECK(questDialogMoney(0) == 0);
    CHECK(questDialogMoney(-5000) == 0);

    // The experience is the packet's own number - no row lookup, no damping, no
    // rounding - and the cap alone takes it away.
    CHECK(questDialogXp(12345, 79, 80) == 12345);
    CHECK(questDialogXp(12345, 80, 80) == 0);
    CHECK(questDialogXp(12345, 80, 0) == 12345);
}

TEST_CASE("A quest's honor is the addition plus multiplier times the level's row, a tenth",
          "[updatefields][questxp]") {
    using namespace wowee::game;

    // TeamContributionPoints.dbc's row for level 80 is 1241.143; level 1's is
    // 28.0. A multiplier of 1 at 80 adds 124.11, rounded to 124.
    CHECK(questRewardHonor(250, 1.0f, 1241.143f) == 374);
    CHECK(questRewardHonor(250, 2.0f, 1241.143f) == 498);  // 248.23
    CHECK(questRewardHonor(0, 1.0f, 28.0f) == 3);          // 2.8
    // No row (not a player, or a level past the table): the addition alone.
    CHECK(questRewardHonor(250, 20.0f, 0.0f) == 250);
    // The product is rounded as the float the client stores it as, ties to
    // even: 25.0 * 1.0 * 0.1f is 2.5000000372 wide, 2.5 as a float - 2.
    CHECK(questRewardHonor(0, 1.0f, 25.0f) == 2);
    CHECK(questRewardHonor(0, 1.0f, 35.0f) == 4);        // 3.5 -> 4
    // Asked only when one of the two is worth reading; a negative addition
    // with a multiplier stands as sent - the client adds to it.
    CHECK(questRewardHonor(0, 0.0f, 1241.143f) == 0);
    CHECK(questRewardHonor(-5, 0.0f, 1241.143f) == 0);
    CHECK(questRewardHonor(-5, 1.0f, 1241.143f) == 119);
}
