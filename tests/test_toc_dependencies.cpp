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

TEST_CASE("the other lists of a .toc", "[toc]") {
    TocFile toc;
    toc.directives["OptionalDeps"] = "Masque, LibStub";
    toc.directives["LoadWith"] = "Blizzard_AuctionUI";
    toc.directives["LoadManagers"] = "AddonLoader";
    toc.directives["Dependencies"] = "Ace3";
    CHECK(toc.getOptionalDependencies() == std::vector<std::string>{"Masque", "LibStub"});
    CHECK(toc.getLoadWith() == std::vector<std::string>{"Blizzard_AuctionUI"});
    CHECK(toc.getLoadManagers() == std::vector<std::string>{"AddonLoader"});
    CHECK(toc.getRequiredDependencies() == std::vector<std::string>{"Ace3"});
}

// 0x005f85d0
TEST_CASE("a .toc's line for another language is none", "[toc]") {
    using wowee::addons::tocDirective;
    using Directive = std::pair<std::string, std::string>;
    CHECK(tocDirective(" Title: Probe", "deDE") == Directive{"Title", "Probe"});
    CHECK(tocDirective(" Title-deDE: Probe A", "deDE") == Directive{"Title", "Probe A"});
    CHECK(tocDirective(" Title-dede : Probe A", "deDE") == Directive{"Title", "Probe A"});
    CHECK(tocDirective("Title-frFR: Essai", "deDE").first.empty());
    CHECK(tocDirective("Notes -frFR: Essai", "frFR") == Directive{"Notes", "Essai"});
    CHECK(tocDirective("X-Category-ruRU: A", "deDE").first.empty());
    CHECK(tocDirective("X-Category-Mage: A", "deDE") == Directive{"X-Category-Mage", "A"});
    CHECK(tocDirective("X-Website: http://a-b.c", "deDE") == Directive{"X-Website", "http://a-b.c"});
    CHECK(tocDirective("SavedVariables-frFR: A", "deDE") == Directive{"SavedVariables-frFR", "A"});
    CHECK(tocDirective("no directive", "deDE").first.empty());
}
