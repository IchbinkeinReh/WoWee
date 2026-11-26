// T405: UnitCastingInfo / UnitChannelInfo answer one start and end for a cast,
// in whole milliseconds (0x00611df0: the unit's +0xa78 / +0xa7c), not times
// worked out anew from a remaining time that runs down by frames.
#include <catch_amalgamated.hpp>

#include "game/cast_times.hpp"

using namespace wowee::game;

TEST_CASE("a cast's times stay put through the cast (T405)", "[cast]") {
    // A 1.5 s cast read four times, the remaining time lagging the clock by a
    // frame or so each time - as the tester read 480403.626, 480408.149 ...
    const struct { double now; float remaining; } reads[] = {
        {480904.0, 1.0f}, {481210.0, 0.6995f}, {481520.0, 0.3866f}, {481700.0, 0.2081f}};
    CastTimes kept;
    double firstStart = 0.0, firstEnd = 0.0;
    bool oldMoved = false;
    for (const auto& r : reads) {
        kept = castTimesNow(kept, 3275, false, 1.5f, r.remaining, r.now);
        if (firstStart == 0.0) {
            firstStart = kept.start;
            firstEnd = kept.end;
        }
        CHECK(kept.start == firstStart);
        CHECK(kept.end == firstEnd);
        // The old answer: worked out on each call.
        const double oldStart = r.now - (1.5 - r.remaining) * 1000.0;
        oldMoved = oldMoved || std::fabs(oldStart - (480904.0 - 500.0)) > 0.5;
    }
    CHECK(oldMoved);
    CHECK(firstStart == std::floor(firstStart));
    CHECK(firstEnd - firstStart == 1500.0);
    // A delay of 0.5 s (total and remaining both longer) moves the end only.
    kept = castTimesNow(kept, 3275, false, 2.0f, 0.6f, 481800.0);
    CHECK(kept.start == firstStart);
    CHECK(kept.end == firstStart + 2000.0);
    // The same spell again, later: a cast of its own.
    kept = castTimesNow(kept, 3275, false, 1.5f, 1.5f, 485000.0);
    CHECK(kept.start == 485000.0);
    CHECK(kept.end == 486500.0);
}
