// What a unit's model events play (0x00732650, 0x007471a0, 0x00747310,
// 0x007461e0, 0x00adb5d8).
#include <catch_amalgamated.hpp>

#include "rendering/unit_model_events.hpp"
#include "rendering/unit_splash.hpp"

namespace ue = wowee::rendering::unit_event;

TEST_CASE("unit events by their ids", "[unit_model_events]") {
    // The words the client compares: "$FD1" is 0x31444624.
    CHECK(ue::fourCC("$FD1") == 0x31444624u);
    CHECK(ue::fourCC("$WNG") == 0x474e5724u);
    CHECK(ue::fourCC("$FDX") == 0x58444624u);

    for (uint32_t i = 0; i < 5; ++i) {
        const char name[5] = {'$', 'F', 'D', static_cast<char>('1' + i), 0};
        const auto a = ue::classify(ue::fourCC(name));
        CHECK(a.kind == ue::Kind::Fidget);
        CHECK(a.index == i);
    }
    // $FD6..$FD9 only ask for the sound data and play nothing.
    CHECK(ue::classify(ue::fourCC("$FD6")).kind == ue::Kind::None);
    CHECK(ue::classify(ue::fourCC("$FD9")).kind == ue::Kind::None);

    CHECK(ue::classify(0x474e5724u).kind == ue::Kind::Slot);
    CHECK(ue::classify(0x474e5724u).index == ue::kSlotWingFlap);
    CHECK(ue::classify(0x47475724u).index == ue::kSlotWingGlide);
    CHECK(ue::classify(0x58444624u).index == ue::kSlotStand);

    CHECK(ue::classify(0x54524224u).kind == ue::Kind::AtEvent);  // $BRT
    CHECK(ue::classify(0x54524224u).index == ue::kColumnBirth);
    CHECK(ue::classify(0x44435324u).index == ue::kColumnSpellCastDirected);  // $SCD
    CHECK(ue::classify(0x474d5324u).index == ue::kColumnSubmerge);  // $SMG
    CHECK(ue::classify(0x444d5324u).index == ue::kColumnSubmerged);  // $SMD

    // A footfall (0x00732650: 0x44534624 -> 0x00747240); the sheath events
    // go elsewhere.
    CHECK(ue::classify(0x44534624u).kind == ue::Kind::Footstep);
    CHECK(ue::classify(ue::fourCC("$FSD")).index == ue::kColumnFootstep);
    CHECK(ue::classify(ue::fourCC("$SHL")).kind == ue::Kind::None);
}

TEST_CASE("a slot's CreatureSoundData column", "[unit_model_events]") {
    // 0x007461e0's offsets over four.
    CHECK(ue::slotColumn(ue::kSlotStand) == 8);
    CHECK(ue::slotColumn(ue::kSlotWingFlap) == 11);
    CHECK(ue::slotColumn(ue::kSlotWingGlide) == 12);
    CHECK(ue::slotColumn(2) == 3);
    CHECK(ue::slotColumn(13) == 3);
    CHECK(ue::slotColumn(6) == 0);
}

TEST_CASE("a slot's chance out of 101 draws", "[unit_model_events]") {
    // The draw times 101, its high word: 0..100, played up to the chance.
    CHECK(ue::slotPlays(ue::kSlotWingFlap, 0xFFFFFFFFu));
    CHECK(ue::slotPlays(ue::kSlotStand, 0));
    // 40: a roll of 40 plays, 41 does not.
    const auto drawFor = [](uint32_t roll) {
        return static_cast<uint32_t>(((static_cast<uint64_t>(roll) << 32) + 100) / 101);
    };
    CHECK(ue::slotPlays(ue::kSlotStand, drawFor(40)));
    CHECK_FALSE(ue::slotPlays(ue::kSlotStand, drawFor(41)));
    CHECK(ue::slotChance(ue::kSlotStand) == 40);
    CHECK(ue::slotChance(0) == 70);
    CHECK(ue::slotChance(2) == 60);
}

TEST_CASE("the Stand sound at most every ten seconds", "[unit_model_events]") {
    CHECK(ue::standSoundDue(10000, 0));
    CHECK_FALSE(ue::standSoundDue(19999, 10000));
    CHECK(ue::standSoundDue(20000, 10000));
    // Across the clock's wrap.
    CHECK_FALSE(ue::standSoundDue(5000, 0xFFFFF000u));
    CHECK(ue::standSoundDue(6000, 0xFFFFF000u));
}

