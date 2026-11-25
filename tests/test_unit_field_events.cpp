// The events of a unit's fields (game/unit_field_events.hpp).
#include <catch_amalgamated.hpp>

#include "game/unit_field_events.hpp"

#include <string>

using namespace wowee::game;

TEST_CASE("a unit's field has the event of its number", "[unit_field_events]") {
    const UnitEventFields at{.bytes0 = 23, .health = 24, .power1 = 25, .maxHealth = 32, .maxPower1 = 33, .level = 54,
                             .faction = 55, .flags = 59, .flags2 = 60, .dynamic = 79};
    const auto event = [&](uint16_t key) {
        const char* name = unitFieldEvent(at, key);
        return std::string(name ? name : "");
    };
    CHECK(event(23) == "UNIT_DISPLAYPOWER");
    CHECK(event(24) == "UNIT_HEALTH");
    CHECK(event(25) == "UNIT_MANA");
    CHECK(event(26) == "UNIT_RAGE");
    CHECK(event(31) == "UNIT_RUNIC_POWER");
    CHECK(event(32) == "UNIT_MAXHEALTH");
    CHECK(event(33) == "UNIT_MAXMANA");
    CHECK(event(54) == "UNIT_LEVEL");
    CHECK(event(55) == "UNIT_FACTION");
    CHECK(event(59) == "UNIT_FLAGS");
    CHECK(event(60) == "UNIT_FLAGS");
    CHECK(event(79) == "UNIT_DYNAMIC_FLAGS");
    CHECK(event(70).empty());
    CHECK(unitFieldEvent(UnitEventFields{}, 0xFFFF) == nullptr);
    CHECK(unitFieldEvent(UnitEventFields{}, 3) == nullptr);
}

TEST_CASE("what a unit's state says of it", "[unit_field_events]") {
    using Events = std::vector<std::string>;
    const auto events = [](const UnitSigns& was, const UnitSigns& is) {
        Events out;
        for (const char* e : unitSignEvents(was, is)) out.emplace_back(e);
        return out;
    };
    CHECK(events({.flags = 0x100}, {}) == Events{"UNIT_FACTION"});
    CHECK(events({}, {.flags = 0x80}) == Events{"UNIT_FACTION"});
    CHECK(events({}, {.flags = 0x80000}).empty());
    CHECK(events({}, {.pvp = 1}) == Events{"UNIT_FACTION"});
    CHECK(events({}, {.dynamic = 4}) == Events{"UNIT_FACTION"});
    CHECK(events({}, {.dynamic = 1}).empty());
    CHECK(events({}, {.dynamic = 0x20, .powerType = 1}) == Events{"UNIT_HEALTH", "UNIT_RAGE"});
}
