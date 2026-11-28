#pragma once

/// What the line of a failed cast names (0x00808200, from 0x008085d6 on): the
/// weapons a spell wants, the forms, a mechanic, a limit. Each takes the table
/// it is in - a pipeline::DBCFile, or a test's own.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

namespace wowee::game::cast_failure {

/// SPELL_FAILED_EQUIPPED_ITEM_CLASS and its two hands: ItemSubClassMask's name
/// for exactly this class and mask ("Nahkampfwaffe"), else the first of the
/// class's subclasses in the mask (ItemSubClass, 0x007fea30).
template <typename Dbc>
[[nodiscard]] std::string itemClassName(const Dbc* masks, const Dbc* subClasses, uint32_t itemClass, uint32_t mask) {
    if (masks && masks->getFieldCount() > 2) {
        for (uint32_t r = 0; r < masks->getRecordCount(); ++r)
            if (masks->getUInt32(r, 0) == itemClass && masks->getUInt32(r, 1) == mask) return masks->getString(r, 2);
    }
    if (subClasses && subClasses->getFieldCount() > 10) {
        for (uint32_t r = 0; r < subClasses->getRecordCount(); ++r)
            if (subClasses->getUInt32(r, 0) == itemClass && (mask & (1u << (subClasses->getUInt32(r, 1) & 31u))) != 0)
                return subClasses->getString(r, 10);
    }
    return {};
}

/// SPELL_FAILED_ONLY_SHAPESHIFT: the named SpellShapeshiftForm rows of the
/// spell's Stances, whose bit k is row k (0x007fe850, 0x0061bd00) - all of
/// them, or as many as are wanted: of a pet's spell the first (0x00805610).
template <typename Dbc>
[[nodiscard]] std::string formNames(const Dbc* forms, const uint32_t (&stances)[2], uint32_t most = 64) {
    std::string out;
    if (!forms || forms->getFieldCount() <= 2) return out;
    const uint32_t rows = std::min<uint32_t>(forms->getRecordCount(), 64);
    for (uint32_t r = 0; r < rows && most > 0; ++r) {
        if ((stances[r >> 5] & (1u << (r & 31u))) == 0) continue;
        const std::string name = forms->getString(r, 2);
        if (name.empty()) continue;
        if (!out.empty()) out += ", ";
        out += name;
        --most;
    }
    return out;
}

/// A row's name in the column after its id (SpellMechanic).
template <typename Dbc>
[[nodiscard]] std::string rowName(const Dbc* dbc, uint32_t id) {
    if (!dbc || dbc->getFieldCount() < 2 || id == 0) return {};
    const int32_t row = dbc->findRecordById(id);
    return row < 0 ? std::string() : dbc->getString(static_cast<uint32_t>(row), 1);
}

struct Limit {
    uint32_t count = 0;
    std::string name;
};
/// SPELL_FAILED_TOO_MANY_OF_ITEM's ItemLimitCategory: the name, sixteen
/// languages and their flags, and behind them how many there may be.
template <typename Dbc>
[[nodiscard]] std::optional<Limit> limitCategory(const Dbc* dbc, uint32_t id) {
    if (!dbc || dbc->getFieldCount() < 19 || id == 0) return std::nullopt;
    const int32_t row = dbc->findRecordById(id);
    if (row < 0) return std::nullopt;
    return Limit{dbc->getUInt32(static_cast<uint32_t>(row), 18), dbc->getString(static_cast<uint32_t>(row), 1)};
}

/// Whether the text is the one with _PET behind its key, where the interface
/// has it: a spell that teaches the pet (LEARN_PET_SPELL, or LEARN_SPELL whose
/// target is the pet).
[[nodiscard]] constexpr bool hasPetText(uint32_t firstEffect, uint32_t firstTarget) {
    return firstEffect == 0x39 || (firstEffect == 0x24 && firstTarget == 5);
}

/// The item a failure names, whose name comes with its data (0x67ca30): none
/// for REAGENTS with -2 (one free bag slot), which 0x00808200 says as the
/// game error 0, ERR_INV_FULL, with its sound (0x005216f0(0)).
inline constexpr uint32_t kNoReagentNamed = 0xFFFFFFFEu;

}  // namespace wowee::game::cast_failure
