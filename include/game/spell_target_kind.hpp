#pragma once

#include <array>
#include <cstdint>

namespace wowee::game {

/// 0x007fe1b0: what a spell is aimed at - 2 enemies, 1 friends, 0 neither -
/// read from Spell.dbc Targets (+0x40) and each effect's ImplicitTargetA
/// (+0x158), ImplicitTargetB (+0x164) and ApplyAuraName (+0x17c).
constexpr uint32_t spellTargetKind(uint32_t targets, const std::array<uint32_t, 3>& implicitA,
                                   const std::array<uint32_t, 3>& implicitB, const std::array<uint32_t, 3>& auras) {
    // Targets 0x100 is friendly whatever follows; 0x80 an enemy unit.
    if ((targets & 0x100u) != 0) return 1;
    if ((targets & 0x80u) != 0) return 2;
    auto enemy = [](uint32_t t) {
        switch (t) {
            case 2: case 6: case 15: case 16: case 24: case 28: case 53: case 54: case 93: return true;
            default: return false;
        }
    };
    auto friendly = [](uint32_t t) {
        switch (t) {
            case 3: case 4: case 5: case 20: case 21: case 27: case 29: case 30: case 31: case 33: case 34:
            case 35: case 45: case 56: case 57: case 58: case 59: case 61: case 62: return true;
            default: return false;
        }
    };
    for (size_t i = 0; i < 3; ++i) {
        if (enemy(implicitA[i]) || enemy(implicitB[i])) return 2;
    }
    for (size_t i = 0; i < 3; ++i) {
        // The caster (1) counts as a friend unless the aura is 4.
        for (uint32_t t : {implicitA[i], implicitB[i]}) {
            if (t == 1 && auras[i] != 4) return 1;
            if (friendly(t)) return 1;
        }
    }
    return 0;
}

}  // namespace wowee::game
