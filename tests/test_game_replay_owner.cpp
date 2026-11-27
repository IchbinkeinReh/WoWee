// Whose a thing of the world is, as the MCP server's rules ask it
// (mcp/owner_guid.hpp), of objects as the realm creates them. See
// game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "mcp/entity_kind.hpp"
#include "mcp/owner_guid.hpp"
#include "mcp/send_guard.hpp"

using replay::Session;
using replay::UpdateObject;
using wowee::game::ObjectType;
using wowee::game::UF;
using wowee::mcp::ownerGuid;

namespace {

constexpr uint64_t kOther = 0x0000000000000777ull;
constexpr uint64_t kHerb = 0xF110000651000088ull;

uint64_t ownerOf(Session& s, uint64_t guid) {
    const auto entity = s.handler().getEntityManager().getEntity(guid);
    REQUIRE(entity);
    return ownerGuid(*entity);
}

}  // namespace

// A herb's faction, behind its dynamic flags, is where a creature has who
// summoned it: gathering was refused as a cast at another player's.
TEST_CASE("what grows belongs to nobody", "[replay][mcp]") {
    Session s;
    s.receive(UpdateObject().create(kHerb, ObjectType::GAMEOBJECT,
                                    {{UF::GAMEOBJECT_DYNAMIC, 0x00000000u}, {UF::GAMEOBJECT_FACTION, 94u}}));
    CHECK(ownerOf(s, kHerb) == 0);
    // Its wire place as a creature's would be a player's guid.
    const auto entity = s.handler().getEntityManager().getEntity(kHerb);
    CHECK(wowee::mcp::isPlayerGuid(wowee::game::unitSummonedByGuid(*entity)));
}

TEST_CASE("what a player made, summoned or left is theirs", "[replay][mcp]") {
    Session s;
    const uint32_t lo = static_cast<uint32_t>(kOther), hi = static_cast<uint32_t>(kOther >> 32);
    constexpr uint64_t kPortal = 0xF110000651000099ull, kPet = 0xF140000C9B00D6E3ull, kBones = 0xF101000000000011ull,
                       kArea = 0xF100000000000012ull;
    s.receive(UpdateObject().create(kPortal, ObjectType::GAMEOBJECT, {{UF::GAMEOBJECT_FIELD_CREATED_BY, lo}}));
    CHECK(ownerOf(s, kPortal) == kOther);
    s.receive(UpdateObject().create(kPet, ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_SUMMONEDBY_LO, lo}, {UF::UNIT_FIELD_SUMMONEDBY_HI, hi}}));
    CHECK(ownerOf(s, kPet) == kOther);
    s.receive(UpdateObject().create(kArea, ObjectType::DYNAMICOBJECT, {{UF::DYNAMICOBJECT_CASTER, lo}}));
    CHECK(ownerOf(s, kArea) == kOther);
    // A corpse's is the field after OBJECT's own six, as a game object's maker.
    s.receive(UpdateObject().create(kBones, ObjectType::CORPSE, {{UF::GAMEOBJECT_FIELD_CREATED_BY, lo}}));
    CHECK(ownerOf(s, kBones) == kOther);
}

// The entities tool's kind "players" (mcp/entity_kind.hpp): a battleground's
// bots are creatures the realm plays as players; the scoreboard lists them
// and they were left out (tester's wish, session 23).
TEST_CASE("the realm's bots are players to the entities tool", "[replay][mcp]") {
    Session s;
    constexpr uint64_t kBot = 0xF130000000000101ull, kMob = 0xF130000000000102ull, kDressed = 0xF130000000000103ull,
                       kImage = 0xF130000000000104ull, kMan = 0x0000000000000778ull;
    const uint32_t lo = static_cast<uint32_t>(kOther), hi = static_cast<uint32_t>(kOther >> 32);
    s.receive(UpdateObject().create(kBot, ObjectType::UNIT, {{UF::UNIT_FIELD_HEALTH, 100u}}));
    s.receive(UpdateObject().create(kMob, ObjectType::UNIT, {{UF::UNIT_FIELD_HEALTH, 100u}}));
    s.receive(UpdateObject().create(kDressed, ObjectType::UNIT, {{UF::UNIT_FIELD_FLAGS_2, 0x10u}}));
    s.receive(UpdateObject().create(kImage, ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_FLAGS_2, 0x10u},
                                     {UF::UNIT_FIELD_SUMMONEDBY_LO, lo},
                                     {UF::UNIT_FIELD_SUMMONEDBY_HI, hi}}));
    s.receive(UpdateObject().create(kMan, ObjectType::PLAYER, {{UF::UNIT_FIELD_HEALTH, 100u}}));
    const auto scored = [&s] {
        std::vector<uint64_t> out;
        if (const auto* board = s.handler().getBgScoreboard()) {
            for (const auto& row : board->players) out.push_back(row.guid);
        }
        return out;
    };
    const auto is = [&](uint64_t guid, const char* kind) {
        const auto entity = s.handler().getEntityManager().getEntity(guid);
        REQUIRE(entity);
        return wowee::mcp::kindWanted(*entity, kind, scored());
    };
    CHECK(is(kMan, "players"));
    CHECK_FALSE(is(kMan, "units"));
    CHECK_FALSE(is(kBot, "players"));
    CHECK(is(kDressed, "players"));   // drawn as a player, nobody's
    CHECK_FALSE(is(kImage, "players"));   // a mage's mirror image
    // The scoreboard names the bot.
    wowee::network::Packet log(wowee::game::wireOpcode(wowee::game::Opcode::MSG_PVP_LOG_DATA));
    log.writeUInt8(0);
    log.writeUInt8(0);
    log.writeUInt32(1);
    log.writeUInt64(kBot);
    for (int k = 0; k < 6; ++k) log.writeUInt32(1);
    log.writeUInt32(0);
    s.receive(log);
    CHECK(is(kBot, "players"));
    CHECK(is(kBot, "units"));
    CHECK(is(kBot, "all"));
    CHECK_FALSE(is(kMob, "players"));
    CHECK(is(kMob, "units"));
    CHECK_FALSE(is(kBot, "objects"));
}
