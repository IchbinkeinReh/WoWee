// What a unit's model events play (0x00732650, 0x007471a0, 0x00747310,
// 0x007461e0, 0x00adb5d8).
#include <catch_amalgamated.hpp>

#include "rendering/unit_model_events.hpp"

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

    // Footfalls and the sheath events go elsewhere.
    CHECK(ue::classify(ue::fourCC("$FSD")).kind == ue::Kind::None);
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
