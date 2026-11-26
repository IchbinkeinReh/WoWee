#include <catch_amalgamated.hpp>

#include "game/dispel_check.hpp"
#include "game/spell_book_records.hpp"

#include <vector>

using namespace wowee::game::dispel_check;

// 0x00803500 / 0x00728500: the client refuses a dispel or spellsteal aimed
// at units with nothing it could remove.

namespace {
Aura aura(uint8_t flags, uint32_t dispel, uint32_t effect0 = 6) {
    AuraSpell s;
    s.dispel = dispel;
    s.effect = {effect0, 0, 0};
    return Aura{flags, s};
}
constexpr uint32_t kMagic = 1;
constexpr uint32_t kCurse = 2;
}  // namespace

TEST_CASE("a dispel type matches the aura's own, or type 7's mask", "[dispel_check]") {
    const std::vector<Aura> curse{aura(0x01, kCurse)};
    REQUIRE(unitHasRemovable(curse, kCurse, Side::Friendly, false, false, false, 0));
    REQUIRE_FALSE(unitHasRemovable(curse, kMagic, Side::Friendly, false, false, false, 0));
    REQUIRE(unitHasRemovable(curse, 7, Side::Friendly, false, false, false, 30));
    REQUIRE(dispelMask(3, 0) == 8u);
}

TEST_CASE("on an enemy only its helpful auras count", "[dispel_check]") {
    const std::vector<Aura> harmful{aura(0x81, kMagic)};
    REQUIRE_FALSE(unitHasRemovable(harmful, kMagic, Side::Hostile, false, false, false, 0));
    REQUIRE(unitHasRemovable(harmful, kMagic, Side::Friendly, false, false, false, 0));
    REQUIRE(unitHasRemovable(harmful, kMagic, Side::Any, false, false, false, 0));
}

TEST_CASE("passive, area and unknown auras are never offered", "[dispel_check]") {
    Aura passive = aura(0x01, kMagic);
    passive.spell->attributes = 0x40;
    REQUIRE_FALSE(unitHasRemovable(std::vector<Aura>{passive}, kMagic, Side::Friendly, false, false, false, 0));
    // Effect 0 applies a raid area aura (65).
    REQUIRE_FALSE(unitHasRemovable(std::vector<Aura>{aura(0x01, kMagic, 0x41)}, kMagic, Side::Friendly, false,
                                   false, false, 0));
    // The same effect not applied on this unit (flag bit clear) does not hide it.
    REQUIRE(unitHasRemovable(std::vector<Aura>{aura(0x02, kMagic, 0x41)}, kMagic, Side::Friendly, false, false,
                             false, 0));
    REQUIRE_FALSE(unitHasRemovable(std::vector<Aura>{Aura{0x01, std::nullopt}}, kMagic, Side::Friendly, false, false,
                                   false, 0));
}

TEST_CASE("spellsteal skips channelled and unstealable auras", "[dispel_check]") {
    Aura channelled = aura(0x01, kMagic);
    channelled.spell->attributesEx = 0x4;
    Aura unstealable = aura(0x01, kMagic);
    unstealable.spell->attributesEx4 = 0x40;
    REQUIRE_FALSE(unitHasRemovable(std::vector<Aura>{channelled, unstealable}, kMagic, Side::Hostile, false, true,
                                   false, 0));
    REQUIRE(unitHasRemovable(std::vector<Aura>{aura(0x01, kMagic)}, kMagic, Side::Hostile, false, true, false, 0));
}

TEST_CASE("a mechanic dispel matches the spell's or an effect's mechanic", "[dispel_check]") {
    Aura root = aura(0x01, 0);
    root.spell->effectMechanic = {7, 0, 0};
    REQUIRE(unitHasRemovable(std::vector<Aura>{root}, 7, Side::Friendly, true, false, false, 0));
    REQUIRE_FALSE(unitHasRemovable(std::vector<Aura>{root}, 12, Side::Friendly, true, false, false, 0));
}

