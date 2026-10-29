// The wire indices of the update fields, against an independent oracle.
//
// A field index is a slot number in the update block the server sends. Read
// the wrong slot and nothing fails: you get the value of whatever field sits
// there, which for a guid is usually zero, so the feature reads as "the server
// never sent it" rather than as a mistake.
//
// UNIT_FIELD_TARGET was 6 for WotLK, which is UNIT_FIELD_CHARM. Vanilla and
// TBC have it at 16 and those were right; WotLK inserts UNIT_FIELD_CRITTER
// among the earlier guid fields, which moves TARGET to 18. Unit frames, the
// target-of-target display and combat targeting all read it, and a charm guid
// is almost always zero, so they showed no target at all.
//
// The oracle is AzerothCore's UpdateFields.h, read once and recorded here.
// Comparing our whole table against it found that one field out of 75.
#include <catch_amalgamated.hpp>

#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>

namespace {

#ifdef WOWEE_SOURCE_DIR
const std::string kRoot = std::string(WOWEE_SOURCE_DIR) + "/";
#else
const std::string kRoot;
#endif

std::map<std::string, int> declaredFields(const std::string& expansion) {
    std::ifstream in(kRoot + "Data/expansions/" + expansion + "/update_fields.json");
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string json = ss.str();
    std::map<std::string, int> out;
    const std::regex pair(R"RX("(\w+)"\s*:\s*(\d+))RX");
    for (std::sregex_iterator it(json.begin(), json.end(), pair), end; it != end; ++it) {
        out[(*it)[1].str()] = std::stoi((*it)[2].str());
    }
    return out;
}

}  // namespace

TEST_CASE("the WotLK field indices are what the server sends", "[update-fields]") {
    // Recorded from AzerothCore's UpdateFields.h. Every one of the 75 fields
    // in the table agreed once UNIT_FIELD_TARGET was corrected; these are the
    // ones whose being wrong would be felt.
    const std::map<std::string, int> oracle = {
        {"OBJECT_FIELD_ENTRY", 3},
        {"OBJECT_FIELD_SCALE_X", 4},
        {"UNIT_FIELD_TARGET_LO", 18},
        {"UNIT_FIELD_TARGET_HI", 19},
        {"UNIT_FIELD_HEALTH", 24},
        {"UNIT_FIELD_MAXHEALTH", 32},
        {"UNIT_FIELD_LEVEL", 54},
        {"UNIT_FIELD_FACTIONTEMPLATE", 55},
        {"UNIT_FIELD_FLAGS", 59},
        {"UNIT_FIELD_DISPLAYID", 67},
        {"UNIT_FIELD_MOUNTDISPLAYID", 69},
        {"UNIT_NPC_FLAGS", 82},
        {"PLAYER_EXPLORED_ZONES_START", 1041},
        {"PLAYER_QUEST_LOG_START", 158},
        {"PLAYER_SKILL_INFO_START", 636},
        // And the client's own reads, 0xda8 and 0xdac past UNIT_END
        // (0x006dca00, 0x006dca90).
        {"PLAYER_TRACK_CREATURES", 1022},
        {"PLAYER_TRACK_RESOURCES", 1023},
        // The arena teams' block, 0x1150 past UNIT_END (0x005bc240).
        {"PLAYER_FIELD_ARENA_TEAM_INFO_1_1", 1256},
    };

    const auto ours = declaredFields("wotlk");
    REQUIRE(ours.size() > 50);
    for (const auto& [name, index] : oracle) {
        INFO(name);
        const auto found = ours.find(name);
        REQUIRE(found != ours.end());
        CHECK(found->second == index);
    }
}

TEST_CASE("the target field moved between vanilla and WotLK",
          "[update-fields]") {
    // The difference that was missed. WotLK inserts UNIT_FIELD_CRITTER among
    // the guid fields before TARGET, so a value correct for one is wrong for
    // the other - and both are plausible slot numbers.
    for (const char* expansion : {"classic", "tbc", "turtle"}) {
        const auto fields = declaredFields(expansion);
        INFO(expansion);
        REQUIRE(fields.count("UNIT_FIELD_TARGET_LO") == 1);
        CHECK(fields.at("UNIT_FIELD_TARGET_LO") == 16);
        CHECK(fields.at("UNIT_FIELD_TARGET_HI") == 17);
    }

    const auto wotlk = declaredFields("wotlk");
    CHECK(wotlk.at("UNIT_FIELD_TARGET_LO") == 18);
    CHECK(wotlk.at("UNIT_FIELD_TARGET_HI") == 19);
}

