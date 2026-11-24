// The unit tokens as the client reads them (0x0060abf0, 0x0060aaa0, 0x0060a630).
#include <catch_amalgamated.hpp>

#include "addons/unit_token.hpp"

#include <map>
#include <string>

using namespace wowee::addons::unit_token;
using wowee::game::unit_target::kAskedNobody;
using wowee::game::unit_target::kAskedNone;

namespace {

struct World {
    uint64_t targetGuid = 100, focusGuid = 0, mouseoverGuid = 0, petGuid = 20, npcGuid = 0;
    std::map<uint64_t, uint64_t> targets{{100, 101}, {101, 102}, {11, 1}, {1, 100}};
    std::map<uint64_t, uint64_t> pets{{1, 20}, {11, 21}};
    std::map<std::string, uint64_t> names{{"Bob", 11}, {"Fido", 20}, {"Ann-Realm", 12}};

    uint64_t player() const { return 1; }
    uint64_t vehicle() const { return 0; }
    uint64_t pet() const { return petGuid; }
    uint64_t target() const { return targetGuid; }
    uint64_t focus() const { return focusGuid; }
    uint64_t mouseover() const { return mouseoverGuid; }
    uint64_t npc() const { return npcGuid; }
    uint64_t questNpc() const { return 0; }
    uint64_t partyPet(int n) const { return n == 1 ? 21u : 0u; }
    uint64_t party(int n) const { return n == 1 ? 11u : 0u; }
    uint64_t raidPet(int) const { return 0; }
    uint64_t raid(int n) const { return n == 12 ? 33u : 0u; }
    uint64_t boss(int n) const { return n == 2 ? 44u : 0u; }
    uint64_t arenaPet(int n) const { return n == 3 ? 55u : 0u; }
    uint64_t arena(int n) const { return n == 3 ? 56u : 0u; }
    uint64_t commentator(int) const { return 0; }
    uint64_t targetOf(uint64_t guid) const {
        const auto it = targets.find(guid);
        return it == targets.end() ? 0 : it->second;
    }
    bool petsKnown(uint64_t guid) const { return guid == 1 || guid == 11; }
    uint64_t petOf(uint64_t guid) const {
        const auto it = pets.find(guid);
        return it == pets.end() ? 0 : it->second;
    }
    uint64_t named(std::string_view name) const {
        const auto it = names.find(std::string(name));
        return it == names.end() ? 0 : it->second;
    }
};

uint64_t unit(const char* text, const World& w = {}) { return resolve(text, w).value_or(0); }

}  // namespace

TEST_CASE("the tokens", "[unit_token]") {
    CHECK(unit("player") == 1);
    CHECK(unit("PLAYER") == 1);
    CHECK(unit("target") == 100);
    CHECK(unit("pet") == 20);
    CHECK(unit("party1") == 11);
    CHECK(unit("partypet1") == 21);
    CHECK(unit("raid12") == 33);
    CHECK(unit("boss2") == 44);
    CHECK(unit("arena3") == 56);
    CHECK(unit("arenapet3") == 55);
    World w;
    w.npcGuid = 60;
    CHECK(unit("npc", w) == 60);
    // npc, questnpc and none are whole words.
    CHECK_FALSE(resolve("npctarget", w).has_value());
    CHECK_FALSE(resolve("nonesuch", w).has_value());
    CHECK_FALSE(resolve("nonetarget", w).has_value());
}

TEST_CASE("targets and pets behind a unit", "[unit_token]") {
    CHECK(unit("targettarget") == 101);
    CHECK(unit("targettargettarget") == 102);
    CHECK(unit("target-target") == 101);
    CHECK(unit("party1target") == 1);       // not party1
    CHECK(unit("party1targettarget") == 100);
    CHECK(unit("party1pet") == 21);
    CHECK(unit("party1-pet") == 21);
    CHECK(unit("playertarget") == 100);
    CHECK(unit("playerpet") == 20);
    CHECK(unit("pettarget") == kAskedNobody);   // known, and it has none
    // Nobody else's pet is known.
    CHECK_FALSE(resolve("targetpet", World{}).has_value());
    CHECK_FALSE(resolve("playerfoo", World{}).has_value());
    CHECK_FALSE(resolve("party1x", World{}).has_value());
}

TEST_CASE("a token that names nobody", "[unit_token]") {
    const World w;
    CHECK(resolve("focus", w) == std::optional<uint64_t>(kAskedNobody));
    CHECK(resolve("party3", w) == std::optional<uint64_t>(kAskedNobody));
    CHECK(resolve("raid", w) == std::optional<uint64_t>(kAskedNobody));
    CHECK(resolve("vehicle", w) == std::optional<uint64_t>(kAskedNobody));
    // Behind nobody there is no unit.
    CHECK_FALSE(resolve("focustarget", w).has_value());
    CHECK_FALSE(resolve("party3target", w).has_value());
    CHECK(resolve("none", w) == std::optional<uint64_t>(kAskedNone));
}

TEST_CASE("names of the group", "[unit_token]") {
    const World w;
    CHECK(unit("Bob") == 11);
    CHECK(unit("Bob-target") == 1);
    CHECK(unit("Bob-pet") == 21);
    CHECK(unit("Bob-target-target") == 100);
    CHECK(unit("Fido") == 20);
    CHECK(unit("Ann-Realm") == 12);
    CHECK_FALSE(resolve("Cid", w).has_value());
    CHECK_FALSE(resolve("Bobtarget", w).has_value());
    CHECK(nameIn("Ann-Realm") == "Ann-Realm");
    CHECK(nameIn("Ann-Realm-target") == "Ann-Realm");
    CHECK(nameIn("Ann-targets") == "Ann-targets");
    CHECK(nameIn("Ann-pet-target") == "Ann");
}