TEST_CASE("a charmed unit offers only the auras holding it, and only as an enemy", "[dispel_check]") {
    const std::vector<Aura> buff{aura(0x01, kMagic, 35 + 100)};  // not 6, 177 or 2
    REQUIRE_FALSE(unitHasRemovable(buff, kMagic, Side::Friendly, false, false, true, 0));
    REQUIRE_FALSE(unitHasRemovable(buff, kMagic, Side::Hostile, false, false, true, 0));
    const std::vector<Aura> applied{aura(0x81, kMagic, 6)};
    REQUIRE(unitHasRemovable(applied, kMagic, Side::Hostile, false, false, true, 0));
}

TEST_CASE("the refusal is nothing to dispel or nothing to steal", "[dispel_check]") {
    const std::array<uint32_t, 3> dispel{kEffectDispel, 0, 0};
    const std::array<uint32_t, 3> steal{kEffectStealBuff, 0, 0};
    const auto none = [](size_t, size_t) -> std::optional<bool> { return false; };
    const auto some = [](size_t, size_t) -> std::optional<bool> { return true; };
    const auto skipped = [](size_t, size_t) -> std::optional<bool> { return std::nullopt; };
    REQUIRE(refusal(dispel, 0, 1, none) == kNothingToDispel);
    REQUIRE(refusal(steal, 0, 1, none) == kNothingToSteal);
    REQUIRE(refusal(dispel, 0, 1, some) == 0);
    REQUIRE(refusal(dispel, 0, 1, skipped) == kNothingToDispel);
    // Not a removal at all.
    REQUIRE(refusal({6, 0, 0}, 0, 1, none) == 0);
    // Another effect with work to do lets it go...
    const std::array<uint32_t, 3> mixed{kEffectDispel, 2, 0};
    REQUIRE(refusal(mixed, 0, 1, none) == 0);
    // ...unless AttributesEx4 0x20000000 << effect makes the removal required.
    REQUIRE(refusal(mixed, 0x20000000u, 1, none) == kNothingToDispel);
}

// 0x0053d680: UnitAura's isStealable against the book's steal mask, which
// 0x00542030 sets from a learned spellsteal (0x00be5d68).
TEST_CASE("an aura is stealable by the book's steal mask", "[dispel_check]") {
    using wowee::game::spell_book::Records;
    using wowee::game::spell_book::Row;
    Records book;
    Row other;
    other.effect0 = 126;
    other.targetA0 = 1;  // not at an enemy
    other.misc0 = 1;
    book.learned(1, other);
    REQUIRE(book.stealMask == 0u);
    Row steal;
    steal.effect0 = 126;
    steal.targetA0 = 6;
    steal.misc0 = 1;
    book.learned(30449, steal);
    REQUIRE(book.stealMask == 2u);
    // A row's mask where SpellDispelType has one.
    Row all = steal;
    all.misc0 = 7;
    all.dispelMask0 = spellDispelTypeMask(7);
    Records allBook;
    allBook.learned(2, all);
    REQUIRE(allBook.stealMask == 30u);
    // Forgetting leaves it; the book's end clears it.
    book.removed(30449);
    REQUIRE(book.stealMask == 2u);
    book.clear();
    REQUIRE(book.stealMask == 0u);

    AuraSpell s;
    s.dispel = kMagic;
    s.effect = {6, 0, 0};
    REQUIRE(auraStealable(2u, true, 0x01, s));
    REQUIRE_FALSE(auraStealable(0u, true, 0x01, s));
    REQUIRE_FALSE(auraStealable(2u, false, 0x01, s));
    REQUIRE_FALSE(auraStealable(2u, true, 0x81, s));  // harmful
    REQUIRE_FALSE(auraStealable(4u, true, 0x01, s));  // curses only
    AuraSpell area = s;
    area.effect = {0x23, 0, 0};  // party area aura
    REQUIRE_FALSE(auraStealable(2u, true, 0x01, area));
    REQUIRE(auraStealable(2u, true, 0x02, area));  // its area effect not applied
    AuraSpell passive = s;
    passive.attributes = 0x40;
    REQUIRE_FALSE(auraStealable(2u, true, 0x01, passive));
    AuraSpell channel = s;
    channel.attributesEx = 0x40;
    REQUIRE_FALSE(auraStealable(2u, true, 0x01, channel));
    AuraSpell unstealable = s;
    unstealable.attributesEx4 = 0x40;
    REQUIRE_FALSE(auraStealable(2u, true, 0x01, unstealable));
}
