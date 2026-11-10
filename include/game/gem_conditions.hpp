#pragma once

// A meta gem's activation conditions, as the reference client reads and words
// them (0x0061c0b0).
//
// A socketed gem's SpellItemEnchantment row names a ConditionID; that
// SpellItemEnchantmentCondition row holds up to five tests of how many gems of
// a colour the player has equipped, either against a number ("at least 2 red
// gems") or against another colour ("more blue gems than yellow gems"). The
// client writes one "Requires ..." line per test, white when it holds and grey
// when it does not, and the gem's own line is white only when every test holds
// (grey otherwise). The counts are of every gem socketed into what the player
// wears, each colour bit of a gem counted once (0x006de4d0): an orange gem
// counts as a red one and as a yellow one.

#include <array>
#include <cstdint>
#include <vector>

namespace wowee::game::gem_conditions {

/// Equipped gems per colour: meta, red, yellow, blue (player +0x1924..0x1930).
using Counts = std::array<int, 4>;

/// One SpellItemEnchantmentCondition row. The file packs it into 64 bytes:
/// ID, Color u8[5] @4, LtOperand u32[5] @9 (unread), Comparator u8[5] @29,
/// CompareColor u8[5] @34, Value u32[5] @39, Logic u8[5] @59.
struct Row {
    std::array<uint8_t, 5> color{};
    std::array<uint8_t, 5> comparator{};
    std::array<uint8_t, 5> compareColor{};
    std::array<int32_t, 5> value{};
    std::array<uint8_t, 5> logic{};
};

/// The GlobalStrings name of a condition colour (1..4), or nullptr for the
/// colour the client words as UNKNOWN (5 and above) and "" for 0.
const char* colorKey(uint8_t color);

/// The equipped count of a condition colour: 1..4 index meta..blue through the
/// client's table at 0xa246e0 ({-1,0,1,2,3}); anything else counts nothing.
int countOf(const Counts& counts, uint8_t color);

/// One "Requires ..." line, before the localized strings are applied.
///
/// `key` is the ENCHANT_CONDITION_* format; a value test formats it with
/// (number, colorA), a colour test with (colorA, colorB) - already in the
/// order the client passes them, which swaps the two for comparators 2 and 4.
/// A comparator past 5 has no wording: `key` is null and the line reads
/// "Requires " and nothing, white, as the client leaves it.
struct Line {
    const char* key = nullptr;
    bool valueForm = false;
    int number = 0;
    uint8_t colorA = 0;
    uint8_t colorB = 0;
    bool met = true;
};

struct Result {
    std::vector<Line> lines;
    bool allMet = true;
};

/// The client's walk over the row: test i, then go on while Logic[i] is set,
/// five tests at most.
Result evaluate(const Row& row, const Counts& counts);

/// Add one socketed gem's colour mask (GemProperties Type) to the counts.
void addGem(Counts& counts, uint32_t colorMask);

}  // namespace wowee::game::gem_conditions
