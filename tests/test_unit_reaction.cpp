#include <catch_amalgamated.hpp>

#include "game/unit_reaction.hpp"

#include <unordered_map>

using namespace wowee::game::reaction;

// How one unit regards another (0x007251c0), a template another unit
// (0x0071f770), template against template (0x00715440), and a standing's
// rank (0x005d0600). Rows are FactionTemplate.dbc's own.

namespace {

FactionTemplate row(uint32_t id, uint32_t faction, uint32_t flags, uint32_t group, uint32_t friendGroup,
                    uint32_t enemyGroup) {
    FactionTemplate t;
    t.id = id;
    t.faction = faction;
    t.flags = flags;
    t.factionGroup = group;
    t.friendGroup = friendGroup;
    t.enemyGroup = enemyGroup;
    return t;
}

const FactionTemplate kHuman = row(1, 1, 72, 3, 2, 12);
const FactionTemplate kOrc = row(2, 2, 72, 5, 4, 10);
const FactionTemplate kBoar = row(7, 7, 0, 0, 0, 0);
const FactionTemplate kMonster = row(14, 14, 0, 8, 0, 1);
const FactionTemplate kCreature = row(114, 77, 0, 0, 0, 1);

struct World {
    std::unordered_map<uint32_t, FactionTemplate> rows;
    std::unordered_map<uint32_t, int> forcedReactions;
    std::unordered_map<uint32_t, int32_t> standings;
    std::unordered_map<uint32_t, bool> war;
    uint64_t activePlayer = 1;
    uint64_t duelOpponent = 0;

    const FactionTemplate* factionTemplate(uint32_t id) const {
        auto it = rows.find(id);
        return it != rows.end() ? &it->second : nullptr;
    }
    bool forced(uint32_t f, int& out) const {
        auto it = forcedReactions.find(f);
        if (it == forcedReactions.end()) return false;
        out = it->second;
        return true;
    }
    bool tracked(uint32_t f) const { return standings.count(f) != 0; }
    bool atWar(uint32_t f) const { return war.count(f) != 0 && war.at(f); }
    int rank(uint32_t f) const { return standingRank(standings.at(f)); }
};

World world() {
    World w;
    for (const auto& t : {kHuman, kOrc, kBoar, kMonster, kCreature}) w.rows[t.id] = t;
    return w;
}

Who player(uint64_t guid, uint32_t ft) {
    Who p;
    p.guid = guid;
    p.factionTemplate = ft;
    p.unitFlags = 0x8;
    p.self.found = true;
    p.self.guid = guid;
    p.behind = p.self;
    return p;
}

Who creature(uint64_t guid, uint32_t ft) {
    Who c;
    c.guid = guid;
    c.factionTemplate = ft;
    return c;
}

}  // namespace

TEST_CASE("template against template (0x00715440)", "[reaction]") {
    CHECK(templateReaction(kHuman, kOrc) == kHostile);
    CHECK(templateReaction(kHuman, kMonster) == kHostile);
    CHECK(templateReaction(kHuman, kBoar) == kNeutral);
    CHECK(templateReaction(kHuman, kHuman) == kFriendly);
    // "Creature" names the players as its enemies; they have nothing against it.
    CHECK(templateReaction(kCreature, kHuman) == kHostile);
    CHECK(templateReaction(kHuman, kCreature) == kNeutral);
    // Friends by name, either way round; hostile by default otherwise.
    FactionTemplate a = row(100, 50, 0, 0, 0, 0);
    FactionTemplate b = row(101, 51, 0x2000, 0, 0, 0);
    CHECK(templateReaction(b, a) == kHostile);
    a.friends = {51, 0, 0, 0};
    CHECK(templateReaction(b, a) == kFriendly);
    CHECK(templateReaction(a, b) == kFriendly);
    // An enemy by name first, and the list ends at its first 0.
    b.enemies = {50, 0, 0, 0};
    CHECK(templateReaction(b, a) == kHostile);
    b.enemies = {0, 50, 0, 0};
    CHECK(templateReaction(b, a) == kFriendly);
}

TEST_CASE("a standing as a rank (0x005d0600)", "[reaction]") {
    CHECK(standingRank(42000) == 7);
    CHECK(standingRank(41999) == 6);
    CHECK(standingRank(9000) == 5);
    CHECK(standingRank(3000) == 4);
    CHECK(standingRank(2999) == 3);
    CHECK(standingRank(0) == 3);
    CHECK(standingRank(-1) == 2);
    CHECK(standingRank(-3000) == 2);
    CHECK(standingRank(-3001) == 1);
    CHECK(standingRank(-6000) == 1);
    CHECK(standingRank(-6001) == 0);
}

TEST_CASE("the player and a creature with no standing go by the templates", "[reaction]") {
    World w = world();
    const Who me = player(1, 1);
    CHECK(unitReaction(w, me, creature(10, 7)) == kNeutral);
    CHECK(unitReaction(w, me, creature(10, 14)) == kHostile);
    CHECK(unitReaction(w, creature(10, 114), me) == kHostile);
    CHECK(unitReaction(w, me, creature(10, 114)) == kNeutral);
    CHECK(unitReaction(w, me, me) == kFriendly);
    // No template on either side is neutral.
    CHECK(unitReaction(w, me, creature(10, 9999)) == kNeutral);
}

