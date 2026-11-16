// A letter from an ignored player is deleted or sent back unread (0x00571c50).
#include <catch_amalgamated.hpp>

#include "game/mail_ignore.hpp"

using wowee::game::IgnoredMailAction;
using wowee::game::ignoredMailAction;

TEST_CASE("an ignored player's letter is deleted or returned", "[mail]") {
    // Nothing on it: deleted.
    CHECK(ignoredMailAction(0, 0, true, 0, 0) == IgnoredMailAction::Delete);
    // An item or money on it: returned.
    CHECK(ignoredMailAction(0, 0, true, 1, 0) == IgnoredMailAction::Return);
    CHECK(ignoredMailAction(0, 0, true, 0, 50) == IgnoredMailAction::Return);
    // Kept: not ignored, not a player's letter, or flagged 0x2/0x8/0x200.
    CHECK(ignoredMailAction(0, 0, false, 0, 0) == IgnoredMailAction::Keep);
    CHECK(ignoredMailAction(2, 0, true, 0, 0) == IgnoredMailAction::Keep);
    CHECK(ignoredMailAction(0, 0x2, true, 0, 0) == IgnoredMailAction::Keep);
    CHECK(ignoredMailAction(0, 0x8, true, 1, 0) == IgnoredMailAction::Keep);
    CHECK(ignoredMailAction(0, 0x200, true, 0, 0) == IgnoredMailAction::Keep);
    CHECK(ignoredMailAction(0, 0x1, true, 0, 0) == IgnoredMailAction::Delete);
}
