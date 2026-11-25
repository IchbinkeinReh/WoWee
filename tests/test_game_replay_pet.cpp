// Who "pet" is and when UNIT_PET says so (0x0060abf0, 0x0060bb70, 0x00614190,
// 0x0073fcc0): the player's charm or summon, which the server sends after the
// bar of it (SMSG_PET_SPELLS). See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "game/spell_handler.hpp"

#include <algorithm>
#include <string>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::ObjectType;
using wowee::game::Opcode;
using wowee::game::UF;

namespace {

constexpr uint64_t kVehicle = 0xAB, kImp = 0xCD;

// Player::VehicleSpellInitialize and PetSpellInitialize, with nothing on the bar.
wowee::network::Packet petSpells(uint64_t guid) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_PET_SPELLS));
    p.writeUInt64(guid);
    if (guid == 0) return p;
    p.writeUInt16(0);
    p.writeUInt32(0);
    p.writeUInt8(0);
    p.writeUInt8(0);
    p.writeUInt16(0);
    for (int i = 0; i < 10; ++i) p.writeUInt32(0);
    p.writeUInt8(0);
    p.writeUInt8(0);
    return p;
}

struct Heard {
    std::vector<std::string> events;
    void listen(Session& s) {
        s.handler().setAddonEventCallback([this](const std::string& name, const std::vector<std::string>& args) {
            events.push_back(args.empty() ? name : name + " " + args.front());
        });
    }
    long count(const std::string& what) const { return std::count(events.begin(), events.end(), what); }
};

bool isPet(Session& s, uint64_t guid) {
    const auto tokens = s.handler().unitTokensForGuid(guid);
    return std::find(tokens.begin(), tokens.end(), "pet") != tokens.end();
}

}  // namespace

// The bar of a vehicle came with the unit "pet" of it, and the pet's action bar
// was shown for it until the field the client looks by had come.
TEST_CASE("the pet's bar does not make it the unit pet", "[replay][pet]") {
    Heard heard;
    Session s;
    heard.listen(s);
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    s.receive(UpdateObject().create(kVehicle, ObjectType::UNIT, {{UF::UNIT_FIELD_HEALTH, 100}}));
    s.receive(petSpells(kVehicle));
    CHECK(s.handler().getPetGuid() == kVehicle);
    CHECK(s.handler().petUnitGuid() == 0);
    CHECK_FALSE(isPet(s, kVehicle));

    heard.events.clear();
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_CHARM, static_cast<uint32_t>(kVehicle)}}));
    CHECK(s.handler().petUnitGuid() == kVehicle);
    CHECK(isPet(s, kVehicle));
    CHECK(heard.count("UNIT_PET player") == 1);

    // The same again says nothing.
    heard.events.clear();
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_CHARM, static_cast<uint32_t>(kVehicle)}}));
    CHECK(heard.count("UNIT_PET player") == 0);
}

TEST_CASE("the charm goes before the summon", "[replay][pet]") {
    Heard heard;
    Session s;
    heard.listen(s);
    s.spawnPlayer({{UF::UNIT_FIELD_SUMMON, static_cast<uint32_t>(kImp)}});
    CHECK(s.handler().petUnitGuid() == kImp);
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_CHARM, static_cast<uint32_t>(kVehicle)}}));
    CHECK(s.handler().petUnitGuid() == kVehicle);
    heard.events.clear();
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_CHARM, 0}}));
    CHECK(s.handler().petUnitGuid() == kImp);
    CHECK(heard.count("UNIT_PET player") == 1);
}

// The player's fields may come before the pet does: it is told once more.
TEST_CASE("a pet that comes into sight is told to its owner", "[replay][pet]") {
    Heard heard;
    Session s;
    heard.listen(s);
    s.spawnPlayer({{UF::UNIT_FIELD_SUMMON, static_cast<uint32_t>(kImp)}});
    heard.events.clear();
    s.receive(UpdateObject().create(kImp, ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 100},
                                     {UF::UNIT_FIELD_SUMMONEDBY_LO, static_cast<uint32_t>(Session::kPlayerGuid)}}));
    CHECK(heard.count("UNIT_PET player") == 1);
    heard.events.clear();
    s.receive(UpdateObject().create(0xEF, ObjectType::UNIT, {{UF::UNIT_FIELD_HEALTH, 100}}));
    CHECK(heard.count("UNIT_PET player") == 0);
}

// UNIT_FLAG_IN_COMBAT, whoever is seen swinging (0x0060f860): set while only
// the pet is fought, and with nobody swinging at the player at all.
TEST_CASE("the player is in combat by its flag", "[replay][combat]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    CHECK_FALSE(s.handler().isInCombat());
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_FLAGS, 0x80000u}}));
    CHECK(s.handler().isInCombat());
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_FLAGS, 0x8u}}));
    CHECK_FALSE(s.handler().isInCombat());

    // SMSG_ATTACKSTART (Unit::SendMeleeAttackStart) against it is not it.
    s.receive(UpdateObject().create(kVehicle, ObjectType::UNIT, {{UF::UNIT_FIELD_HEALTH, 100}}));
    wowee::network::Packet swing(wowee::game::wireOpcode(Opcode::SMSG_ATTACKSTART));
    swing.writeUInt64(kVehicle);
    swing.writeUInt64(Session::kPlayerGuid);
    s.receive(swing);
    CHECK_FALSE(s.handler().isInCombat());
}

