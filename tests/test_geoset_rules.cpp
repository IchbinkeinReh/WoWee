// Which of a character model's geosets are drawn: the client's character
// component (0x004dfda0, 0x004ee460, 0x004ef0d0, 0x004ed900).
#include <catch_amalgamated.hpp>
#include "core/geoset_rules.hpp"

#include <algorithm>

using namespace wowee::core;

namespace {
bool has(const std::unordered_set<uint16_t>& s, uint16_t id) { return s.count(id) != 0; }
size_t inGroup(const std::unordered_set<uint16_t>& s, uint16_t group) {
    return static_cast<size_t>(std::count_if(s.begin(), s.end(),
                                             [&](uint16_t id) { return id / 100 == group && id != 0; }));
}
ItemGeosets item(uint32_t g0, uint32_t g1 = 0, uint32_t g2 = 0) {
    ItemGeosets i;
    i.worn = true;
    i.group = {g0, g1, g2};
    return i;
}
}  // namespace

TEST_CASE("the appearance key packs three bytes and nothing else", "[geoset]") {
    CHECK(appearanceKey(1, 0, 3) == 0x010003u);
    CHECK(appearanceKey(10, 1, 255) == 0x0A01FFu);
}

TEST_CASE("the component starts at the bald scalp and variant 1, ears 702", "[geoset]") {
    const auto d = kInitialCharacterGeosets;
    CHECK(d[0] == 1);
    CHECK(d[4] == 401);
    CHECK(d[7] == 702);
    CHECK(d[9] == 901);
    CHECK(d[18] == 1801);
}

TEST_CASE("the hair and facial rows set groups 0, 1, 3, 2, 16 and 17", "[geoset]") {
    FacialGeosetColumns f;
    f.column = {2, 3, 4, 0, 2};
    const auto d = characterGeosetDefaults(5, &f);
    CHECK(d[0] == 5);
    CHECK(d[1] == 102);
    CHECK(d[3] == 303);
    CHECK(d[2] == 204);
    CHECK(d[16] == 1600);
    CHECK(d[17] == 1702);
    // A hair row naming no geoset, or none at all: the bald scalp. No facial
    // row leaves the facial groups as they start.
    const auto bare = characterGeosetDefaults(0, nullptr);
    CHECK(bare[0] == 1);
    CHECK(bare[1] == 101);
    CHECK(bare[17] == 1701);
}

TEST_CASE("a helmet's masks hide by race bit", "[geoset]") {
    auto d = characterGeosetDefaults(5, nullptr);
    std::array<uint32_t, 7> masks{};
    masks[0] = 1u << 1;  // humans' hair
    masks[4] = 1u << 4;  // night elves' ears
    applyHelmetGeosetVis(d, masks, 1);
    CHECK(d[0] == 1);
    CHECK(d[7] == 702);
    applyHelmetGeosetVis(d, masks, 4);
    CHECK(d[7] == 701);
}

TEST_CASE("bare, a character shows the body and the nineteen defaults", "[geoset]") {
    const auto shown = characterGeosets(kInitialCharacterGeosets, false, {});
    CHECK(has(shown, 0));
    CHECK(has(shown, 401));
    CHECK(has(shown, 501));
    CHECK(has(shown, 901));
    CHECK(has(shown, 1301));
    CHECK(has(shown, 1501));
    CHECK(shown.size() == 20);
    // The death knight's eyes replace group 17's default.
    const auto dk = characterGeosets(kInitialCharacterGeosets, true, {});
    CHECK(has(dk, 1703));
    CHECK_FALSE(has(dk, 1701));
}

TEST_CASE("gloves take the forearms; without them the chest names the sleeves", "[geoset]") {
    CharacterEquipmentGeosets eq;
    eq.chest = item(2);
    auto shown = characterGeosets(kInitialCharacterGeosets, false, eq);
    CHECK(has(shown, 803));
    CHECK(has(shown, 401));
    eq.gloves = item(3);
    shown = characterGeosets(kInitialCharacterGeosets, false, eq);
    CHECK(has(shown, 404));
    CHECK_FALSE(has(shown, 401));
    CHECK_FALSE(has(shown, 803));
}

