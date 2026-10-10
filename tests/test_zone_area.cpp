// Where the player is, as the client names it (game/zone_area.hpp):
// 0x0077fa00, 0x00782560, 0x0078f020, 0x0078ec70 and 0x005204c0.
#include <catch_amalgamated.hpp>

#include "game/zone_area.hpp"

#include <map>

namespace za = wowee::game::zone_area;

namespace {
// A few of AreaTable's rows: Crystalsong Forest, Dalaran and two of its
// areas, Elwynn Forest and Goldshire.
const std::map<uint32_t, za::AreaInfo> kAreas = {
    {2817, {0, "Crystalsong Forest"}},
    {4395, {0, "Dalaran"}},
    {4601, {4395, "Dalaran Island"}},
    {4567, {4395, "The Violet Hold"}},
    {12, {0, "Elwynn Forest"}},
    {87, {12, "Goldshire"}},
    {9999, {8888, "Orphan"}},
};
const za::AreaInfo* lookup(uint32_t id) {
    const auto it = kAreas.find(id);
    return it == kAreas.end() ? nullptr : &it->second;
}
}  // namespace

TEST_CASE("the place is named by the building met no further down than the ground", "[zone_area]") {
    // Dalaran's street, 600 yards over the forest: the building's area.
    CHECK(za::placeArea(0.3f, 4601, 600.0f, 2817) == 4601u);
    // Nothing of the ground met on the way down: the building's still.
    CHECK(za::placeArea(0.3f, 4601, std::nullopt, 2817) == 4601u);
    // A tie goes to the building.
    CHECK(za::placeArea(2.0f, 4601, 2.0f, 2817) == 4601u);
    // The ground nearer: the ground's chunk.
    CHECK(za::placeArea(5.0f, 4601, 1.0f, 2817) == 2817u);
    // A group whose row names no area: the ground's chunk.
    CHECK(za::placeArea(0.3f, 0, 600.0f, 2817) == 2817u);
    // No building: the ground's.
    CHECK(za::placeArea(std::nullopt, 0, 1.0f, 87) == 87u);
}

TEST_CASE("the sound's area is the linked group's, the ground's otherwise", "[zone_area]") {
    CHECK(za::linkedArea(true, 4601, 2817) == 4601u);
    CHECK(za::linkedArea(true, 0, 2817) == 2817u);
    CHECK(za::linkedArea(false, 4601, 2817) == 2817u);
}

TEST_CASE("an area splits into its parent zone and itself, one step up", "[zone_area]") {
    auto s = za::split(4601, lookup);
    REQUIRE(s);
    CHECK(s->zone == 4395u);
    CHECK(s->subzone == 4601u);
    s = za::split(4395, lookup);
    REQUIRE(s);
    CHECK(s->zone == 4395u);
    CHECK(s->subzone == 0u);
    CHECK_FALSE(za::split(1234, lookup));
    // A parent AreaTable has no row for.
    CHECK_FALSE(za::split(9999, lookup));
}

TEST_CASE("the texts: zone, subzone, real zone and the minimap's", "[zone_area]") {
    // On Dalaran's street, no building names.
    auto t = za::texts(4601, lookup, std::nullopt);
    REQUIRE(t);
    CHECK(t->zoneId == 4395u);
    CHECK(t->subzoneId == 4601u);
    CHECK(t->zone == "Dalaran");
    CHECK(t->realZone == "Dalaran");
    CHECK(t->subzone == "Dalaran Island");
    CHECK(t->minimap == "Dalaran Island");

    // A zone with no subzone: the minimap shows the zone.
    t = za::texts(2817, lookup, std::nullopt);
    REQUIRE(t);
    CHECK(t->subzone.empty());
    CHECK(t->minimap == "Crystalsong Forest");

    // In a shop: the group's name over the subzone, the zone kept.
    t = za::texts(4395, lookup, za::BuildingNames{"Like Clockwork", ""});
    REQUIRE(t);
    CHECK(t->zone == "Dalaran");
    CHECK(t->subzone == "Like Clockwork");
    CHECK(t->minimap == "Like Clockwork");

    // In the Lion's Pride Inn: no group names, the building's own.
    t = za::texts(87, lookup, za::BuildingNames{"", "Lion's Pride Inn"});
    REQUIRE(t);
    CHECK(t->zone == "Elwynn Forest");
    CHECK(t->realZone == "Elwynn Forest");
    CHECK(t->subzone == "Lion's Pride Inn");

    // A building with no names leaves AreaTable's.
    t = za::texts(87, lookup, za::BuildingNames{});
    REQUIRE(t);
    CHECK(t->subzone == "Goldshire");

    CHECK_FALSE(za::texts(1234, lookup, std::nullopt));
}

TEST_CASE("the event a change raises", "[zone_area]") {
    const auto forest = *za::texts(2817, lookup, std::nullopt);
    const auto street = *za::texts(4601, lookup, std::nullopt);
    const auto shop = *za::texts(4395, lookup, za::BuildingNames{"Like Clockwork", ""});
    // Flying up into the city: a new zone.
    CHECK(za::event(forest, street, false) == za::Event::ZoneChangedNewArea);
    // Off the street into a shop: the subzone, indoors.
    CHECK(za::event(street, shop, true) == za::Event::ZoneChangedIndoors);
    // And back out.
    CHECK(za::event(shop, street, false) == za::Event::ZoneChanged);
    // Nothing that is read changed.
    CHECK(za::event(street, street, true) == za::Event::None);
    // The minimap's text alone raises nothing.
    auto other = street;
    other.minimap = "x";
    CHECK(za::event(street, other, false) == za::Event::None);
    CHECK(std::string(za::eventName(za::Event::ZoneChangedIndoors)) == "ZONE_CHANGED_INDOORS");
    CHECK(za::eventName(za::Event::None) == nullptr);
}
