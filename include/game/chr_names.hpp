#pragma once

// A race's or a class's name as the client says it: ChrRaces' and ChrClasses'
// own words, in the client's language and the unit's gender (0x00715970,
// 0x007159e0).

#include <cstdint>
#include <string>
#include <utility>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace game {

struct GenderedName {
    std::string name;
    /// The gender the word is in: 0 male, 1 female, 2 neither - the one asked
    /// for, or the other where that one has no word of its own.
    int gender = 2;
};

/// The client's pick of a row's three: Name_female or Name_male by the gender
/// asked, the other where that one is empty, Name where both are or no gender
/// is asked.
[[nodiscard]] inline GenderedName pickGenderedName(std::string name, std::string female, std::string male,
                                                   int gender) {
    if (gender == 0) {
        if (!male.empty()) return {std::move(male), 0};
        if (!female.empty()) return {std::move(female), 1};
    } else if (gender == 1) {
        if (!female.empty()) return {std::move(female), 1};
        if (!male.empty()) return {std::move(male), 0};
    }
    return {std::move(name), gender};
}

/// Empty where the file or its layout is not to hand.
[[nodiscard]] GenderedName chrRaceName(pipeline::AssetManager* assets, uint32_t raceId, int gender);
[[nodiscard]] GenderedName chrClassName(pipeline::AssetManager* assets, uint32_t classId, int gender);

}  // namespace game
}  // namespace wowee
