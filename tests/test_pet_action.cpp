#include <catch_amalgamated.hpp>

#include "game/pet_action.hpp"

#include <array>
#include <utility>
#include <vector>

using namespace wowee::game::pet;

// The bug these cover: dismiss packed action 0 under the command type, which is
// COMMAND_STAY, so the pet planted itself instead of leaving. The ids overlap
// between the two built-in types, so a test that only checks the low bits would
// have passed on the broken encoding too - each case pins both halves.

TEST_CASE("pet action packs type into the high byte", "[pet]") {
    REQUIRE(packPetAction(ActionType::Command, kAbandon) == 0x07000003u);
    REQUIRE(packPetAction(ActionType::Command, kStay)    == 0x07000000u);
    REQUIRE(packPetAction(ActionType::Command, kFollow)  == 0x07000001u);
    REQUIRE(packPetAction(ActionType::Command, kAttack)  == 0x07000002u);

    REQUIRE(packPetAction(ActionType::Reaction, kPassive)    == 0x06000000u);
    REQUIRE(packPetAction(ActionType::Reaction, kDefensive)  == 0x06000001u);
    REQUIRE(packPetAction(ActionType::Reaction, kAggressive) == 0x06000002u);
}

TEST_CASE("dismiss is not stay", "[pet]") {
    REQUIRE(packPetAction(ActionType::Command, kAbandon) !=
            packPetAction(ActionType::Command, kStay));
}

TEST_CASE("built-in ids collide across types and are told apart by type", "[pet]") {
    const uint32_t follow    = packPetAction(ActionType::Command,  kFollow);
    const uint32_t defensive = packPetAction(ActionType::Reaction, kDefensive);

    // Same low bits - the id alone cannot distinguish them.
    REQUIRE(petActionId(follow) == petActionId(defensive));
    REQUIRE(follow != defensive);
    REQUIRE(petActionType(follow) == ActionType::Command);
    REQUIRE(petActionType(defensive) == ActionType::Reaction);
}

TEST_CASE("round trip through unpack", "[pet]") {
    for (uint32_t id : {kStay, kFollow, kAttack, kAbandon, kMoveTo}) {
        const uint32_t packed = packPetAction(ActionType::Command, id);
        REQUIRE(petActionId(packed) == id);
        REQUIRE(petActionType(packed) == ActionType::Command);
    }
}

TEST_CASE("only castable slots count as spells", "[pet]") {
    // Commands and stances are not spells, whatever their id - the old test for
    // this was "id > 6", which called stay/follow/attack spells the moment the
    // server sent them with their real ids of 0/1/2.
    REQUIRE_FALSE(isPetSpellAction(packPetAction(ActionType::Command, kStay)));
    REQUIRE_FALSE(isPetSpellAction(packPetAction(ActionType::Command, kAttack)));
    REQUIRE_FALSE(isPetSpellAction(packPetAction(ActionType::Reaction, kPassive)));

    REQUIRE(isPetSpellAction(packPetAction(ActionType::Disabled, 2649u)));
    REQUIRE(isPetSpellAction(packPetAction(ActionType::Enabled, 2649u)));
    REQUIRE(isPetSpellAction(packPetAction(ActionType::Passive, 3110u)));
}

TEST_CASE("a spell id larger than 24 bits cannot corrupt the type", "[pet]") {
    const uint32_t packed = packPetAction(ActionType::Enabled, 0xFFFFFFFFu);
    REQUIRE(petActionType(packed) == ActionType::Enabled);
    REQUIRE(petActionId(packed) == 0x00FFFFFFu);
}

TEST_CASE("an action-bar reference to a pet slot reads only its spells (0x005a80e0)", "[pet]") {
    REQUIRE(petSlotSpell(packPetAction(ActionType::Enabled, 2649u)) == 2649u);
    REQUIRE(petSlotSpell(packPetAction(ActionType::Disabled, 2649u)) == 2649u);
    REQUIRE(petSlotSpell(packPetAction(ActionType::Passive, 3110u)) == 3110u);
    // States 8..17 are spells too, 18 is not.
    REQUIRE(petSlotSpell((8u << 24) | 77u) == 77u);
    REQUIRE(petSlotSpell((17u << 24) | 77u) == 77u);
    REQUIRE(petSlotSpell((18u << 24) | 77u) == 0u);
    REQUIRE(petSlotSpell(packPetAction(ActionType::Command, kAttack)) == 0u);
    REQUIRE(petSlotSpell(packPetAction(ActionType::Reaction, kDefensive)) == 0u);
}

