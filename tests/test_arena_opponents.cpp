// The arena's opponents as BattlefieldInfo.cpp slots them: a player into the
// first free slot or beside the pet that came first (0x0054d100), a pet beside
// its owner (0x0054d1c0), and ARENA_OPPONENT_UPDATE's words for each, down to
// SMSG_ARENA_UNIT_DESTROYED (0x0054ad10).
#include <catch_amalgamated.hpp>

#include "game/arena_opponents.hpp"

using wowee::game::arena::OpponentUpdate;
using wowee::game::arena::Roster;

TEST_CASE("Opponents take the first free slot and are said seen", "[arena]") {
    Roster r;
    CHECK(r.opponentSeen(0x10, true) == std::vector<OpponentUpdate>{{"arena1", "seen"}});
    CHECK(r.opponentSeen(0x11, false) ==
          std::vector<OpponentUpdate>{{"arena2", "seen"}, {"arena2", "unseen"}});
    CHECK(r.opponentSeen(0x10, true) == std::vector<OpponentUpdate>{{"arena1", "seen"}});  // its own slot
    CHECK(r.opponent(1) == 0x11);
}

TEST_CASE("A pet goes beside its owner, and the owner beside a pet seen first", "[arena]") {
    Roster r;
    r.opponentSeen(0x10, true);
    CHECK(r.petSeen(0xF140000001000001ull, 0x10, true) == std::vector<OpponentUpdate>{{"arenapet1", "seen"}});
    // A pet whose owner is not yet seen takes a slot of its own, and the owner
    // then joins it there.
    CHECK(r.petSeen(0xF140000002000002ull, 0x12, true) == std::vector<OpponentUpdate>{{"arenapet2", "seen"}});
    CHECK(r.opponentSeen(0x12, true) == std::vector<OpponentUpdate>{{"arena2", "seen"}});
}

TEST_CASE("Destroyed: an opponent, an opponent's pet in sight, a pet's slot", "[arena]") {
    Roster r;
    r.opponentSeen(0x10, true);
    r.petSeen(0xF140000001000001ull, 0x10, true);
    const auto none = [](uint64_t) { return uint64_t{0}; };
    CHECK(r.destroyed(0x10, none) == std::vector<OpponentUpdate>{{"arena1", "destroyed"}});
    CHECK(r.opponent(0) == 0x10);  // said, not cleared
    const auto summoned = [](uint64_t o) { return o == 0x10 ? 0xF140000009000009ull : 0; };
    CHECK(r.destroyed(0xF140000009000009ull, summoned) == std::vector<OpponentUpdate>{{"arenapet1", "destroyed"}});
    CHECK(r.destroyed(0xF140000001000001ull, none) == std::vector<OpponentUpdate>{{"arenapet1", "destroyed"}});
    CHECK(r.pet(0) == 0);
    CHECK(r.destroyed(0x99, none).empty());
}