TEST_CASE("a faction with a standing: at war from the player's side, the rank from the unit's", "[reaction]") {
    World w = world();
    w.rows[1000] = row(1000, 500, 0, 0, 0, 0);
    w.standings[500] = 900 - 1200;  // unfriendly
    const Who me = player(1, 1);
    const Who npc = creature(10, 1000);
    CHECK(unitReaction(w, me, npc) == kFriendly);
    CHECK(unitReaction(w, npc, me) == 2);
    w.war[500] = true;
    CHECK(unitReaction(w, me, npc) == kHostile);
    CHECK(unitReaction(w, npc, me) == 2);
    w.standings[500] = 22000;
    CHECK(unitReaction(w, npc, me) == 6);
    // Not of a player that ignores reputation (UNIT_FIELD_FLAGS_2 0x4).
    Who ignoring = me;
    ignoring.unitFlags2 = 0x4;
    ignoring.behind.unitFlags2 = 0x4;
    CHECK(unitReaction(w, ignoring, npc) == kNeutral);
    CHECK(unitReaction(w, npc, ignoring) == kNeutral);
    // A forced reaction first, both ways.
    w.forcedReactions[500] = 5;
    CHECK(unitReaction(w, me, npc) == 5);
    CHECK(unitReaction(w, npc, me) == 5);
    // A pet the player controls asks as the player does.
    Who pet = creature(20, 1);
    pet.unitFlags = 0x8;
    pet.behind = me.self;
    CHECK(unitReaction(w, pet, npc) == 5);
    // Not one somebody else controls.
    pet.behind.guid = 2;
    CHECK(unitReaction(w, pet, npc) == kNeutral);
}

TEST_CASE("a contested guard and a contested player", "[reaction]") {
    World w = world();
    w.rows[1001] = row(1001, 501, 0x1000, 0, 0, 0);
    w.standings[501] = 0;
    Who me = player(1, 1);
    const Who guard = creature(10, 1001);
    CHECK(unitReaction(w, me, guard) == kFriendly);
    me.self.playerFlags = me.behind.playerFlags = 0x100;
    CHECK(unitReaction(w, me, guard) == kHostile);
    CHECK(unitReaction(w, guard, me) == kHostile);
}

TEST_CASE("between players: duels, one player, the group, free-for-all", "[reaction]") {
    World w = world();
    Who me = player(1, 1);
    Who other = player(2, 1);
    CHECK(unitReaction(w, me, other) == kFriendly);
    // Free-for-all both: hostile, unless grouped.
    me.pvpFlags = other.pvpFlags = 0x4;
    CHECK(unitReaction(w, me, other) == kHostile);
    other.behind.grouped = other.self.grouped = true;
    CHECK(unitReaction(w, me, other) == kFriendly);
    // A duel decides first, by side.
    me.self.duelArbiter = me.behind.duelArbiter = 77;
    other.self.duelArbiter = other.behind.duelArbiter = 77;
    me.self.duelTeam = me.behind.duelTeam = 1;
    other.self.duelTeam = other.behind.duelTeam = 2;
    CHECK(unitReaction(w, me, other) == kHostile);
    other.self.duelTeam = other.behind.duelTeam = 1;
    CHECK(unitReaction(w, me, other) == kFriendly);
    // The player's own pet is friendly whatever else.
    Who pet = creature(20, 2);
    pet.unitFlags = 0x8;
    pet.pvpFlags = 0x4;
    pet.behind = me.self;
    CHECK(unitReaction(w, me, pet) == kFriendly);
    // An Orc player is hostile by the templates.
    Who orc = player(3, 2);
    CHECK(unitReaction(w, player(1, 1), orc) == kHostile);
}

TEST_CASE("one with no player behind it, made by the duel opponent (0x0071f5c0)", "[reaction]") {
    World w = world();
    const Who me = player(1, 1);
    Who thing = creature(30, 1);
    thing.unitFlags = 0x8;
    thing.charmerOrCreator = 2;
    CHECK(unitReaction(w, me, thing) == kFriendly);
    w.duelOpponent = 2;
    CHECK(unitReaction(w, me, thing) == kHostile);
    CHECK(unitReaction(w, thing, me) == kHostile);
}

TEST_CASE("one side by faction group, neither charmed, not attackable (0x00729b30)", "[reaction]") {
    // Two Alliance players who may not attack each other.
    CHECK(sameSide(&kHuman, &kHuman, false, false, false));
    // An Alliance player and a boar: groups 3 and 0 differ, whatever else.
    CHECK_FALSE(sameSide(&kHuman, &kBoar, false, false, false));
    // Same group, but in a duel or flagged: may attack, so not cooperating.
    CHECK_FALSE(sameSide(&kHuman, &kHuman, false, false, true));
    // Either charmed, whatever the groups.
    CHECK_FALSE(sameSide(&kHuman, &kHuman, true, false, false));
    CHECK_FALSE(sameSide(&kHuman, &kHuman, false, true, false));
    // A missing template leaves only the attack question.
    CHECK(sameSide(&kHuman, nullptr, false, false, false));
    CHECK(sameSide(nullptr, &kOrc, false, false, false));
    CHECK_FALSE(sameSide(nullptr, &kOrc, false, false, true));
}