namespace {
DismissContext ownPetOnFoot() {
    DismissContext c;
    c.player = 0x11;
    c.pet = 0xF140000000000022ull;
    c.petInView = true;
    c.petOwner = c.player;
    c.mover = c.player;
    return c;
}
}  // namespace

TEST_CASE("PetDismiss sends the pet away as 0x005d46f0 does", "[pet]") {
    DismissContext c = ownPetOnFoot();
    CHECK(dismissAction(c) == DismissAction::PetAction);
    // A summon whose SummonProperties Flags have 0x20 stays.
    c.summonPropertiesFlags = 0x20;
    CHECK(dismissAction(c) == DismissAction::None);
    c = ownPetOnFoot();
    // Stunned, fleeing or confused (0x005d3310): nothing.
    c.petUnitFlags = 0x40000;
    CHECK(dismissAction(c) == DismissAction::None);
    c = ownPetOnFoot();
    // Out of view, or another's: nothing.
    c.petInView = false;
    CHECK(dismissAction(c) == DismissAction::None);
    c = ownPetOnFoot();
    c.petOwner = 0x33;
    CHECK(dismissAction(c) == DismissAction::None);
    c = ownPetOnFoot();
    // The player charmed: nothing.
    c.playerCharmedBy = 0x44;
    CHECK(dismissAction(c) == DismissAction::None);
}

TEST_CASE("PetDismiss from a driven vehicle", "[pet][vehicle]") {
    constexpr uint64_t kVehicle = 0xF150001234000056ull;
    // The driven vehicle is not the pet: the camera looks through the
    // vehicle, so 0x005d3e80 finds no pet to send away.
    DismissContext c = ownPetOnFoot();
    c.mover = kVehicle;
    c.farsight = kVehicle;
    c.playerTransport = kVehicle;
    CHECK(dismissAction(c) == DismissAction::None);
    // Without the farsight, the fall-through sends the pet away.
    c.farsight = 0;
    CHECK(dismissAction(c) == DismissAction::PetAction);
    // The vehicle is the pet: by its seat's Flags 0x2000000.
    c.pet = kVehicle;
    c.farsight = kVehicle;
    c.moverIsVehicle = true;
    c.seatKnown = true;
    CHECK(dismissAction(c) == DismissAction::None);
    c.seatFlags = 0x2000000;
    CHECK(dismissAction(c) == DismissAction::DismissControlledVehicle);
}

TEST_CASE("a pet's spell is cast by the pet where it is the mover or the spell asks", "[pet]") {
    // 0x005d4210: AttributesEx4 0x20, or the pet the active mover - a
    // vehicle the player drives - goes through 0x0080cce0.
    CHECK(petCastsItself(0, true));
    CHECK(petCastsItself(0x20, false));
    CHECK_FALSE(petCastsItself(0x10, false));
}

TEST_CASE("no pet stunned, fleeing or confused casts unless the spell allows it (0x005d4210)", "[pet]") {
    CHECK_FALSE(petsCannotCast(std::array<uint32_t, 1>{0u}, 0));
    CHECK(petsCannotCast(std::array<uint32_t, 1>{0x40000u}, 0));
    CHECK_FALSE(petsCannotCast(std::array<uint32_t, 1>{0x40000u}, 0x8));
    CHECK(petsCannotCast(std::array<uint32_t, 1>{0x800000u}, 0));
    CHECK_FALSE(petsCannotCast(std::array<uint32_t, 1>{0x800000u}, 0x20000));
    CHECK(petsCannotCast(std::array<uint32_t, 1>{0x400000u}, 0));
    CHECK_FALSE(petsCannotCast(std::array<uint32_t, 1>{0x400000u}, 0x40000));
    // Stunned but allowed it, and fleeing: the fleeing bars it.
    CHECK(petsCannotCast(std::array<uint32_t, 1>{0x840000u}, 0x8));
    // Every pet, in the same way: one stunned, one free casts.
    CHECK_FALSE(petsCannotCast(std::array<uint32_t, 2>{0x40000u, 0u}, 0));
    CHECK_FALSE(petsCannotCast(std::array<uint32_t, 2>{0x40000u, 0x800000u}, 0));
    CHECK_FALSE(petsCannotCast(std::array<uint32_t, 0>{}, 0));
}

