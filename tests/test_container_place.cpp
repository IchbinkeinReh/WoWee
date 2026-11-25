// Where a container's slot is (game/container_place.hpp).
#include <catch_amalgamated.hpp>

#include "game/container_place.hpp"

using wowee::game::ContainerPlace;
using wowee::game::containerPlace;

TEST_CASE("a container's slot", "[container_place]") {
    const auto place = containerPlace(0, 9);
    CHECK(place.where == ContainerPlace::Where::Backpack);
    CHECK(place.slot == 8);
    const auto first = containerPlace(1, 9);
    CHECK(first.where == ContainerPlace::Where::Bag);
    CHECK(first.bag == 0);
    CHECK(first.slot == 8);
    CHECK(containerPlace(4, 1).bag == 3);
    CHECK(containerPlace(5, 1).where == ContainerPlace::Where::Elsewhere);
    CHECK(containerPlace(-1, 1).where == ContainerPlace::Where::Elsewhere);
    CHECK(containerPlace(1, 0).where == ContainerPlace::Where::Nowhere);
}
