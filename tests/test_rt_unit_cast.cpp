// Which units cast traced shadows: not what is drawn see-through.

#include "rendering/rt_unit_cast.hpp"

#include <catch_amalgamated.hpp>

using namespace wowee::rendering::rt_unit_cast;

TEST_CASE("an opaque unit casts, a see-through one does not", "[rt_unit_cast]") {
    REQUIRE(castsTracedShadow(1.0f, 1.0f));
    REQUIRE_FALSE(castsTracedShadow(0.5f, 1.0f));   // a ghost
    REQUIRE_FALSE(castsTracedShadow(1.0f, 0.3f));   // a kit's fade, GM invisibility
    REQUIRE_FALSE(castsTracedShadow(0.98f, 1.0f));  // still fading in
    REQUIRE_FALSE(castsTracedShadow(0.0f, 1.0f));
}