// T286: 0x005d4210 asks 0x006dd060: the unit seen through and possessed, whoever moves it.
TEST_CASE("a vehicle the player does not move casts its spells itself", "[replay][pet]") {
    constexpr uint32_t kFeast = 61784;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    const uint32_t me = static_cast<uint32_t>(Session::kPlayerGuid);
    s.receive(UpdateObject().create(kVehicle, ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_FLAGS, 0x1008008}}));
    s.handler().spellNameCacheRef()[kFeast].name = "Feast";
    s.handler().spellNameCacheRef()[kFeast].implicitTargetsA[0] = 1;
    s.receive(petSpells(kVehicle));
    // The chair's control, given and taken again before it is seen through.
    const uint32_t action = 0x08000000u | kFeast;
    const auto casts = [&] { return s.sent().of(Opcode::CMSG_PET_CAST_SPELL).size(); };
    const auto actions = [&] { return s.sent().of(Opcode::CMSG_PET_ACTION).size(); };
    CHECK(s.handler().possessedUnit() == 0);
    s.receive(UpdateObject().values(kVehicle, {{UF::UNIT_FIELD_CHARMEDBY, me}}));
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_FARSIGHT, static_cast<uint32_t>(kVehicle)}}));
    CHECK(s.handler().possessedUnit() == kVehicle);
    s.handler().usePetSlot(action, 0);
    CHECK(casts() == 1);
    CHECK(actions() == 0);
    // Another's is not.
    s.receive(UpdateObject().values(kVehicle, {{UF::UNIT_FIELD_CHARMEDBY, 0x77}}));
    CHECK(s.handler().possessedUnit() == 0);
    s.handler().usePetSlot(action, 0);
    CHECK(casts() == 1);
    CHECK(actions() == 1);
}

// T284: 0x0080c790, case 0x59: an aimed shot's place is picked by its trajectory. It waited for
// a unit, which its target flags do not want.
TEST_CASE("a vehicle's aimed shot waits for no target", "[replay][pet]") {
    constexpr uint32_t kHurlBoulder = 62306, kMissile = 1682;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    s.receive(UpdateObject().create(kVehicle, ObjectType::UNIT, {{UF::UNIT_FIELD_HEALTH, 100}}));
    auto& spell = s.handler().spellNameCacheRef()[kHurlBoulder];
    spell.name = "Hurl Boulder";
    spell.targetFlags = 0x40;
    spell.implicitTargetsA[0] = 89;
    spell.spellMissileId = kMissile;
    auto* spells = s.handler().getSpellHandler();
    uint32_t flags = 17;
    spells->setMissileRowSource([&](uint32_t id) -> std::optional<wowee::game::missile_trajectory::MissileRow> {
        if (id != kMissile) return std::nullopt;
        wowee::game::missile_trajectory::MissileRow row;
        row.flags = flags;
        return row;
    });
    spells->castPetSpell(kVehicle, kHurlBoulder, 0);
    CHECK_FALSE(spells->cancelGroundTargeting());
    // One that is not aimed waits for its place.
    flags = 16;
    spells->castPetSpell(kVehicle, kHurlBoulder, 0);
    CHECK(spells->cancelGroundTargeting());
    spells->setMissileRowSource({});
}

// P69: 0x005d4ad0 puts the pet's actions into the bonus bar each time (0x005ab800), and each of
// its twelve slots is said. The buttons have the number they had, and redrew for nothing else:
// after the first vehicle and a map they never came back.
TEST_CASE("a vehicle's actions are said slot by slot each time they come", "[replay][pet]") {
    Heard heard;
    Session s;
    heard.listen(s);
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    s.receive(UpdateObject().create(kVehicle, ObjectType::UNIT, {{UF::UNIT_FIELD_HEALTH, 100}}));
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_FARSIGHT, static_cast<uint32_t>(kVehicle)}}));
    for (int time = 0; time < 2; ++time) {
        heard.events.clear();
        s.receive(petSpells(kVehicle));
        CHECK(s.handler().petActionsOnBonusBar());
        CHECK(s.handler().getBonusActionBarOffset() == 5);
        for (int slot = 121; slot <= 132; ++slot)
            CHECK(heard.count("ACTIONBAR_SLOT_CHANGED " + std::to_string(slot)) == 1);
        // Said before the bar is.
        const auto at = [&](const std::string& what) {
            return std::find(heard.events.begin(), heard.events.end(), what) - heard.events.begin();
        };
        CHECK(at("ACTIONBAR_SLOT_CHANGED 132") < at("PET_BAR_UPDATE"));
        if (time == 0) CHECK(at("ACTIONBAR_SLOT_CHANGED 132") < at("UPDATE_BONUS_ACTIONBAR"));
        // Off it, nothing is put there.
        heard.events.clear();
        s.receive(petSpells(0));
        CHECK_FALSE(s.handler().petActionsOnBonusBar());
        CHECK(heard.count("ACTIONBAR_SLOT_CHANGED 121") == 0);
    }
    // A pet on a bar of its own takes none of the player's.
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_FARSIGHT, 0}}));
    heard.events.clear();
    s.receive(petSpells(kVehicle));
    CHECK(heard.count("ACTIONBAR_SLOT_CHANGED 121") == 0);
}
