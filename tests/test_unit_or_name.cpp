// InviteUnit and UninviteUnit take a unit token or a name
// (addons/unit_or_name.hpp). InviteUnit("target") used to invite a player
// called Target.
#include <catch_amalgamated.hpp>

#include "addons/unit_or_name.hpp"

#include <map>
#include <string>

using wowee::addons::unitOrName;

namespace {
std::string nameOf(const std::string& token) {
    static const std::map<std::string, std::string> units = {
        {"target", "Vannata"}, {"focus", "Pason"}, {"party1", "Palala"}, {"mouseover", "Berodin"}};
    const auto it = units.find(token);
    return it == units.end() ? std::string() : it->second;
}
}  // namespace

TEST_CASE("a unit token invites that unit by its name", "[unit_or_name]") {
    CHECK(unitOrName("target", nameOf) == "Vannata");
    CHECK(unitOrName("focus", nameOf) == "Pason");
    CHECK(unitOrName("party1", nameOf) == "Palala");
    CHECK(unitOrName("mouseover", nameOf) == "Berodin");
}

TEST_CASE("tokens are read whatever their case", "[unit_or_name]") {
    CHECK(unitOrName("TARGET", nameOf) == "Vannata");
    CHECK(unitOrName("Party1", nameOf) == "Palala");
}

TEST_CASE("a name, or a token with no unit behind it, is taken as given", "[unit_or_name]") {
    CHECK(unitOrName("Cleando", nameOf) == "Cleando");
    CHECK(unitOrName("party4", nameOf) == "party4");
    CHECK(unitOrName("Locki", {}) == "Locki");
}