TEST_CASE("the two halves of a guid field are adjacent", "[update-fields]") {
    // A guid occupies two slots. Any pair that is not consecutive is reading
    // half of one field and half of another.
    for (const char* expansion : {"classic", "tbc", "wotlk", "turtle"}) {
        const auto fields = declaredFields(expansion);
        INFO(expansion);
        REQUIRE(fields.count("UNIT_FIELD_TARGET_LO") == 1);
        CHECK(fields.at("UNIT_FIELD_TARGET_HI") ==
              fields.at("UNIT_FIELD_TARGET_LO") + 1);
    }
}

TEST_CASE("the summoner field sits where the target field says it does",
          "[update-fields]") {
    // UNIT_FIELD_SUMMONEDBY was added from the server's own header, where it
    // is OBJECT_END + 0x08 with UNIT_FIELD_CREATEDBY between it and the target
    // field. That gap is the check: CREATEDBY takes two slots, so the summoner
    // is always four below the target, in every layout. Vanilla and WotLK
    // disagree about the absolute numbers - WotLK inserted UNIT_FIELD_CRITTER
    // ahead of both - and they agree about this distance.
    //
    // Worth pinning because the failure is silent. A slipped column reads as
    // zero for every unit, which is indistinguishable from "nobody summoned
    // it", and every pet in the world would quietly become an ordinary
    // creature.
    for (const char* expansion : {"classic", "tbc", "wotlk", "turtle"}) {
        const auto fields = declaredFields(expansion);
        INFO(expansion);
        REQUIRE(fields.count("UNIT_FIELD_SUMMONEDBY_LO") == 1);
        CHECK(fields.at("UNIT_FIELD_SUMMONEDBY_HI") ==
              fields.at("UNIT_FIELD_SUMMONEDBY_LO") + 1);
        CHECK(fields.at("UNIT_FIELD_SUMMONEDBY_LO") + 4 ==
              fields.at("UNIT_FIELD_TARGET_LO"));
    }

    // And the one absolute the server header states outright.
    const auto wotlk = declaredFields("wotlk");
    CHECK(wotlk.at("UNIT_FIELD_SUMMONEDBY_LO") == 14);
}

TEST_CASE("the charmer and the maker sit either side of the summoner",
          "[update-fields]") {
    // CHARMEDBY, SUMMONEDBY and CREATEDBY are three guid pairs in a row; the
    // owner the client asks about is the charmer, else the maker (0x004f5f20).
    for (const char* expansion : {"classic", "tbc", "wotlk", "turtle"}) {
        const auto fields = declaredFields(expansion);
        INFO(expansion);
        REQUIRE(fields.count("UNIT_FIELD_CREATEDBY") == 1);
        REQUIRE(fields.count("UNIT_FIELD_CHARMEDBY") == 1);
        CHECK(fields.at("UNIT_FIELD_CREATEDBY") == fields.at("UNIT_FIELD_SUMMONEDBY_LO") + 2);
        CHECK(fields.at("UNIT_FIELD_CHARMEDBY") == fields.at("UNIT_FIELD_SUMMONEDBY_LO") - 2);
    }
    CHECK(declaredFields("wotlk").at("UNIT_FIELD_CREATEDBY") == 16);
    // The pet a unit summoned follows what it charms (0x0052d310: +0, else +8).
    CHECK(declaredFields("wotlk").at("UNIT_FIELD_SUMMON") == declaredFields("wotlk").at("UNIT_FIELD_CHARM") + 2);
}

