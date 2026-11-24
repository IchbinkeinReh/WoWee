// Threat lists through GameHandler's own dispatch. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "core/coordinates.hpp"
#include "game_replay.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::Opcode;
using wowee::game::UF;

namespace {

constexpr uint64_t kGuard = 0xF130004221000099ull;

// SMSG_THREAT_UPDATE: the unit, how many are on its list, and each with its
// threat.
wowee::network::Packet threatUpdate(uint64_t victim) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_THREAT_UPDATE));
    p.writePackedGuid(kGuard);
    p.writeUInt32(1);
    p.writePackedGuid(victim);
    p.writeUInt32(1000);
    return p;
}

}  // namespace

TEST_CASE("a threat list is told of by the tokens its unit has", "[replay][threat]") {
    std::vector<std::pair<std::string, std::string>> events;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}});
    s.receive(UpdateObject().create(kGuard, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}}));
    s.handler().setAddonEventCallback([&](const std::string& name, const std::vector<std::string>& args) {
        if (name.rfind("UNIT_THREAT_", 0) == 0) events.emplace_back(name, args.empty() ? std::string() : args[0]);
    });
    // A guard at somebody else, nothing of the player's: no token, no event.
    s.receive(threatUpdate(0xF130004221000077ull));
    CHECK(events.empty());

    s.handler().setTarget(kGuard);
    events.clear();
    s.receive(threatUpdate(0xF130004221000077ull));
    const auto has = [&](const char* name, const char* unit) {
        return std::count(events.begin(), events.end(), std::pair<std::string, std::string>(name, unit)) == 1;
    };
    CHECK(has("UNIT_THREAT_LIST_UPDATE", "target"));
    for (const auto& [name, unit] : events) CHECK_FALSE(unit.empty());
    s.handler().setAddonEventCallback(nullptr);
}

// The target keys, as the playtest pressed them at the Terrace: what is behind
// is not picked, however near by the old forty yards, and a friend is whoever
// may be helped and lives.
TEST_CASE("the target key looks ahead", "[replay][targeting]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}});
    auto& gh = s.handler();
    const auto unit = [&](uint64_t guid, float serverX, float serverY) {
        s.receive(UpdateObject().create(guid, wowee::game::ObjectType::UNIT,
                                        {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}}));
        const auto e = gh.getEntityManager().getEntity(guid);
        REQUIRE(e);
        e->setPosition(serverY, serverX, 0.0f, 0.0f);   // canonical
    };
    const auto me = gh.getEntityManager().getEntity(Session::kPlayerGuid);
    REQUIRE(me);
    me->setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    gh.setOrientation(wowee::core::coords::serverToCanonicalYaw(0.0f));   // along the wire's x
    unit(0xF130000000000001ull, -15.0f, 0.0f);   // behind, the nearest
    unit(0xF130000000000002ull, 35.0f, 3.0f);    // ahead
    unit(0xF130000000000003ull, 20.0f, -2.0f);   // ahead, nearer
    unit(0xF130000000000004ull, 0.0f, 9.0f);     // beside, within ten
    unit(0xF130000000000005ull, 60.0f, 0.0f);    // too far

    std::vector<uint64_t> order;
    for (int i = 0; i < 3; ++i) {
        gh.targetNearest(false);
        order.push_back(gh.getTargetGuid());
    }
    CHECK(order == std::vector<uint64_t>{0xF130000000000003ull, 0xF130000000000002ull, 0xF130000000000004ull});

    // One that is not drawn is not picked.
    gh.clearTarget();
    gh.setUnitDrawnCallback([](uint64_t guid) { return guid != 0xF130000000000003ull; });
    gh.targetNearest(false);
    CHECK(gh.getTargetGuid() == 0xF130000000000002ull);
}
