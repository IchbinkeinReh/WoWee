// An addon's required dependencies as the client reads its TOC (0x005f86a0):
// every directive beginning "RequiredDep" or "Dep", split at spaces and commas.
#include <catch_amalgamated.hpp>

#include "addons/toc_parser.hpp"

using wowee::addons::TocFile;

TEST_CASE("Dependencies and RequiredDeps are both required", "[toc]") {
    TocFile toc;
    toc.directives["Dependencies"] = "Ace3, LibStub";
    toc.directives["RequiredDeps"] = "Bagnon_Core";
    toc.directives["OptionalDeps"] = "Masque";
    toc.directives["Title"] = "Test";
    const auto deps = toc.getRequiredDependencies();
    REQUIRE(deps.size() == 3);
    // Directives in name order: Dependencies before RequiredDeps.
    CHECK(deps[0] == "Ace3");
    CHECK(deps[1] == "LibStub");
    CHECK(deps[2] == "Bagnon_Core");
}

TEST_CASE("the prefix is matched without case, and a bare Dep counts", "[toc]") {
    TocFile toc;
    toc.directives["dep"] = "Foo Bar";
    const auto deps = toc.getRequiredDependencies();
    REQUIRE(deps.size() == 2);
    CHECK(deps[0] == "Foo");
    CHECK(deps[1] == "Bar");
}

TEST_CASE("no dependency directive, no dependencies", "[toc]") {
    TocFile toc;
    toc.directives["Interface"] = "30300";
    CHECK(toc.getRequiredDependencies().empty());
}
