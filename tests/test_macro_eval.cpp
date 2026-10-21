// What a macro shows, read out of its text as the client reads it
// (UIMacros.cpp 0x00565c40, 0x005650c0).

#include <catch_amalgamated.hpp>

#include "game/macro_eval.hpp"

#include <map>

using namespace wowee::game::macro_eval;

namespace {

int slashKind(const std::string& word) {
    if (word == "/cast" || word == "/use") return kCast;
    if (word == "/castrandom") return kRandom;
    if (word == "/castsequence") return kSequence;
    if (word == "/equip") return kEquip;
    if (word == "/equipslot") return kEquipSlot;
    return 0;
}

Hooks hooks() {
    Hooks h;
    static const std::map<std::string, uint32_t> spells = {{"Fireball", 133}, {"Frostbolt", 116}};
    static const std::map<std::string, uint32_t> items = {{"Hearthstone", 6948}};
    h.spellByName = [](const std::string& name, bool& pet) -> uint32_t {
        pet = false;
        const auto it = spells.find(name);
        return it == spells.end() ? 0 : it->second;
    };
    h.itemByName = [](const std::string& name) -> uint32_t {
        const auto it = items.find(name);
        return it == items.end() ? 0 : it->second;
    };
    h.itemInSlot = [](int bag, int slot) -> uint32_t {
        if (bag == -1 && slot == 12) return 1111;    // a trinket
        if (bag == -1 && slot == 23) return 2222;    // the backpack's first
        if (bag == 1 && slot == 3) return 3333;      // the second bag's fourth
        return 0;
    };
    h.options = [](const std::string& text, std::string& result, uint64_t& target) {
        // "[combat] A; B" with combat false: B.
        const size_t semi = text.find(';');
        if (semi == std::string::npos) return false;
        result = text.substr(semi + 1);
        target = 0;
        return true;
    };
    h.castSequence = [](const std::string&) { return std::string("Frostbolt"); };
    return h;
}

}  // namespace

TEST_CASE("A plain /cast is read and ends the reading", "[macro]") {
    const Parsed p = parse("/cast Fireball\n/cast Frostbolt", slashKind);
    REQUIRE(p.commands.size() == 1);
    CHECK(p.commands[0].kind == kCast);
    CHECK(p.commands[0].text == "Fireball");
    CHECK_FALSE(p.dynamic);
    const Result r = evaluate(p, hooks());
    CHECK(r.spell == 133);
    CHECK(r.item == 0);
}

TEST_CASE("#showtooltip with a name decides; without one it only sets the flag", "[macro]") {
    Parsed p = parse("#showtooltip Frostbolt\n/cast Fireball", slashKind);
    CHECK(p.showTooltip);
    REQUIRE(p.commands.size() == 1);
    CHECK(p.commands[0].kind == kShow);
    CHECK(evaluate(p, hooks()).spell == 116);

    p = parse("#showtooltip\n/cast Fireball", slashKind);
    CHECK(p.showTooltip);
    REQUIRE(p.commands.size() == 1);
    CHECK(evaluate(p, hooks()).spell == 133);
}

TEST_CASE("A conditional makes the macro dynamic and reading goes on", "[macro]") {
    const Parsed p = parse("/cast [combat] Fireball; Frostbolt\n/cast Fireball\n#show Frostbolt\n/cast X", slashKind);
    CHECK(p.dynamic);
    // Once dynamic, every command up to the #show is kept.
    REQUIRE(p.commands.size() == 3);
    CHECK(p.commands[2].kind == kShow);
    const Result r = evaluate(p, hooks());
    CHECK(r.spell == 116);
}

TEST_CASE("Names that resolve to nothing answer -1", "[macro]") {
    const Parsed p = parse("/cast Pyroblast", slashKind);
    CHECK(evaluate(p, hooks()).spell == -1);
    // An item is looked for first, and a /equip never names a spell.
    CHECK(evaluate(parse("/use Hearthstone", slashKind), hooks()).item == 6948);
    const Result equip = evaluate(parse("/equip Fireball", slashKind), hooks());
    CHECK(equip.spell == -1);
    CHECK(equip.item == 0);
}

TEST_CASE("Slot numbers name what is in the slot", "[macro]") {
    Parsed p = parse("/use 13", slashKind);
    CHECK(p.dynamic);
    CHECK(evaluate(p, hooks()).item == 1111);
    CHECK(evaluate(parse("/use 0 1", slashKind), hooks()).item == 2222);
    CHECK(evaluate(parse("/use 2 4", slashKind), hooks()).item == 3333);
    const Result empty = evaluate(parse("/use 5", slashKind), hooks());
    CHECK(empty.item == 0);
    CHECK(empty.spell == -1);
}

TEST_CASE("/castrandom shows its first entry, /castsequence its current one", "[macro]") {
    CHECK(evaluate(parse("/castrandom Fireball, Frostbolt", slashKind), hooks()).spell == 133);
    const Parsed seq = parse("/castsequence reset=3 Fireball, Frostbolt", slashKind);
    CHECK(seq.dynamic);
    CHECK(evaluate(seq, hooks()).spell == 116);
}

TEST_CASE("Unknown commands, comments and \"none\"", "[macro]") {
    CHECK(parse("/say hi\n# a comment\n", slashKind).commands.empty());
    CHECK(parse("/cast none\n/cast Fireball", slashKind).commands.empty());
    CHECK(evaluate(parse("/say hi", slashKind), hooks()).spell == 0);
}
