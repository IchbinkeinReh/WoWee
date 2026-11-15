// CharSections: the head's layers as the client composites them.
//
// Beside the face, the client paints the facial hair row's two textures and
// the hair row's scalp (its second and third) onto the head's two regions,
// in that order: face, facial hair, scalp (0x004ea1f0, 0x004e90e0,
// 0x004e8ff0). The reader dropped both.
#include <catch_amalgamated.hpp>

#include "pipeline/char_sections.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"

#include <string>
#include <vector>

using namespace wowee::pipeline;

namespace {
std::vector<uint8_t> charSectionsDbc(const std::vector<std::vector<std::string>>& rows) {
    // ID, Race, Sex, BaseSection, Variation, Colour, Tex1, Tex2, Tex3, Flags.
    std::string strings(1, '\0');
    std::vector<std::vector<uint32_t>> records;
    for (const auto& row : rows) {
        std::vector<uint32_t> rec;
        for (size_t i = 0; i < 10; ++i) {
            if (i >= 6 && i <= 8) {
                if (row[i].empty()) {
                    rec.push_back(0);
                } else {
                    rec.push_back(static_cast<uint32_t>(strings.size()));
                    strings += row[i];
                    strings.push_back('\0');
                }
            } else {
                rec.push_back(static_cast<uint32_t>(std::stoul(row[i])));
            }
        }
        records.push_back(rec);
    }
    std::vector<uint8_t> data = {'W', 'D', 'B', 'C'};
    auto u32 = [&](uint32_t v) {
        for (int b = 0; b < 4; ++b) data.push_back(static_cast<uint8_t>(v >> (8 * b)));
    };
    u32(static_cast<uint32_t>(records.size()));
    u32(10);
    u32(40);
    u32(static_cast<uint32_t>(strings.size()));
    for (const auto& rec : records)
        for (uint32_t v : rec) u32(v);
    data.insert(data.end(), strings.begin(), strings.end());
    return data;
}
}  // namespace

TEST_CASE("the head's layers: face, facial hair, then scalp", "[char_sections]") {
    DBCFile dbc;
    REQUIRE(dbc.load(charSectionsDbc({
        {"1", "3", "0", "0", "0", "1", "Skin01.blp", "", "", "0"},
        {"2", "3", "0", "1", "4", "1", "FaceLower04_01.blp", "FaceUpper04_01.blp", "", "0"},
        {"3", "3", "0", "2", "5", "2", "FacialLowerHair05_02.blp", "FacialUpperHair05_02.blp", "", "0"},
        {"4", "3", "0", "3", "6", "2", "Hair06_02.blp", "ScalpLowerHair06_02.blp", "ScalpUpperHair06_02.blp", "0"},
    })));
    CharSectionsFields f;
    CharacterAppearance who{.raceId = 3, .sexId = 0, .skinId = 1, .faceId = 4, .hairStyleId = 6, .hairColorId = 2,
                            .facialHairId = 5};
    const CharacterSectionTextures t = resolveCharacterSections(&dbc, f, who);
    CHECK(t.hair == "Hair06_02.blp");
    CHECK(t.scalpLower == "ScalpLowerHair06_02.blp");
    CHECK(t.scalpUpper == "ScalpUpperHair06_02.blp");
    CHECK(t.facialLower == "FacialLowerHair05_02.blp");
    CHECK(t.facialUpper == "FacialUpperHair05_02.blp");

    const auto layers = faceRegionLayers(t);
    REQUIRE(layers.size() == 6);
    const std::vector<std::pair<std::string, bool>> want = {
        {"FaceLower04_01.blp", true},       {"FaceUpper04_01.blp", false},
        {"FacialLowerHair05_02.blp", true}, {"FacialUpperHair05_02.blp", false},
        {"ScalpLowerHair06_02.blp", true},  {"ScalpUpperHair06_02.blp", false}};
    for (size_t i = 0; i < want.size(); ++i) {
        CHECK(layers[i].path == want[i].first);
        CHECK(layers[i].lower == want[i].second);
    }

    // The facial hair row is the style's in the HAIR colour; a caller
    // without the style gets none.
    who.facialHairId = -1;
    CHECK(resolveCharacterSections(&dbc, f, who).facialLower.empty());
    who.facialHairId = 5;
    who.hairColorId = 1;
    CHECK(resolveCharacterSections(&dbc, f, who).facialLower.empty());
}

TEST_CASE("head layers the data does not carry are dropped, as the client skips them", "[char_sections]") {
    // 3.3.5a's own rows: a Blood Elf male's facial hair and scalp name
    // FacialLowerHair00_00.blp and ScalpLowerHair00_00.blp, which no archive
    // has. The client composites a head layer only once its image loaded
    // (0x004f08a0, 0x004f2d80); here the name never reaches the compositor.
    DBCFile dbc;
    REQUIRE(dbc.load(charSectionsDbc({
        {"1", "10", "0", "0", "0", "0", "BloodElfMaleSkin00_00.blp", "", "", "0"},
        {"2", "10", "0", "1", "0", "0", "BloodElfMaleFaceLower00_00.blp", "BloodElfMaleFaceUpper00_00.blp", "",
         "0"},
        {"3", "10", "0", "2", "0", "0", "FacialLowerHair00_00.blp", "FacialUpperHair00_00.blp", "", "0"},
        {"4", "10", "0", "3", "0", "0", "Hair00_00.blp", "ScalpLowerHair00_00.blp", "ScalpUpperHair00_00.blp",
         "0"},
        {"5", "10", "0", "4", "0", "0", "BloodElfMaleNakedPelvisSkin00_00.blp", "", "", "0"},
    })));
    CharSectionsFields f;
    const CharacterAppearance who{.raceId = 10, .sexId = 0, .skinId = 0, .faceId = 0, .hairStyleId = 0,
                                  .hairColorId = 0, .facialHairId = 0};
    auto shipped = [](const std::string& path, void*) {
        return path.find("FacialLowerHair") == std::string::npos &&
               path.find("FacialUpperHair") == std::string::npos && path.find("Scalp") == std::string::npos;
    };
    const CharacterSectionTextures t = resolveCharacterSections(&dbc, f, who, shipped, nullptr);
    CHECK(t.facialLower.empty());
    CHECK(t.facialUpper.empty());
    CHECK(t.scalpLower.empty());
    CHECK(t.scalpUpper.empty());
    // What is there is untouched.
    CHECK(t.bodySkin == "BloodElfMaleSkin00_00.blp");
    CHECK(t.hair == "Hair00_00.blp");
    CHECK(t.faceLower == "BloodElfMaleFaceLower00_00.blp");
    REQUIRE(t.underwear.size() == 1);

    const auto layers = faceRegionLayers(t);
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].path == "BloodElfMaleFaceLower00_00.blp");
    CHECK(layers[1].path == "BloodElfMaleFaceUpper00_00.blp");

    // Without a way to ask, every name is kept.
    CHECK(resolveCharacterSections(&dbc, f, who).facialLower == "FacialLowerHair00_00.blp");
}
