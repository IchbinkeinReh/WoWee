#pragma once

/// A unit's power as the client predicts it between the server's updates
/// (CGUnit_C +0xfb4, `predictedPower`): what UnitPower answers (0x0060ed40)
/// and the cast is held to (0x008017e0) while the CVar is on (0x0071c2e0).
/// The active player and its pet regenerate it every tenth of a second or
/// more (0x005196f0, 0x00728a20, 0x00722d80); the server's word sets it
/// (0x00722c50) or moves it by an energize or a drain (0x00722cf0).
/// Arithmetic only: the caller reads the unit.

#include <array>
#include <cmath>
#include <cstdint>

namespace wowee::game::power_prediction {

/// 0x004f5300's table (0x00b6ba80): a power's own regeneration per second,
/// regenerating as usual and interrupted. Mana has none of its own; rage and
/// runic power fall out of combat; focus and energy fill.
inline constexpr float kBaseRegen[7][2] = {
    {0.0f, 0.0f}, {-12.5f, 0.0f}, {5.0f, 5.0f}, {10.0f, 10.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {-12.5f, 0.0f}};

/// 0x005196f0: the shortest step the prediction is moved by, milliseconds.
inline constexpr uint32_t kTickMs = 100;
/// 0x0071c390: how long a mana cost interrupts mana regeneration.
inline constexpr int64_t kFiveSecondRuleMs = 5000;

struct Predicted {
    std::array<int32_t, 7> value{};  ///< +0xfb4
    uint32_t carry = 0;              ///< +0xfd0: thousandths not yet a whole point
    int64_t interruptedUntil = 0;    ///< +0xfd4: the five second rule's end, 0 none

    /// 0x0071c320: the powers as the unit's fields hold them.
    void reset(const std::array<uint32_t, 7>& fields) {
        for (int t = 0; t < 7; ++t) value[t] = static_cast<int32_t>(fields[t]);
    }
};

/// 0x00722c50: `v`, no more than `max`; whether it changed. The caller says
/// a change to `max` (UNIT_<POWER>, 0x0060bf10 0x13 + type).
[[nodiscard]] inline bool set(Predicted& p, int type, int32_t v, int32_t max) {
    if (type < 0 || type >= 7) return false;
    if (max < v) v = max;
    if (v == p.value[type]) return false;
    p.value[type] = v;
    return true;
}

/// 0x007234d0: whether a power's field update is taken - always, but for
/// the player and its pet (`predicted`, the CVar on) only ahead of the
/// prediction.
[[nodiscard]] constexpr bool takesField(bool predicted, int32_t field, int32_t value) {
    return !predicted || field > value;
}

/// 0x00722cf0: `delta` on the unit's own power type, a loss not where
/// UNIT_FIELD_FLAGS_2 0x100 holds its stats; never under zero.
[[nodiscard]] inline bool add(Predicted& p, int type, int32_t delta, int unitPowerType, uint32_t flags2,
                              int32_t max) {
    if ((delta < 0 && (flags2 & 0x100u) != 0) || type != unitPowerType || type < 0 || type >= 7) return false;
    const int32_t v = p.value[type] + delta;
    return set(p, type, v < 0 ? 0 : v, max);
}

/// 0x00722d80 past whether it is interrupted: `dtMs` at the rate 0x004f5390
/// gives - the table's plus UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER, or the
/// INTERRUPTED one (`modifier`) - in the x87's precision. A gain counts
/// thousandths into the carry while under `max`; a loss takes the carry's
/// fraction with it, down to zero, and keeps what is under a point
/// (0x00577370 truncating), not where UNIT_FIELD_FLAGS_2 0x100 holds its
/// stats. Whether it changed.
[[nodiscard]] inline bool regenerate(Predicted& p, int type, uint32_t dtMs, bool interrupted, float modifier,
                                     int32_t max, uint32_t flags2) {
    if (type < 0 || type >= 7) return false;
    const long double r = static_cast<long double>(kBaseRegen[type][interrupted ? 1 : 0]) + modifier;
    if ((r < 0 ? -r : r) < 2.384185791015625e-07L) return false;
    const long double amount = static_cast<long double>(dtMs) * r * static_cast<long double>(0.001f);
    const int32_t now = p.value[type];
    if (amount >= 0) {
        if (now >= max) return false;
        p.carry += static_cast<uint32_t>(static_cast<int64_t>(amount * 1000.0L));
        const bool changed = set(p, type, static_cast<int32_t>(p.carry / 1000u) + now, max);
        p.carry %= 1000u;
        return changed;
    }
    if (now <= 0 || (flags2 & 0x100u) != 0) return false;
    long double v = static_cast<long double>(now) +
                    static_cast<long double>(p.carry) * static_cast<long double>(0.001f) + amount;
    if (v < 0) v = 0;
    const float vf = static_cast<float>(v);
    const auto n = static_cast<int32_t>(vf);
    const bool changed = set(p, type, n, max);
    const float rest = static_cast<float>(static_cast<long double>(vf) * 1000.0L - static_cast<long double>(n * 1000));
    p.carry = static_cast<uint32_t>(static_cast<int32_t>(std::nearbyint(static_cast<long double>(rest) - 0.5L)));
    return changed;
}

/// 0x00722d80's question: whether `type` regenerates as interrupted - mana
/// inside the five second rule (cleared once past it) or channelling a mana
/// spell that costs (`channellingCost`), anything else in combat
/// (UNIT_FIELD_FLAGS 0x80000) or under SPELL_AURA_INTERRUPT_REGEN (94).
[[nodiscard]] inline bool interrupted(Predicted& p, int type, int64_t nowMs, bool channellingCost,
                                      uint32_t unitFlags, bool interruptRegenAura) {
    if (type != 0) return (unitFlags & 0x80000u) != 0 || interruptRegenAura;
    bool held = false;
    if (p.interruptedUntil != 0) {
        if (nowMs - p.interruptedUntil < 0) held = true;
        else p.interruptedUntil = 0;
    }
    return held || channellingCost;
}

}  // namespace wowee::game::power_prediction
