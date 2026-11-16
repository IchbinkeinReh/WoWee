// UNIT_COMBAT's words as 0x005132c0 builds them: the flag from the amount and
// the hit flags, the action tables of a swing (0x00ac8050) and of a spell's
// miss (0x00adbfac), and the hit flags the spell logs build.
#include <catch_amalgamated.hpp>

#include <string>

#include "game/unit_combat.hpp"

namespace uc = wowee::game::unit_combat;

TEST_CASE("A hit that did nothing says why, one that landed says how", "[unit_combat]") {
    CHECK(std::string(uc::flag(0, 0x20)) == "ABSORB");
    CHECK(std::string(uc::flag(0, 0x2000 | 0x80)) == "BLOCK");
    CHECK(std::string(uc::flag(0, 0x80)) == "RESIST");
    CHECK(std::string(uc::flag(0, 0x200)) == "");  // a crit of nothing is no crit
    CHECK(std::string(uc::flag(120, 0x20 | 0x200)) == "CRITICAL");
    CHECK(std::string(uc::flag(120, 0x10000 | 0x20000)) == "GLANCING");
    CHECK(std::string(uc::flag(120, 0x20000)) == "CRUSHING");
    CHECK(std::string(uc::flag(120, 0x20)) == "");
    CHECK(std::string(uc::flag(-5, 0x20)) == "ABSORB");
}

TEST_CASE("Swing and miss actions follow the client's tables", "[unit_combat]") {
    CHECK(std::string(uc::meleeAction(0)) == "MISS");
    CHECK(std::string(uc::meleeAction(1)) == "WOUND");
    CHECK(std::string(uc::meleeAction(4)) == "INTERRUPT");
    CHECK(std::string(uc::meleeAction(8)) == "DEFLECT");
    CHECK(uc::meleeAction(9) == nullptr);
    CHECK(std::string(uc::missAction(0)) == "NONE");
    CHECK(std::string(uc::missAction(2)) == "RESIST");
    CHECK(std::string(uc::missAction(7)) == "IMMUNE");
    CHECK(std::string(uc::missAction(8)) == "IMMUNE");
    CHECK(std::string(uc::missAction(10)) == "ABSORB");
    CHECK(std::string(uc::missAction(11)) == "REFLECT");
    CHECK(uc::missAction(12) == nullptr);
}

TEST_CASE("Spell logs build their hit flags and hide what the log hides", "[unit_combat]") {
    CHECK(uc::hitInfoFromLog(0, 0, 0, false) == 0u);
    CHECK(uc::hitInfoFromLog(5, 0, 0, false) == 0x20u);
    CHECK(uc::hitInfoFromLog(0, 5, 0, false) == 0x80u);
    CHECK(uc::hitInfoFromLog(0, 0, 5, false) == 0x2000u);
    CHECK(uc::hitInfoFromLog(1, 1, 1, true) == (0x20u | 0x80u | 0x2000u | 0x200u));
    CHECK(uc::spellRaises(0u));
    CHECK_FALSE(uc::spellRaises(0x100u));
    CHECK_FALSE(uc::spellRaises(std::nullopt));
}
