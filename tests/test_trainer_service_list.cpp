// The trainer list as the client keeps it (ClassTrainerFrame.cpp): services
// grouped under header rows, the shown rows sorted first, and the filters that
// decide what shows (0x00596450, 0x00594ba0).

#include <catch_amalgamated.hpp>

#include "game/trainer_service_list.hpp"

#include <map>

using namespace wowee::game;

namespace {

TrainerSpell service(uint32_t spell, uint8_t state, uint8_t level, uint32_t skillValue = 0) {
    TrainerSpell s;
    s.spellId = spell;
    s.state = state;
    s.reqLevel = level;
    s.reqSkillValue = skillValue;
    return s;
}

struct Fixture {
    std::map<uint32_t, uint32_t> lineOf;
    std::map<uint32_t, std::string> lineNames;
    std::map<uint32_t, std::string> spellNames;
    TrainerServiceList::Lookups lookups() const {
        TrainerServiceList::Lookups lk;
        lk.groupOf = [this](const TrainerSpell& s) {
            const auto it = lineOf.find(s.spellId);
            return it == lineOf.end() ? 0u : it->second;
        };
        lk.lineName = [this](uint32_t line) {
            const auto it = lineNames.find(line);
            return it == lineNames.end() ? std::string() : it->second;
        };
        lk.spellText = [this](uint32_t spell, std::string& name, std::string& rank) {
            const auto it = spellNames.find(spell);
            if (it == spellNames.end()) return false;
            name = it->second;
            rank.clear();
            return true;
        };
        return lk;
    }
};

std::vector<int32_t> shownSpells(const TrainerServiceList& list) {
    std::vector<int32_t> out;
    for (uint32_t i = 0; i < list.visibleCount(); ++i) {
        const auto* r = list.row(i);
        out.push_back(r->header() ? -static_cast<int32_t>(r->line) : r->spellId);
    }
    return out;
}

std::vector<uint8_t> statesOf(const std::vector<TrainerSpell>& v) {
    std::vector<uint8_t> out;
    for (const auto& s : v) out.push_back(s.state);
    return out;
}

}  // namespace

TEST_CASE("A class trainer's services sit under their skill lines, by level then name",
          "[trainer]") {
    Fixture f;
    f.lineOf = {{10, 6}, {11, 6}, {12, 8}, {13, 6}, {14, 0}};
    f.lineNames = {{6, "Frost"}, {8, "Arcane"}};
    f.spellNames = {{10, "Frostbolt"}, {11, "Blizzard"}, {12, "Arcane Missiles"}, {13, "Cone of Cold"}};
    const std::vector<TrainerSpell> spells = {service(10, 0, 20), service(11, 0, 20), service(12, 0, 8),
                                              service(13, 1, 26), service(14, 0, 1)};
    TrainerServiceList list;
    list.build(spells, statesOf(spells), false, 3, f.lookups());
    // Spell 14 has no skill line and is not listed. Arcane before Frost by
    // name; in Frost, level 20 before 26 and Blizzard before Frostbolt.
    CHECK(list.size() == 6);
    CHECK(shownSpells(list) == std::vector<int32_t>{-8, 12, -6, 11, 10, 13});
    // The first available shown service is selected.
    CHECK(list.selectedSpell() == 12);
    CHECK(list.selectedRow() == 1);
}

TEST_CASE("The type filter hides services and empties a header", "[trainer]") {
    Fixture f;
    f.lineOf = {{10, 6}, {11, 8}};
    f.lineNames = {{6, "Frost"}, {8, "Arcane"}};
    f.spellNames = {{10, "Frostbolt"}, {11, "Arcane Blast"}};
    const std::vector<TrainerSpell> spells = {service(10, 0, 4), service(11, 2, 64)};
    TrainerServiceList list;
    list.build(spells, statesOf(spells), false, 3, f.lookups());
    // "used" is off: Arcane's only service is hidden, and its header with it.
    CHECK(shownSpells(list) == std::vector<int32_t>{-6, 10});
    list.refilter(7, f.lookups());
    CHECK(shownSpells(list) == std::vector<int32_t>{-8, 11, -6, 10});
    // Hidden rows stay in the list past the shown ones.
    CHECK(list.size() == 4);
}

TEST_CASE("A collapsed header keeps its row and hides its services", "[trainer]") {
    Fixture f;
    f.lineOf = {{10, 6}, {11, 8}};
    f.lineNames = {{6, "Frost"}, {8, "Arcane"}};
    f.spellNames = {{10, "Frostbolt"}, {11, "Arcane Blast"}};
    const std::vector<TrainerSpell> spells = {service(10, 0, 4), service(11, 0, 64)};
    TrainerServiceList list;
    list.build(spells, statesOf(spells), false, 3, f.lookups());
    REQUIRE(list.headerOfRow(0) == 0);   // Arcane is the first header
    CHECK(list.headerOfRow(1) == -1);    // a service row is no header
    list.setExpandedMask(~1u);
    list.refilter(3, f.lookups());
    CHECK(shownSpells(list) == std::vector<int32_t>{-8, -6, 10});
    CHECK(list.rowCollapsed(0));
    CHECK_FALSE(list.rowCollapsed(1));
    // The skill line filter hides a header and everything under it.
    list.setExpandedMask(~0u);
    list.setLineFilter(~2u);
    list.refilter(3, f.lookups());
    CHECK(shownSpells(list) == std::vector<int32_t>{-8, 11});
}

TEST_CASE("A tradeskill trainer groups by kind and orders by skill", "[trainer]") {
    Fixture f;
    f.lineOf = {{20, 2}, {21, 1}, {22, 2}, {23, 3}};
    f.spellNames = {{20, "Bolt of Linen"}, {21, "Journeyman"}, {22, "Brown Linen Vest"}, {23, "Passive"}};
    const std::vector<TrainerSpell> spells = {service(20, 0, 0, 1), service(21, 0, 10, 50), service(22, 0, 0, 10),
                                              service(23, 1, 0, 0)};
    TrainerServiceList list;
    list.build(spells, statesOf(spells), true, 3, f.lookups());
    CHECK(list.tradeskill());
    CHECK(shownSpells(list) == std::vector<int32_t>{-1, 21, -2, 20, 22, -3, 23});
}

TEST_CASE("Learning a service recounts the list without a new one", "[trainer]") {
    Fixture f;
    f.lineOf = {{10, 6}};
    f.lineNames = {{6, "Frost"}};
    f.spellNames = {{10, "Frostbolt"}};
    const std::vector<TrainerSpell> spells = {service(10, 0, 4)};
    TrainerServiceList list;
    list.build(spells, statesOf(spells), false, 3, f.lookups());
    CHECK(list.visibleCount() == 2);
    CHECK(list.restate([](const TrainerServiceList::Row&) { return uint8_t{2}; }, 3, f.lookups()));
    // With "used" off the header has nothing left to show.
    CHECK(list.visibleCount() == 0);
    CHECK_FALSE(list.restate([](const TrainerServiceList::Row&) { return uint8_t{2}; }, 3, f.lookups()));
}

TEST_CASE("Service type names are the client's, without case", "[trainer]") {
    CHECK(trainerServiceType("available") == 0);
    CHECK(trainerServiceType("Unavailable") == 1);
    CHECK(trainerServiceType("USED") == 2);
    CHECK(trainerServiceType("header") == 6);
    CHECK(trainerServiceType(nullptr) == 6);
}
