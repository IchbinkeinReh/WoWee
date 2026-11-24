// The lines an edit box remembers (0x00964690, 0x00966600, 0x00966670).
#include <catch_amalgamated.hpp>

#include "ui/edit_history.hpp"

using wowee::ui::EditHistory;

namespace {
std::string text(const std::string* line) { return line ? *line : std::string("-"); }
}  // namespace

TEST_CASE("a box that keeps no lines has none", "[editbox]") {
    EditHistory h;
    h.add("a");
    CHECK(h.previous() == nullptr);
    CHECK(h.next() == nullptr);
    h.keep(-3);
    CHECK(h.kept() == 0);
    h.keep(4);
    CHECK(h.previous() == nullptr);
    CHECK(h.next() == nullptr);
}

TEST_CASE("the arrows walk round the lines there are", "[editbox]") {
    EditHistory h;
    h.keep(32);
    for (const char* line : {"one", "two", "two", "three"}) h.add(line);
    CHECK(text(h.previous()) == "three");
    CHECK(text(h.previous()) == "two");
    CHECK(text(h.previous()) == "two");   // a line sent twice is there twice
    CHECK(text(h.previous()) == "one");
    CHECK(text(h.previous()) == "three"); // past the oldest, the newest again
    CHECK(text(h.next()) == "one");
    CHECK(text(h.next()) == "two");
}

TEST_CASE("the oldest line goes when there is no more room", "[editbox]") {
    EditHistory h;
    h.keep(3);
    for (const char* line : {"a", "b", "c", "d"}) h.add(line);
    CHECK(text(h.previous()) == "d");
    CHECK(text(h.previous()) == "c");
    CHECK(text(h.previous()) == "b");
    CHECK(text(h.previous()) == "d");
}

TEST_CASE("a line sent after walking back goes in where it was walked to", "[editbox]") {
    EditHistory h;
    h.keep(8);
    for (const char* line : {"a", "b", "c"}) h.add(line);
    CHECK(text(h.previous()) == "c");
    CHECK(text(h.previous()) == "b");
    h.add("B");
    CHECK(text(h.previous()) == "B");
    CHECK(text(h.previous()) == "a");
    CHECK(text(h.next()) == "B");
    CHECK(text(h.next()) == "c");
}

TEST_CASE("fewer kept, or none of them", "[editbox]") {
    EditHistory h;
    h.keep(8);
    for (const char* line : {"a", "b", "c"}) h.add(line);
    h.keep(2);
    CHECK(h.kept() == 2);
    CHECK(text(h.previous()) == "a");
    CHECK(text(h.previous()) == "b");
    h.clear();
    CHECK(h.kept() == 2);
    CHECK(h.previous() == nullptr);
    h.add("d");
    CHECK(text(h.previous()) == "d");
    CHECK(text(h.next()) == "d");
}
