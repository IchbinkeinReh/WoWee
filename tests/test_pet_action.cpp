#include <catch_amalgamated.hpp>

#include "game/pet_action.hpp"

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