TEST_CASE("the glyph run and the pet's spell power are consecutive dwords",
          "[update-fields]") {
    // The client's player field array lays these end to end: six GlyphSlot.dbc
    // ids at +0x1230, the six GlyphProperties ids scribed in them at +0x1248, the
    // bitmask of unlocked sockets at +0x1260, and the pet's spell power at
    // +0x1264 - which is what GetPetSpellBonusDamage reads (0x0060e410) and what
    // the watcher that raises PET_SPELL_POWER_UPDATE is registered on
    // (0x006d1cb0). The glyph watcher names a socket as (offset - 0x1248) >> 2,
    // so the six id slots have to be consecutive and in order; a table one dword
    // off fires GLYPH_ADDED about a socket that never changed.
    const auto wotlk = declaredFields("wotlk");
    REQUIRE(wotlk.count("PLAYER_FIELD_GLYPH_SLOTS_1") == 1);
    REQUIRE(wotlk.count("PLAYER_FIELD_PET_SPELL_POWER") == 1);
    CHECK(wotlk.at("PLAYER_FIELD_GLYPHS_1") == wotlk.at("PLAYER_FIELD_GLYPH_SLOTS_1") + 6);
    CHECK(wotlk.at("PLAYER_GLYPHS_ENABLED") == wotlk.at("PLAYER_FIELD_GLYPHS_1") + 6);
    CHECK(wotlk.at("PLAYER_FIELD_PET_SPELL_POWER") == wotlk.at("PLAYER_GLYPHS_ENABLED") + 1);
}

TEST_CASE("the shield block value is the dword after the spell crit floats",
          "[update-fields]") {
    // GetShieldBlock (0x0060dff0) reads +0xdec into the field blob the player
    // object keeps at +0x1008 - dword 891 of it - and the client's own
    // per-field descriptor table in .data names that dword
    // PLAYER_SHIELD_BLOCK.  It comes straight after the seven spell-crit
    // percentages, which is the check worth having: the sheet's block tooltip
    // (paperdollframe.lua:479) prints the number with no range to it, so a
    // column off by one answers with a crit percentage or with the explored
    // zones' first byte, and both read as a plausible figure.
    const auto wotlk = declaredFields("wotlk");
    REQUIRE(wotlk.count("PLAYER_SHIELD_BLOCK") == 1);
    CHECK(wotlk.at("PLAYER_SHIELD_BLOCK") ==
          wotlk.at("PLAYER_SPELL_CRIT_PERCENTAGE1") + 7);
    CHECK(wotlk.at("PLAYER_SHIELD_BLOCK") + 2 ==
          wotlk.at("PLAYER_EXPLORED_ZONES_START"));
    // 891 dwords into the player's own blob, and WoWee numbers the player
    // fields from UNIT_END (148): 891 + 148.
    CHECK(wotlk.at("PLAYER_SHIELD_BLOCK") == 1039);
}

TEST_CASE("the resistance a target loses is three past the healing bonus",
          "[update-fields]") {
    // GetSpellPenetration (0x0060e470) reads +0x105c into the field blob the
    // player object keeps at +0x1008 - dword 1047 - which the client's own
    // per-field descriptor table names PLAYER_FIELD_MOD_TARGET_RESISTANCE.  The
    // character sheet prints its negation as its own stat row, so a wrong slot
    // shows a number of the right shape: the healing-percent float two below it
    // would read as a penetration of 0.01 or so, and the healing bonus next
    // door as hundreds.  Pinning the run keeps both halves of that gap honest.
    const auto wotlk = declaredFields("wotlk");
    REQUIRE(wotlk.count("PLAYER_FIELD_MOD_TARGET_RESISTANCE") == 1);
    CHECK(wotlk.at("PLAYER_FIELD_MOD_TARGET_RESISTANCE") ==
          wotlk.at("PLAYER_FIELD_MOD_HEALING_DONE_POS") + 3);
    CHECK(wotlk.at("PLAYER_FIELD_MOD_TARGET_RESISTANCE") + 2 ==
          wotlk.at("PLAYER_FIELD_BYTES"));
    // 1047 dwords into the player's own blob, 148 of WoWee's numbering ahead.
    CHECK(wotlk.at("PLAYER_FIELD_MOD_TARGET_RESISTANCE") == 1195);
}

