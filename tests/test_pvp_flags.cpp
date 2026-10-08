// The PvP flags two players' attack decision reads (0x00729740), and where
// each version keeps them.
#include <catch_amalgamated.hpp>

#include "game/pvp_flags.hpp"

using namespace wowee::game;

TEST_CASE("WotLK keeps the PvP flags in byte 1 of UNIT_FIELD_BYTES_2", "[pvp]") {
    // Sheath state melee in byte 0, PvP and sanctuary in byte 1.
    CHECK(unitPvpFlags(true, 0x0901u, 0) == (pvp::kPvp | pvp::kSanctuary));
    CHECK(unitPvpFlags(true, 0x0001u, kUnitFlagPvp) == 0);
}

TEST_CASE("Classic and TBC read UNIT_FIELD_FLAGS, not BYTES_2's byte 1", "[pvp]") {
    // Byte 1 of their BYTES_2 is other flags (0x10 the aura display).
    CHECK(unitPvpFlags(false, 0x1001u, 0) == 0);
    CHECK(unitPvpFlags(false, 0x1001u, kUnitFlagPvp) == pvp::kPvp);
}

TEST_CASE("who may attack whom by the flags", "[pvp]") {
    CHECK(pvpFlagsAllowAttack(0, pvp::kPvp));
    CHECK_FALSE(pvpFlagsAllowAttack(pvp::kPvp, 0));
    CHECK(pvpFlagsAllowAttack(pvp::kContested, 0));
    CHECK(pvpFlagsAllowAttack(0, pvp::kContested));
    CHECK(pvpFlagsAllowAttack(pvp::kFreeForAll, pvp::kFreeForAll));
    CHECK_FALSE(pvpFlagsAllowAttack(pvp::kSanctuary, pvp::kPvp));
    CHECK_FALSE(pvpFlagsAllowAttack(0, pvp::kPvp | pvp::kSanctuary));
}
