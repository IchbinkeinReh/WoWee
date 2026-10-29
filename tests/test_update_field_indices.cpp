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