TEST_CASE("the unit's three attack times are one run of fields",
          "[update-fields]") {
    // The client's UnitAttackSpeed (0x00610a00) and UnitRangedDamage
    // (0x00610550) read the trio straight out of the field blob at +0xe0,
    // +0xe4 and +0xe8 - consecutive dwords, the last of which this table
    // already names as UNIT_FIELD_RANGEDATTACKTIME, 64. So the main-hand and
    // off-hand pair is 62 and 63: across the unit fields the blob runs six
    // dwords behind the wire index, which UnitLevel (0x0060f9e0, +0xc0 for
    // LEVEL's 54) and MINDAMAGE/MAXDAMAGE (+0x100 and +0x104 for WoWee's 70
    // and 71) both show.
    //
    // Worth pinning because a slipped index reads a convincing number rather
    // than an obviously wrong one: the neighbours here are a faction template
    // and a mount's display id, and 2050 divided by 1000 is a perfectly
    // plausible weapon speed.
    const auto wotlk = declaredFields("wotlk");
    REQUIRE(wotlk.count("UNIT_FIELD_BASEATTACKTIME") == 1);
    REQUIRE(wotlk.count("UNIT_FIELD_OFFHANDATTACKTIME") == 1);
    CHECK(wotlk.at("UNIT_FIELD_BASEATTACKTIME") + 1 ==
          wotlk.at("UNIT_FIELD_OFFHANDATTACKTIME"));
    CHECK(wotlk.at("UNIT_FIELD_OFFHANDATTACKTIME") + 1 ==
          wotlk.at("UNIT_FIELD_RANGEDATTACKTIME"));
}

TEST_CASE("the two damage pairs sit where the client reads them",
          "[update-fields]") {
    // UnitDamage (0x00610860) reads four consecutive dwords of the field blob
    // at +0x100, +0x104, +0x108 and +0x10c: MINDAMAGE and MAXDAMAGE, which this
    // table has always named, and the off-hand pair, which it did not. Both ends
    // of that run are anchors this table already carries - MAXDAMAGE below and
    // BYTES_1's 74 above, which combat_handler independently records the realm
    // writing at 74 - so the pair is 72 and 73 with the blob drift as no more
    // than the arithmetic between them.
    //
    // The ranged pair is the same shape on the other side of the block:
    // UnitRangedDamage (0x00610550) reads +0x1ec and +0x1f0, and the client's
    // own field-name table lists MINRANGEDDAMAGE and MAXRANGEDDAMAGE directly
    // below POWER_COST_MODIFIER, which this table has at 131. RANGED_ATTACK_POWER
    // at 126 with its two mod fields bounds the run from below, so 129 and 130
    // are pinned twice over.
    //
    // A slipped index here is the convincing kind rather than the obvious one:
    // one low reads the ranged attack power multiplier, one high reads a power
    // cost modifier, and both are floats near one - a damage range of
    // "1.0 - 1.0" looks like an empty field, not a wrong one.
    const auto wotlk = declaredFields("wotlk");
    for (const char* name : {"UNIT_FIELD_MINOFFHANDDAMAGE", "UNIT_FIELD_MAXOFFHANDDAMAGE",
                             "UNIT_FIELD_MINRANGEDDAMAGE", "UNIT_FIELD_MAXRANGEDDAMAGE"})
        REQUIRE(wotlk.count(name) == 1);
    CHECK(wotlk.at("UNIT_FIELD_MINDAMAGE") + 1 == wotlk.at("UNIT_FIELD_MAXDAMAGE"));
    CHECK(wotlk.at("UNIT_FIELD_MAXDAMAGE") + 1 == wotlk.at("UNIT_FIELD_MINOFFHANDDAMAGE"));
    CHECK(wotlk.at("UNIT_FIELD_MINOFFHANDDAMAGE") + 1 == wotlk.at("UNIT_FIELD_MAXOFFHANDDAMAGE"));
    CHECK(wotlk.at("UNIT_FIELD_MAXOFFHANDDAMAGE") + 1 == wotlk.at("UNIT_FIELD_BYTES_1"));
    CHECK(wotlk.at("UNIT_FIELD_RANGED_ATTACK_POWER") + 3 == wotlk.at("UNIT_FIELD_MINRANGEDDAMAGE"));
    CHECK(wotlk.at("UNIT_FIELD_MINRANGEDDAMAGE") + 1 == wotlk.at("UNIT_FIELD_MAXRANGEDDAMAGE"));
    CHECK(wotlk.at("UNIT_FIELD_MAXRANGEDDAMAGE") + 1 ==
          wotlk.at("UNIT_FIELD_POWER_COST_MODIFIER"));
}

