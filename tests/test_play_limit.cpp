// How many sounds of a kind the client lets play at once
// (audio/play_limit.hpp): 0x009f2268's counts, refused past them.
#include <catch_amalgamated.hpp>

#include "audio/play_limit.hpp"

using wowee::audio::PlayLimit;
namespace pl = wowee::audio::play_limit;

TEST_CASE("the client's counted play types allow its own numbers", "[audio]") {
    CHECK(pl::maxPlaying(PlayLimit::ErrorSpeech) == 1);
    CHECK(pl::maxPlaying(PlayLimit::RaceSplash) == 2);
    CHECK(pl::maxPlaying(PlayLimit::UnitFootstep) == 6);
    CHECK(pl::maxPlaying(PlayLimit::OwnFootstep) == 4);
    CHECK(pl::maxPlaying(PlayLimit::WeaponSwing) == 2);
    CHECK(pl::maxPlaying(PlayLimit::WeaponImpact) == 4);
}

TEST_CASE("a counted type is refused once its count is playing", "[audio]") {
    // A crowd's footfalls: the sixth starts, the seventh does not.
    CHECK(pl::admits(PlayLimit::UnitFootstep, 5));
    CHECK_FALSE(pl::admits(PlayLimit::UnitFootstep, 6));
    CHECK_FALSE(pl::admits(PlayLimit::UnitFootstep, 7));
    // The player's own: four at once.
    CHECK(pl::admits(PlayLimit::OwnFootstep, 3));
    CHECK_FALSE(pl::admits(PlayLimit::OwnFootstep, 4));
    // One error line at a time.
    CHECK(pl::admits(PlayLimit::ErrorSpeech, 0));
    CHECK_FALSE(pl::admits(PlayLimit::ErrorSpeech, 1));
    // A melee brawl: two whooshes and four impacts heard at once.
    CHECK(pl::admits(PlayLimit::WeaponSwing, 1));
    CHECK_FALSE(pl::admits(PlayLimit::WeaponSwing, 2));
    CHECK(pl::admits(PlayLimit::WeaponImpact, 3));
    CHECK_FALSE(pl::admits(PlayLimit::WeaponImpact, 4));
}

TEST_CASE("an uncounted play is never refused for those playing", "[audio]") {
    CHECK(pl::admits(PlayLimit::None, 0));
    CHECK(pl::admits(PlayLimit::None, 100000));
}
