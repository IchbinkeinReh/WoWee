#include <catch_amalgamated.hpp>

#include "game/update_field_table.hpp"
#include "game/known_currencies.hpp"

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