TEST_CASE("the shirt's sleeves only where the chest leaves the arms bare", "[geoset]") {
    CharacterEquipmentGeosets eq;
    eq.shirt = item(1, 2);
    auto shown = characterGeosets(kInitialCharacterGeosets, false, eq);
    CHECK(has(shown, 802));
    CHECK(has(shown, 1003));
    eq.chest = item(0);
    eq.chest.texturesArmsOrBody = true;
    shown = characterGeosets(kInitialCharacterGeosets, false, eq);
    CHECK_FALSE(has(shown, 802));
}

TEST_CASE("a robe's skirt replaces everything below the waist", "[geoset]") {
    CharacterEquipmentGeosets eq;
    eq.chest = item(0, 0, 2);
    eq.boots = item(3);
    eq.legs = item(2, 1);
    eq.tabard = item(1);
    const auto shown = characterGeosets(kInitialCharacterGeosets, false, eq);
    CHECK(has(shown, 1303));
    CHECK(inGroup(shown, 13) == 1);
    CHECK(inGroup(shown, 5) == 0);
    CHECK(has(shown, 901));     // 901 is not in the hidden 902-999
    CHECK_FALSE(has(shown, 504));
    CHECK_FALSE(has(shown, 1202));  // no tabard under a robe
    CHECK(inGroup(shown, 11) == 0);
}

TEST_CASE("boots, kneepads, trousers and the tabard", "[geoset]") {
    CharacterEquipmentGeosets eq;
    eq.boots = item(2);
    eq.legs = item(3, 2);
    eq.tabard = item(1);
    auto shown = characterGeosets(kInitialCharacterGeosets, false, eq);
    CHECK(has(shown, 503));
    CHECK_FALSE(has(shown, 501));
    CHECK(has(shown, 901));
    CHECK(has(shown, 1202));
    // Trousers from 3 up hide group 13 and show group 11.
    CHECK(has(shown, 1104));
    CHECK(inGroup(shown, 13) == 0);
    // Without boots the legs' second column names the kneepads.
    eq.boots = {};
    shown = characterGeosets(kInitialCharacterGeosets, false, eq);
    CHECK(has(shown, 903));
    // Short trousers under a tabard are left off.
    eq.legs = item(1);
    shown = characterGeosets(kInitialCharacterGeosets, false, eq);
    CHECK_FALSE(has(shown, 1102));
    eq.tabard = {};
    shown = characterGeosets(kInitialCharacterGeosets, false, eq);
    CHECK(has(shown, 1102));
    CHECK(has(shown, 1301));
}

TEST_CASE("a chest with display flag 4 keeps the legs' columns out", "[geoset]") {
    CharacterEquipmentGeosets eq;
    eq.legs = item(4, 0, 2);
    CharacterGeosetFlags flags;
    flags.chestOwnsLegs = true;
    const auto shown = characterGeosets(kInitialCharacterGeosets, false, eq, flags);
    CHECK_FALSE(has(shown, 1303));
    CHECK_FALSE(has(shown, 1105));
}

TEST_CASE("the cloak and the belt replace their groups' defaults", "[geoset]") {
    CharacterEquipmentGeosets eq;
    eq.cape = item(2);
    eq.belt = item(1);
    const auto shown = characterGeosets(kInitialCharacterGeosets, false, eq);
    CHECK(has(shown, 1503));
    CHECK_FALSE(has(shown, 1501));
    CHECK(has(shown, 1802));
    CHECK_FALSE(has(shown, 1801));
    // A cloak naming no geoset leaves the bare back.
    CharacterEquipmentGeosets plain;
    plain.cape = item(0);
    CHECK(has(characterGeosets(kInitialCharacterGeosets, false, plain), 1501));
}

TEST_CASE("a model draws what is shown, and everything above 2000", "[geoset]") {
    const std::unordered_set<uint16_t> shown{0, 5, 401};
    const auto drawn = modelGeosetsShown(shown, {0, 1, 5, 6, 401, 402, 2001, 2002});
    CHECK(drawn == std::unordered_set<uint16_t>{0, 5, 401, 2001, 2002});
}