TEST_CASE("footfall sounds by footstep id and terrain type", "[unit_model_events]") {
    // TerrainType rows as the 3.3.5a table has them: Dirt 0 -> 1,
    // Metallic 1 -> 2, Stone 2 -> 3, None 10 -> 0.
    ue::FootstepTerrainSounds t;
    CHECK(t.empty());
    t.addTerrainType(0, 1);
    t.addTerrainType(1, 2);
    t.addTerrainType(2, 3);
    t.addTerrainType(10, 0);
    // FootstepTerrainLookup: footstep 8 on sound 1 (dirt) and 3 (stone).
    t.addLookup(8, 1, 650, 1055);
    t.addLookup(8, 3, 653, 1057);
    t.addLookup(8, -1, 999, 999);  // left out (0x004cf990)
    CHECK_FALSE(t.empty());

    CHECK(t.find(8, 2, false) == 653);
    CHECK(t.find(8, 2, true) == 1057);  // the splash in water
    CHECK(t.find(8, 0, false) == 650);
    CHECK(t.find(8, 1, false) == 0);    // no row for metallic
    CHECK(t.find(8, 42, false) == 0);   // no such terrain type
    CHECK(t.find(9, 2, false) == 0);    // no such footstep id

    // 0x004cf170: none for the ground, then terrain type 0's.
    CHECK(t.sound(8, 2, false) == 653);
    CHECK(t.sound(8, 1, false) == 650);
    CHECK(t.sound(8, -1, false) == 650);
    CHECK(t.sound(8, -1, true) == 1055);
    CHECK(t.sound(9, 2, false) == 0);
}

TEST_CASE("the ground a unit stands on", "[unit_model_events]") {
    // A building's floor over the terrain: the floor's material's type.
    CHECK(ue::groundTerrainType(5.0f, 4, 2.0f, 0) == 4);
    CHECK(ue::groundTerrainType(5.0f, -1, std::nullopt, std::nullopt) == -1);
    // The terrain above a floor (a cellar below ground) - the terrain was
    // not met first only if it is above the probe, which the caller drops.
    CHECK(ue::groundTerrainType(1.0f, 4, 2.0f, 7) == 7);
    CHECK(ue::groundTerrainType(std::nullopt, -1, 2.0f, 7) == 7);
    // Under the terrain's surface with no building's floor beneath - on a
    // doodad in a cave mouth, say: the quad's type all the same, read by x
    // and y (0x007a0530).
    CHECK(ue::groundTerrainType(std::nullopt, -1, std::nullopt, 7) == 7);
    // A hole, or a layer with no effect row: none.
    CHECK(ue::groundTerrainType(std::nullopt, -1, 2.0f, std::nullopt) == -1);
}

TEST_CASE("a footfall in water, and a hovering unit", "[unit_model_events]") {
    CHECK(ue::footInWater(10.0f, 9.5f, 0));
    CHECK(ue::footInWater(10.0f, 10.005f, 0));    // within 0.01 over it
    CHECK_FALSE(ue::footInWater(10.0f, 10.02f, 0));
    CHECK_FALSE(ue::footInWater(std::nullopt, 9.5f, 0));
    CHECK_FALSE(ue::footInWater(10.0f, 9.5f, ue::kMoveFlagSwimming));

    // UNIT_FIELD_BYTES_1's third byte, flag 0x2 (0x00747240 reads +0x112).
    CHECK(ue::hovers(0x00020000u));
    CHECK_FALSE(ue::hovers(0x00010000u));
    CHECK_FALSE(ue::hovers(0x00000002u));
}

TEST_CASE("the footfalls 0x00747240 silences before looking for a sound", "[unit_model_events]") {
    // A unit walking on the ground: heard.
    CHECK_FALSE(ue::footfallSilent(0, 0x1, 0, true, 0));
    // Hovering, by its bytes or by its movement.
    CHECK(ue::footfallSilent(0x00020000u, 0x1, 0, false, 0));
    CHECK(ue::footfallSilent(0, ue::kMoveFlagHover | 0x1, 0, false, 0));
    // Using a vehicle: in its seat (3), or on the way in or out of it.
    CHECK(ue::footfallSilent(0, 0x1, 3, true, 0));
    CHECK(ue::footfallSilent(0, 0x1, 1, false, 0));
    // A player's ghost - and the same flag on a creature means nothing.
    CHECK(ue::footfallSilent(0, 0x1, 0, true, ue::kPlayerFlagGhost));
    CHECK_FALSE(ue::footfallSilent(0, 0x1, 0, false, ue::kPlayerFlagGhost));
    // Other player flags do not.
    CHECK_FALSE(ue::footfallSilent(0, 0x1, 0, true, 0x8u));
}

TEST_CASE("a unit splashes crossing four tenths of its height in water", "[unit_splash]") {
    namespace us = wowee::rendering::unit_splash;
    // A human: HumanMale's 2.031 at scale 1 - the mark at 0.81 deep.
    const float h = us::collisionHeight(2.031f, 1.0f, 1.0f, 1.0f);
    CHECK(h == Catch::Approx(2.031f));
    CHECK(us::crossed(h, 0.9f, 0.7f));     // wading in
    CHECK(us::crossed(h, 0.7f, 0.9f));     // and out
    CHECK_FALSE(us::crossed(h, 0.5f, 0.2f));
    CHECK_FALSE(us::crossed(h, 1.4f, 0.9f));
    // A scale below 1 counts as 1; a display scale of nothing as 1.
    CHECK(us::collisionHeight(2.0f, 0.5f, 0.0f, 1.0f) == Catch::Approx(2.0f));
    CHECK(us::collisionHeight(1.653f, 1.0f, 1.35f, 1.0f) == Catch::Approx(1.653f * 1.35f));
    // A creature not sized by its model is 73/36 tall.
    CHECK(us::kDefaultCollisionHeight == Catch::Approx(2.0277778f));
}
