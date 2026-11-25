// CVars that hold a set of ids (addons/cvar_ids.hpp), and the dungeon finder's
// roles by class (game/lfg_roles.hpp).
#include <catch_amalgamated.hpp>

#include "addons/cvar_ids.hpp"
#include "game/lfg_roles.hpp"

using namespace wowee::addons;

// 0x00766860, 0x00766ca0
TEST_CASE("ids put in a CVar and taken out again", "[cvar_ids]") {
    std::string v;
    CHECK_FALSE(cvarHasId(v, 0));
    v = cvarWithId(v, 0, true);
    CHECK(v == "\x02""A");
    v = cvarWithId(v, 5, true);
    CHECK(v == "\x02""a");
    v = cvarWithId(v, 13, true);
    CHECK(v == "\x02""a@B");
    CHECK(cvarHasId(v, 0));
    CHECK(cvarHasId(v, 5));
    CHECK(cvarHasId(v, 13));
    CHECK_FALSE(cvarHasId(v, 1));
    CHECK_FALSE(cvarHasId(v, 12));
    CHECK_FALSE(cvarHasId(v, 300));
    v = cvarWithId(v, 13, false);
    CHECK(v == "\x02""a");
    v = cvarWithId(cvarWithId(v, 0, false), 5, false);
    CHECK(v == "\x02");
    CHECK(cvarWithId(v, 7, false) == v);
    CHECK(cvarWithId(v, 6000, true) == v);
}

// 0x00553b50
TEST_CASE("the roles a class may take", "[cvar_ids]") {
    using wowee::game::lfgClassRoles;
    CHECK(lfgClassRoles(1) == 0x0b);
    CHECK(lfgClassRoles(2) == 0x0f);
    CHECK(lfgClassRoles(5) == 0x0d);
    CHECK(lfgClassRoles(9) == 0x09);
    CHECK(lfgClassRoles(11) == 0x0f);
    CHECK(lfgClassRoles(10) == 0);
}
