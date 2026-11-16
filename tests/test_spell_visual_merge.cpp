// A ranged spell's visual completed from the ranged weapon's (0x00720f80).
#include <catch_amalgamated.hpp>

#include "rendering/spell_visual_merge.hpp"

using namespace wowee::rendering::spell_visual_merge;

namespace {
Row row(std::initializer_list<std::pair<int, int32_t>> cells) {
    Row r{};
    for (const auto& [i, v] : cells) r[static_cast<size_t>(i)] = v;
    return r;
}
}  // namespace

TEST_CASE("a ranged spell with no visual takes its weapon's", "[spell][visual]") {
    // A wand's visual 225: kits 372 and 2970, a missile 188, attachment -1.
    const Row wand = row({{0, 225}, {1, 372}, {2, 2970}, {7, 1}, {8, 188}, {10, 1}, {16, -1}});
    const Row out = fillFromWeapon(nullptr, wand);
    CHECK(out[kPrecastKit] == 372);
    CHECK(out[kCastKit] == 2970);
    CHECK(out[kHasMissile] == 1);
    CHECK(out[kMissileModel] == 188);
    CHECK(out[kMissileDestinationAttachment] == 1);
    CHECK(out[kMissileAttachment] == -1);
    CHECK(out[0] == 0);  // the id is not the weapon's
}

TEST_CASE("a ranged spell keeps what its own visual has", "[spell][visual]") {
    // Arcane Shot's 3299: impact kit 419, its own missile 1602, sound 3013.
    const Row spell = row({{0, 3299}, {3, 419}, {7, 1}, {8, 1602}, {10, 1}, {11, 3013}, {13, 4}, {16, -1}});
    const Row bow = row({{0, 5}, {1, 7}, {2, 164}, {3, 1947}, {10, 1}, {11, 4222}, {13, 9}, {16, -1}, {4, 77}});
    const Row out = fillFromWeapon(&spell, bow);
    CHECK(out[kPrecastKit] == 7);      // empty, from the bow
    CHECK(out[kCastKit] == 164);       // empty, from the bow
    CHECK(out[kImpactKit] == 419);     // the spell's own
    CHECK(out[kMissileModel] == 1602); // the spell's own missile
    CHECK(out[kMissileSound] == 3013);
    CHECK(out[13] == 4);               // Flags stay the spell's
    CHECK(out[4] == 0);                // so does the state kit
}

TEST_CASE("a weapon missile fills only a spell without one", "[spell][visual]") {
    const Row spell = row({{7, 0}, {8, 0}});
    const Row weapon = row({{7, 1}, {8, 3493}});
    CHECK(fillFromWeapon(&spell, weapon)[kMissileModel] == 3493);
    const Row none = row({});
    CHECK(fillFromWeapon(&spell, none)[kHasMissile] == 0);
}
