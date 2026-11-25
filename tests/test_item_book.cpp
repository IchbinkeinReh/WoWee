// What is read in the item text window (game/item_book.hpp).
#include <catch_amalgamated.hpp>

#include "game/item_book.hpp"

using wowee::game::ItemBook;

TEST_CASE("a book's pages are asked for as they are turned to", "[item_book]") {
    ItemBook book;
    CHECK_FALSE(book.isOpen());
    CHECK(book.wanted() == 0);
    CHECK_FALSE(book.arrived(5, "x", 0));
    book.open(10);
    CHECK(book.wanted() == 10);
    CHECK_FALSE(book.turnOn());
    // The server answers for the pages after it, too.
    CHECK(book.arrived(10, "one", 11));
    CHECK_FALSE(book.arrived(11, "two", 12));
    CHECK_FALSE(book.arrived(10, "one", 11));
    CHECK(book.text() == "one");
    CHECK(book.page() == 0);
    CHECK(book.hasNext());
    CHECK(book.turnOn());
    CHECK(book.page() == 1);
    CHECK(book.wanted() == 0);
    CHECK(book.text() == "two");
    CHECK(book.turnOn());
    CHECK(book.wanted() == 12);
    CHECK(book.text().empty());
    CHECK(book.arrived(12, "three", 0));
    CHECK_FALSE(book.hasNext());
    CHECK_FALSE(book.turnOn());
    CHECK(book.turnBack());
    CHECK(book.text() == "two");
    CHECK(book.turnBack());
    CHECK_FALSE(book.turnBack());
    CHECK(book.turnOn());
    CHECK(book.text() == "two");
    book.close();
    CHECK_FALSE(book.isOpen());
    CHECK(book.text().empty());
    // Opened again, its pages are there.
    book.open(10);
    CHECK(book.wanted() == 0);
    CHECK(book.page() == 0);
}