TEST_CASE("each attack-power base is followed by its mods and its multiplier",
          "[update-fields]") {
    // UnitAttackPower (0x00610b60) reads three consecutive dwords of the field
    // blob - +0x1d4, +0x1d8, +0x1dc - and multiplies every answer by the float
    // at the end of the run; UnitRangedAttackPower (0x00610ca0) reads the run
    // again three dwords later at +0x1e0, +0x1e4, +0x1e8. The bases are 123 and
    // 126, which this table has always had, so each mods/multiplier pair is
    // pinned between the base before it and the next one after it - and the
    // ranged pair has MINRANGEDDAMAGE's 129 as its upper neighbour too.
    //
    // The multiplier is a float, which makes a wrong index here the quiet kind
    // of wrong: the ranged attack power base read as a multiplier scales every
    // attack-power number on the sheet by roughly two hundred million.
    const auto wotlk = declaredFields("wotlk");
    for (const char* name : {"UNIT_FIELD_ATTACK_POWER_MODS", "UNIT_FIELD_ATTACK_POWER_MULTIPLIER",
                             "UNIT_FIELD_RANGED_ATTACK_POWER_MODS",
                             "UNIT_FIELD_RANGED_ATTACK_POWER_MULTIPLIER"})
        REQUIRE(wotlk.count(name) == 1);
    CHECK(wotlk.at("UNIT_FIELD_ATTACK_POWER") + 1 == wotlk.at("UNIT_FIELD_ATTACK_POWER_MODS"));
    CHECK(wotlk.at("UNIT_FIELD_ATTACK_POWER_MODS") + 1 ==
          wotlk.at("UNIT_FIELD_ATTACK_POWER_MULTIPLIER"));
    CHECK(wotlk.at("UNIT_FIELD_ATTACK_POWER_MULTIPLIER") + 1 ==
          wotlk.at("UNIT_FIELD_RANGED_ATTACK_POWER"));
    CHECK(wotlk.at("UNIT_FIELD_RANGED_ATTACK_POWER") + 1 ==
          wotlk.at("UNIT_FIELD_RANGED_ATTACK_POWER_MODS"));
    CHECK(wotlk.at("UNIT_FIELD_RANGED_ATTACK_POWER_MODS") + 1 ==
          wotlk.at("UNIT_FIELD_RANGED_ATTACK_POWER_MULTIPLIER"));
    CHECK(wotlk.at("UNIT_FIELD_RANGED_ATTACK_POWER_MULTIPLIER") + 1 ==
          wotlk.at("UNIT_FIELD_MINRANGEDDAMAGE"));
}

