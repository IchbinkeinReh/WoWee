// Which entity a name means (mcp/entity_pick.hpp).
#include <catch_amalgamated.hpp>

#include "mcp/entity_pick.hpp"

#include <cmath>

using wowee::mcp::PickCandidate;
using wowee::mcp::pickRank;

TEST_CASE("the one at hand before a namesake far off", "[mcp][pick]") {
    const PickCandidate corpse{.wholeName = true, .unit = true, .dead = true, .lootable = true, .distance = 2.0f};
    const PickCandidate living{.wholeName = true, .unit = true, .dead = false, .lootable = false, .distance = 90.0f};
    CHECK(pickRank(corpse, "", false) < pickRank(living, "", false));   // interact
    CHECK(pickRank(living, "", true) < pickRank(corpse, "", true));     // target
    CHECK(std::isinf(pickRank(living, "dead", true)));
    CHECK(std::isinf(pickRank(living, "lootable", true)));
    CHECK(std::isinf(pickRank(corpse, "alive", false)));
    CHECK_FALSE(std::isinf(pickRank(corpse, "lootable", false)));
    // A thing is neither, and alive only leaves the dead out.
    const PickCandidate chest{.wholeName = true, .distance = 5.0f};
    CHECK_FALSE(std::isinf(pickRank(chest, "alive", true)));
    CHECK(std::isinf(pickRank(chest, "dead", true)));
    // The whole name before a part of one.
    const PickCandidate part{.wholeName = false, .unit = true, .distance = 1.0f};
    CHECK(pickRank(living, "", true) < pickRank(part, "", true));
}
