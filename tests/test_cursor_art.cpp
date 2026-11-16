// The cursor table looked up by art name, as 0x00616280 does for an object's
// IconName and a lock's LockType CursorName.
#include <catch_amalgamated.hpp>

#include "ui/cursor_art.hpp"

using namespace wowee::ui;

TEST_CASE("an art name finds its kind without regard to case", "[cursorart]") {
    CHECK(cursorKindForArt("Interact") == 5);
    CHECK(cursorKindForArt("pickLOCK") == 14);
    CHECK(cursorKindForArt("GatherHerbs") == 13);
    CHECK(cursorPathForKind(cursorKindForArt("Mine")) == "Interface\\Cursor\\Mine.blp");
}

TEST_CASE("a name the table lacks finds none", "[cursorart]") {
    CHECK(cursorKindForArt("NoSuchCursor") == kCursorKindNone);
    CHECK(cursorKindForArt("") == kCursorKindNone);
    CHECK(cursorKindForArt("Interac") == kCursorKindNone);
}

TEST_CASE("the can't twins sit 26 along", "[cursorart]") {
    CHECK(cursorKindForArt("UnableInteract") == 5 + 26);
}