namespace {
struct TestAura {
    uint32_t spellId = 0;
    uint8_t flags = 0;
};
}  // namespace

TEST_CASE("a pet's spell in effect is taken off by its button (0x00802cb0)", "[pet]") {
    const auto noSlot = [](uint32_t) { return 0u; };
    const auto noMount = [](uint32_t) { return std::pair<uint32_t, uint32_t>{0u, 0u}; };
    ToggleSpell s;
    s.id = 100;
    std::vector<TestAura> auras{{100, 0x10}};
    // Without an ActiveIconID, a companion or a mount, never.
    CHECK_FALSE(spellInEffectOn(s, ToggleUnit{}, noSlot, auras, noMount));
    s.activeIconId = 7;
    CHECK(spellInEffectOn(s, ToggleUnit{}, noSlot, auras, noMount));
    // Only with the positive flag.
    auras[0].flags = 0x8;
    CHECK_FALSE(spellInEffectOn(s, ToggleUnit{}, noSlot, auras, noMount));
    // A companion's: its critter is the one it names.
    ToggleSpell c;
    c.id = 200;
    c.effects[0] = kEffectSummon;
    c.misc[0] = 4242;
    c.miscB0 = 41;
    const auto slot5 = [](uint32_t row) { return row == 41 ? 5u : 0u; };
    ToggleUnit u;
    CHECK_FALSE(spellInEffectOn(c, u, slot5, std::vector<TestAura>{}, noMount));
    u.critterEntry = 4242;
    CHECK(spellInEffectOn(c, u, slot5, std::vector<TestAura>{}, noMount));
    // A summon naming the vehicle it rides, made by the player.
    ToggleSpell v;
    v.id = 300;
    v.activeIconId = 1;
    v.effects[1] = kEffectSummon;
    v.misc[1] = 555;
    ToggleUnit rider;
    rider.vehicleEntry = 555;
    CHECK_FALSE(spellInEffectOn(v, rider, noSlot, std::vector<TestAura>{}, noMount));
    rider.vehicleMadeByPlayer = true;
    CHECK(spellInEffectOn(v, rider, noSlot, std::vector<TestAura>{}, noMount));
    // A mount: another aura mounting the same creature.
    ToggleSpell m;
    m.id = 400;
    m.auras[0] = kAuraMounted;
    m.misc[0] = 99;
    const auto mountOf = [](uint32_t spell) {
        return spell == 401 ? std::pair<uint32_t, uint32_t>{kAuraMounted, 99u} : std::pair<uint32_t, uint32_t>{0u, 0u};
    };
    CHECK(spellInEffectOn(m, ToggleUnit{}, noSlot, std::vector<TestAura>{{401, 0}}, mountOf));
    CHECK_FALSE(spellInEffectOn(m, ToggleUnit{}, noSlot, std::vector<TestAura>{{402, 0}}, mountOf));
}

TEST_CASE("only an aura's spell is taken off a pet (0x007fdf20)", "[pet]") {
    CHECK(spellAppliesAura({0, 6, 0}));
    CHECK(spellAppliesAura({143, 0, 0}));
    CHECK_FALSE(spellAppliesAura({28, 0, 0}));
}

TEST_CASE("AttributesEx6 0x40000 is cast by the charmer, and Ex7 0x8 wants its flag (0x0080cce0)", "[pet]") {
    CHECK(castingUnit(1, 0, 2, 3) == 1);
    CHECK(castingUnit(1, 0x40000, 2, 3) == 2);
    CHECK(castingUnit(1, 0x40000, 0, 3) == 3);
    CHECK(casterMayCast(0, 0));
    CHECK_FALSE(casterMayCast(0x8, 0));
    CHECK(casterMayCast(0x8, 0x40000));
}