TEST_CASE("each stat carries its two buff halves above it", "[update-fields]") {
    // UnitStat (0x00610300) takes three dwords per stat out of the field blob -
    // +0x138, +0x14c and +0x160 - which with the blob running six dwords behind
    // the wire index is 84, 89 and 94: STAT0..4 as the total, then POSSTAT0..4
    // and NEGSTAT0..4, the halves paperdollframe.lua:246 subtracts back out of
    // the first to print the unbuffed figure beside the buffed one.
    //
    // The client names all fifteen itself. Its field-name table steps down from
    // RESISTANCES at 0x009f8c40 through NEGSTAT4..0, POSSTAT4..0 to STAT0 at
    // 0x009f8d70, so one name per element here, and both ends of that run are
    // keys this table has always had - STAT0's 84 under it, RESISTANCES' 99
    // over. The buff runs are pinned by the reads and by the count between two
    // names already in the table, which is what makes the middle link safe.
    //
    // A slipped index is the convincing kind again: one low and POSSTAT0 reads
    // STAT4, a perfectly plausible Stamina, and the tooltip goes on showing a
    // breakdown that adds up to neither number.
    const auto wotlk = declaredFields("wotlk");
    for (int i = 0; i < 5; ++i) {
        INFO(i);
        const std::string suffix = std::to_string(i);
        REQUIRE(wotlk.count("UNIT_FIELD_STAT" + suffix) == 1);
        REQUIRE(wotlk.count("UNIT_FIELD_POSSTAT" + suffix) == 1);
        REQUIRE(wotlk.count("UNIT_FIELD_NEGSTAT" + suffix) == 1);
        if (i > 0) {
            CHECK(wotlk.at("UNIT_FIELD_STAT" + suffix) ==
                  wotlk.at("UNIT_FIELD_STAT" + std::to_string(i - 1)) + 1);
            CHECK(wotlk.at("UNIT_FIELD_POSSTAT" + suffix) ==
                  wotlk.at("UNIT_FIELD_POSSTAT" + std::to_string(i - 1)) + 1);
            CHECK(wotlk.at("UNIT_FIELD_NEGSTAT" + suffix) ==
                  wotlk.at("UNIT_FIELD_NEGSTAT" + std::to_string(i - 1)) + 1);
        }
    }
    CHECK(wotlk.at("UNIT_FIELD_STAT4") + 1 == wotlk.at("UNIT_FIELD_POSSTAT0"));
    CHECK(wotlk.at("UNIT_FIELD_POSSTAT4") + 1 == wotlk.at("UNIT_FIELD_NEGSTAT0"));
    CHECK(wotlk.at("UNIT_FIELD_NEGSTAT4") + 1 == wotlk.at("UNIT_FIELD_RESISTANCES"));

    // And why the binding spells the three runs out instead of offsetting from
    // UNIT_FIELD_STAT0: the other layouts put the stats somewhere else entirely,
    // so their 89 through 98 belong to unrelated fields. Absent from those
    // tables is what keeps a WotLK-only field from reading field zero there.
    for (const char* expansion : {"classic", "tbc", "turtle"}) {
        const auto fields = declaredFields(expansion);
        INFO(expansion);
        CHECK(fields.count("UNIT_FIELD_POSSTAT0") == 0);
        CHECK(fields.count("UNIT_FIELD_NEGSTAT4") == 0);
    }
}

TEST_CASE("the resistance block is three seven-wide runs in a row",
          "[update-fields]") {
    // 0x004f54d0 answers UnitArmor and UnitResistance from three arrays read at
    // one school index: the figure itself at blob +0x174 - WoWee's RESISTANCES,
    // 99 - then the positive part at +0x190 and the negative part at +0x1ac.
    // With the blob six dwords behind the wire index those are 106 and 113,
    // which is also where the client's field-name table puts them: it lists
    // RESISTANCEBUFFMODSNEGATIVE at 0x009f8bf0, above POSITIVE at 0x009f8c18,
    // above RESISTANCES at 0x009f8c40 - one string per run of seven elements,
    // in descending index order, so the negative run is the 113 end and the
    // positive one the 106 end. Both ends of the whole block are keys this
    // table has always carried: RESISTANCES' 99 below, BASE_MANA's 120 above.
    //
    // Two bases pinned this way rather than fourteen names, and a base one off
    // is the convincing kind of wrong: 105 reads the fire resistance as an
    // armour buff, and the tooltip formula still adds up.
    const auto wotlk = declaredFields("wotlk");
    REQUIRE(wotlk.count("UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE") == 1);
    REQUIRE(wotlk.count("UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE") == 1);
    CHECK(wotlk.at("UNIT_FIELD_RESISTANCES") + 7 ==
          wotlk.at("UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE"));
    CHECK(wotlk.at("UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE") + 7 ==
          wotlk.at("UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE"));
    CHECK(wotlk.at("UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE") + 7 ==
          wotlk.at("UNIT_FIELD_BASE_MANA"));

    // And the two runs are WotLK's alone: the other layouts put the resistance
    // block somewhere else, so the bindings' guards, not an index, are what
    // keep them from reading field zero there.
    for (const char* expansion : {"classic", "tbc", "turtle"}) {
        const auto fields = declaredFields(expansion);
        INFO(expansion);
        CHECK(fields.count("UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE") == 0);
        CHECK(fields.count("UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE") == 0);
    }
}
