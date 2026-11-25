#include <catch_amalgamated.hpp>

#include "rendering/memory_growth.hpp"

using namespace wowee::rendering;

TEST_CASE("an owner is the same whatever its counts", "[memory]") {
    CHECK(memoryOwnerKey("Doodad models (12): geometry") == "Doodad models : geometry");
    CHECK(memoryOwnerKey("Doodad models (9): geometry") == memoryOwnerKey("Doodad models (12): geometry"));
    CHECK(memoryOwnerKey("  (of those, 5 models with no instance: 3 MB)").empty());
    CHECK(memoryOwnerKey("A (b (c) d) e") == "A e");
}

TEST_CASE("what grows is told from what stays, per point of the change", "[memory]") {
    constexpr uint64_t kMb = 1ull << 20;
    MemoryGrowth g;
    auto lines = g.note("cleared", {{"Kept (1)", 0, 100 * kMb}, {"Leak (1)", 10 * kMb, 0}, {"Small", 0, kMb}});
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].find("+0.0 MB since the first") == 0);
    // Another point's figures are its own.
    g.note("before", {{"Leak (1)", 900 * kMb, 0}});
    g.note("cleared", {{"Kept (2)", 0, 100 * kMb}, {"Leak (2)", 30 * kMb, 0}});
    lines = g.note("cleared", {{"Kept (3)", 0, 100 * kMb}, {"Leak (3)", 55 * kMb, 0}});
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "+45.0 MB since the first, +25.0 since the last: Leak (3): GPU 55.0, CPU 0.0");
    CHECK(lines[1] == "+0.0 MB since the first, +0.0 since the last: Kept (3): GPU 0.0, CPU 100.0");
    // One that went is said too.
    lines = g.note("cleared", {{"Leak (0)", 0, 0}});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("-10.0 MB since the first, -55.0 since the last") == 0);
}
