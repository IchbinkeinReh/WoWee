#pragma once

/// The client's stat summary of an item (Tooltip.cpp's, 0x0061b5b0 ff.):
/// what GetItemStats and GetItemStatDelta hand to the interface. One damage
/// per second and 73 integer slots (+4 on), each slot a stat the comparison
/// tooltip can name: the armour and six resistances (0-6), the attack
/// powers (7-10), ITEM_MOD 0-48 at 11 on, block value (60), the power
/// regenerations (61-67), health regeneration (68) and the four socket
/// colours (69-72). Arithmetic only: the caller reads the item and the DBCs.

#include <array>
#include <cstdint>
#include <string>

namespace wowee::game::item_stats {

inline constexpr int kSlots = 0x49;
inline constexpr int kMeleeAttackPower = 8;
inline constexpr int kRangedAttackPower = 9;
inline constexpr int kFeralAttackPower = 10;
inline constexpr int kFirstMod = 11;      ///< ITEM_MOD_MANA's slot
inline constexpr int kBlockValue = 60;
inline constexpr int kFirstPowerRegen = 61;
inline constexpr int kHealthRegen = 68;
inline constexpr int kFirstSocket = 69;

struct Summary {
    float dps = 0.0f;
    std::array<int32_t, kSlots> v{};
    void add(int slot, int32_t amount) {
        if (slot >= 0 && slot < kSlots) v[static_cast<size_t>(slot)] += amount;
    }
};

/// One stat of ITEM_MOD `type` (0x0061b990 for each of the item's pairs,
/// 0x0061f2b0's enchantment type 5): its slot, and ATTACK_POWER (38) both
/// attack powers as well, RANGED_ATTACK_POWER (39) the ranged one.
inline void addMod(Summary& s, int32_t type, int32_t amount) {
    s.add(kFirstMod + type, amount);
    if (type == 38) {
        s.add(kMeleeAttackPower, amount);
        s.add(kRangedAttackPower, amount);
    } else if (type == 39) {
        s.add(kRangedAttackPower, amount);
    }
}

/// Spell.dbc as 0x0061b5e0 reads it.
struct AuraSpell {
    uint32_t attr = 0;
    uint32_t effect[3] = {0, 0, 0};
    uint32_t aura[3] = {0, 0, 0};
    int32_t basePoints[3] = {0, 0, 0};
    int32_t misc[3] = {0, 0, 0};
};

/// 0x00ad2dc8: the combat rating each of MOD_RATING's 25 bits is, -1 for
/// none.
inline constexpr int32_t kRatingMod[25] = {11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
                                           24, 25, 26, 27, 28, 29, 30, -1, -1, -1, 37, 44};

/// 0x0061b5e0: a passive spell's (Attributes 0x40) applied auras, each at
/// its base points and one.
inline void addPassiveAuras(Summary& s, const AuraSpell& sp) {
    if ((sp.attr & 0x40u) == 0) return;
    for (int k = 0; k < 3; ++k) {
        if (sp.effect[k] != 6) continue;
        const int32_t amount = sp.basePoints[k] + 1;
        const int32_t misc = sp.misc[k];
        switch (sp.aura[k]) {
            case 13: s.add(53, amount); break;  // MOD_DAMAGE_DONE: SPELL_DAMAGE_DONE
            case 22:                            // MOD_RESISTANCE: each school's
                for (int r = 0; r < 7; ++r)
                    if ((misc & (1 << r)) != 0) s.add(r, amount);
                break;
            case 85: s.add(kFirstPowerRegen + misc, amount); break;  // MOD_POWER_REGEN
            case 99: s.add(kMeleeAttackPower, amount); break;
            case 123: s.add(58, -amount); break;  // MOD_TARGET_RESISTANCE: SPELL_PENETRATION
            case 124: s.add(kRangedAttackPower, amount); break;
            case 135: s.add(52, amount); break;  // MOD_HEALING_DONE
            case 150: s.add(26, amount); break;
            case 158: s.add(kBlockValue, amount); break;
            case 161: s.add(kHealthRegen, amount); break;
            case 189:  // MOD_RATING
                for (int b = 0; b < 25; ++b)
                    if ((static_cast<uint32_t>(misc) & (1u << b)) != 0 && kRatingMod[b] != -1)
                        s.add(kFirstMod + kRatingMod[b], amount);
                break;
            default: break;
        }
    }
}

/// 0x0061b9e0: a summed stat (`to`) given to each part (`from`); where the
/// parts then agree they become it, else it is dropped.
template <size_t N>
inline void fold(Summary& s, int to, const int (&from)[N]) {
    const int32_t v = s.v[static_cast<size_t>(to)];
    bool same = true;
    int32_t last = 0;
    for (size_t i = 0; i < N; ++i) {
        int32_t& part = s.v[static_cast<size_t>(from[i])];
        part += v;
        same = same && (i == 0 || part == last);
        last = part;
    }
    if (!same) {
        s.v[static_cast<size_t>(to)] = 0;
        return;
    }
    s.v[static_cast<size_t>(to)] = s.v[static_cast<size_t>(from[0])];
    for (size_t i = 0; i < N; ++i) s.v[static_cast<size_t>(from[i])] = 0;
}

/// 0x0061ba90: feral attack power equal to the melee is dropped; spell
/// power, hit, crit, hit and crit taken and haste out of their melee,
/// ranged and spell parts, mana regeneration into the first power's, and
/// the attack power out of the melee and ranged ones.
inline void merge(Summary& s) {
    if (s.v[kMeleeAttackPower] == s.v[kFeralAttackPower]) s.v[kFeralAttackPower] = 0;
    fold(s, 56, {53, 52});
    fold(s, 42, {27, 28, 29});
    fold(s, 43, {30, 31, 32});
    fold(s, 44, {33, 34, 35});
    fold(s, 45, {36, 37, 38});
    fold(s, 47, {39, 40, 41});
    fold(s, 61, {54});
    fold(s, 7, {8, 9});
}

/// 0x0061dbc0: `a` less `b`, merged again.
inline Summary delta(const Summary& a, const Summary& b) {
    Summary d;
    for (size_t i = 0; i < d.v.size(); ++i) d.v[i] = a.v[i] - b.v[i];
    d.dps = a.dps - b.dps;
    merge(d);
    return d;
}

/// 0x0061a8f0: a druid's feral attack power from a main-hand weapon
/// (0x00a2d288's slots for the inventory type hold 0x8000) - fourteen for
/// each point of damage per second over 54.8.
[[nodiscard]] inline int32_t feralAttackPower(uint32_t itemClass, uint32_t inventoryType, uint32_t delayMs,
                                              float damage) {
    const bool mainHand = inventoryType == 13 || inventoryType == 17 || inventoryType == 21 || inventoryType == 22;
    if (itemClass != 2 || !mainHand) return 0;
    const float dps = damage / (static_cast<float>(delayMs) * 0.001f);
    if (!(dps > 54.81f)) return 0;
    return static_cast<int32_t>((dps - 54.81f) * 14.0f + 0.5f);
}

/// 0x00707650: a random suffix's share (in hundredths of a percent) of its
/// factor, rounded down.
[[nodiscard]] inline int32_t suffixAmount(int32_t allocationPct, uint32_t factor) {
    const float x = static_cast<float>(allocationPct) * 0.0001f * static_cast<float>(factor);
    const float twice = x < 0.0f ? x + x + 0.5f : x + x - 0.5f;
    const auto r = static_cast<int32_t>(__builtin_rintf(twice));
    return r >> 1;
}

/// 0x0061bb20: the global string naming a slot - "RESISTANCE<n>_NAME",
/// the attack powers', ITEM_MOD 0-48's own (0x00ad6640) and "_SHORT",
/// block value, the power regenerations', health regeneration and
/// "EMPTY_SOCKET_<colour>". Some ITEM_MOD names are empty, and a slot of
/// one is "_SHORT".
[[nodiscard]] inline std::string key(int i) {
    static constexpr const char* kMods[49] = {
        "ITEM_MOD_MANA", "ITEM_MOD_HEALTH", "", "ITEM_MOD_AGILITY", "ITEM_MOD_STRENGTH", "ITEM_MOD_INTELLECT",
        "ITEM_MOD_SPIRIT", "ITEM_MOD_STAMINA", "", "", "", "", "ITEM_MOD_DEFENSE_SKILL_RATING",
        "ITEM_MOD_DODGE_RATING", "ITEM_MOD_PARRY_RATING", "ITEM_MOD_BLOCK_RATING", "ITEM_MOD_HIT_MELEE_RATING",
        "ITEM_MOD_HIT_RANGED_RATING", "ITEM_MOD_HIT_SPELL_RATING", "ITEM_MOD_CRIT_MELEE_RATING",
        "ITEM_MOD_CRIT_RANGED_RATING", "ITEM_MOD_CRIT_SPELL_RATING", "ITEM_MOD_HIT_TAKEN_MELEE_RATING",
        "ITEM_MOD_HIT_TAKEN_RANGED_RATING", "ITEM_MOD_HIT_TAKEN_SPELL_RATING", "ITEM_MOD_CRIT_TAKEN_MELEE_RATING",
        "ITEM_MOD_CRIT_TAKEN_RANGED_RATING", "ITEM_MOD_CRIT_TAKEN_SPELL_RATING", "ITEM_MOD_HASTE_MELEE_RATING",
        "ITEM_MOD_HASTE_RANGED_RATING", "ITEM_MOD_HASTE_SPELL_RATING", "ITEM_MOD_HIT_RATING", "ITEM_MOD_CRIT_RATING",
        "ITEM_MOD_HIT_TAKEN_RATING", "ITEM_MOD_CRIT_TAKEN_RATING", "ITEM_MOD_RESILIENCE_RATING",
        "ITEM_MOD_HASTE_RATING", "ITEM_MOD_EXPERTISE_RATING", "ITEM_MOD_ATTACK_POWER",
        "ITEM_MOD_RANGED_ATTACK_POWER", "", "ITEM_MOD_SPELL_HEALING_DONE", "ITEM_MOD_SPELL_DAMAGE_DONE",
        "ITEM_MOD_MANA_REGENERATION", "ITEM_MOD_ARMOR_PENETRATION_RATING", "ITEM_MOD_SPELL_POWER",
        "ITEM_MOD_HEALTH_REGEN", "ITEM_MOD_SPELL_PENETRATION", "ITEM_MOD_BLOCK_VALUE"};
    static constexpr const char* kSockets[4] = {"META", "RED", "YELLOW", "BLUE"};
    if (i >= kFirstMod && i < kFirstMod + 49) return std::string(kMods[i - kFirstMod]) + "_SHORT";
    if (i >= 0 && i < 7) return "RESISTANCE" + std::to_string(i) + "_NAME";
    if (i >= kFirstSocket && i < kFirstSocket + 4) return std::string("EMPTY_SOCKET_") + kSockets[i - kFirstSocket];
    if (i >= kFirstPowerRegen && i < kFirstPowerRegen + 7)
        return "ITEM_MOD_POWER_REGEN" + std::to_string(i - kFirstPowerRegen) + "_SHORT";
    switch (i) {
        case 7: return "ITEM_MOD_ATTACK_POWER_SHORT";
        case 8: return "ITEM_MOD_MELEE_ATTACK_POWER_SHORT";
        case 9: return "ITEM_MOD_RANGED_ATTACK_POWER_SHORT";
        case 10: return "ITEM_MOD_FERAL_ATTACK_POWER_SHORT";
        case kBlockValue: return "ITEM_MOD_BLOCK_VALUE_SHORT";
        case kHealthRegen: return "ITEM_MOD_HEALTH_REGEN_SHORT";
        default: return {};
    }
}

}  // namespace wowee::game::item_stats
