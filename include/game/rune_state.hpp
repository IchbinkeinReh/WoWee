#pragma once

/// The death knight's runes as the client keeps them (0x00c24304..0x00c24388):
/// each rune's type, the one it was born as, when its cooldown began and when
/// it will be ready, and which are ready. SMSG_RESYNC_RUNES (0x005ee440),
/// SMSG_ADD_RUNE_POWER (0x005ee520), SMSG_CONVERT_RUNE (0x005ee020) and a
/// spell's rune list (SMSG_SPELL_GO, 0x005ee240) change them; the player's
/// update makes them ready (0x00728a20); a spell's SpellRuneCost is held to
/// them (0x005ee5b0). Times are milliseconds of the client's clock
/// (0x0086ae20); `regen` is PLAYER_RUNE_REGEN by type (+0x1008 +0x1214).

#include <array>
#include <cmath>
#include <cstdint>

namespace wowee::game::runes {

inline constexpr int kMax = 8;
inline constexpr uint8_t kDeath = 3;

/// RUNE_POWER_UPDATE for a rune (0x0081b530 0x256): its slot, from 0, and
/// whether it is usable now.
struct PowerUpdate {
    int rune = 0;
    bool usable = false;
};

struct Runes {
    int count = 6;                       ///< 0x00c24384
    std::array<uint8_t, kMax> type{};    ///< 0x00c24304
    std::array<uint8_t, kMax> base{};    ///< 0x00c24324
    uint32_t ready = 0xFFFFFFFFu;        ///< 0x00c24388
    std::array<int64_t, kMax> start{};   ///< 0x00c24364: when its cooldown began, 0 none
    std::array<int64_t, kMax> readyAt{}; ///< 0x00c24344

    /// 0x005edef0: two of each of blood, unholy, frost, all ready.
    Runes() { reset(); }
    void reset() {
        count = 6;
        ready = 0xFFFFFFFFu;
        type.fill(0);
        base.fill(0);
        start.fill(0);
        readyAt.fill(0);
        for (int i = 0; i < 6; ++i) type[i] = base[i] = static_cast<uint8_t>(i / 2);
    }

    /// 0x005ee140: a rune `fraction` along its cooldown is ready after the
    /// rest of it at its type's regen rate; none for a rate under 0.01.
    void restAt(int i, float fraction, int64_t now, const std::array<float, 4>& regen) {
        const float rate = regen[type[i] & 3];
        if (rate < 0.01f) {
            readyAt[i] = 0;
            return;
        }
        readyAt[i] = now - static_cast<int64_t>(std::nearbyint((1.0f - fraction) * (1.0f / rate) * -1000.0f));
    }

    /// 0x005ee440 per rune: its type (born as it too) and how far along its
    /// cooldown it is, out of 255 - begun that much of ten seconds ago.
    void resync(int i, uint8_t newType, uint8_t progress, int64_t now, const std::array<float, 4>& regen) {
        type[i] = base[i] = newType;
        const float f = std::fmin(static_cast<float>(progress) * 0.003921569f, 1.0f);
        start[i] = now - static_cast<int64_t>(std::nearbyint(f * 10000.0f));
        restAt(i, f, now, regen);
    }

    /// 0x005ee240: ready now as `after` says. One ready before (`before`) and
    /// not now begins its cooldown, as far along as its byte says, unless
    /// already cooling; one ready now and not before, or one ready now that
    /// was cooling, is cooled. What it says of each is put in `out`
    /// (returning how many).
    int changed(uint32_t before, uint32_t after, const uint8_t* bytes, int64_t now, const std::array<float, 4>& regen,
                std::array<PowerUpdate, kMax>& out) {
        int n = 0;
        ready = after;
        for (int i = 0; i < kMax; ++i) {
            const uint32_t bit = 1u << i;
            if ((before & bit) != 0 && (after & bit) == 0) {
                if (start[i] != 0) continue;
                const float f = bytes ? std::fmin(static_cast<float>(bytes[i]) * 0.003921569f, 1.0f) : 0.0f;
                start[i] = now - static_cast<int64_t>(std::nearbyint(f * 10000.0f));
                restAt(i, f, now, regen);
                out[n++] = {i, false};
            } else if (((before & bit) == 0 && (after & bit) != 0) || ((ready & bit) != 0 && start[i] != 0)) {
                start[i] = 0;
                readyAt[i] = 0;
                out[n++] = {i, true};
            }
        }
        return n;
    }

    /// 0x005ee520: each rune in the mask not ready has its cooldown run out
    /// - begun ten seconds ago, ready now.
    int addPower(uint32_t mask, int64_t now, const std::array<float, 4>& regen, std::array<PowerUpdate, kMax>& out) {
        int n = 0;
        for (int i = 0; i < count && i < kMax; ++i) {
            const uint32_t bit = 1u << i;
            if ((mask & bit) == 0 || (ready & bit) != 0) continue;
            start[i] = now - 10000;
            restAt(i, 1.0f, now, regen);
            out[n++] = {i, false};
        }
        return n;
    }

    /// 0x00728a20: the ready mask once every cooling rune whose time has come
    /// is ready (0x005ee1f0's time, 0 where none).
    [[nodiscard]] uint32_t readyBy(int64_t now) const {
        uint32_t mask = ready;
        for (int i = 0; i < kMax; ++i) {
            const uint32_t bit = 1u << i;
            if ((ready & bit) == 0 && now - readyAt[i] >= 0) mask |= bit;
        }
        return mask;
    }

    /// 0x005ee3d0: the ready runes of a type.
    [[nodiscard]] int readyOf(uint8_t t) const {
        int n = 0;
        for (int i = 0; i < count && i < kMax; ++i)
            if (type[i] == t && (ready & (1u << i)) != 0) ++n;
        return n;
    }
};

/// 0x005ee5b0: whether the runes pay a SpellRuneCost row's blood, unholy
/// and frost - each raised by the cost percentage (the school's power cost
/// modifier and multiplier through the cost modifiers, over 100) - with
/// death runes standing in for what the others lack. `short_` gets what
/// each type lacks.
[[nodiscard]] inline bool costMet(const Runes& r, const uint32_t (&cost)[3], int32_t costPct,
                                  std::array<int32_t, 3>* short_ = nullptr) {
    int32_t lacking = 0;
    for (uint8_t t = 0; t < 3; ++t) {
        if (cost[t] == 0) continue;
        int32_t need = static_cast<int32_t>(cost[t]);
        if (costPct > 100) need = need * costPct / 100;
        const int32_t have = r.readyOf(t);
        if (have < need) {
            lacking += need - have;
            if (short_) (*short_)[t] = need - have;
        }
    }
    return lacking == 0 || r.readyOf(kDeath) >= lacking;
}

/// 0x005ee7e0: for a cost the runes do not pay, when they will - the
/// latest-cooling rune of each type lacking, a cooling death rune readying
/// sooner standing in for it; of each pair (blood 0-1, unholy 2-3, frost
/// 4-5) the sooner where one is lacking, the later where two are; the
/// latest of the three. None (0) where the cost is paid.
[[nodiscard]] inline int64_t readyFor(const Runes& r, const uint32_t (&cost)[3], int32_t costPct) {
    std::array<int32_t, 3> lack{};
    if (costMet(r, cost, costPct, &lack)) return 0;
    std::array<int64_t, kMax> at{};
    // 0x005ee1f0: a cooling rune's time, 0 for a ready one.
    const auto time = [&](int i) { return (r.ready & (1u << i)) == 0 ? r.readyAt[i] : 0; };
    int last = 0;
    int64_t latest = 0;
    for (int i = 0; i < kMax; ++i) {
        if (r.type[i] == kDeath || r.type[i] > 2 || lack[r.type[i]] == 0) continue;
        at[i] = time(i);
        if (latest == 0 || at[i] - latest >= 0) {
            last = i;
            latest = at[i];
        }
    }
    for (int i = 0; i < kMax; ++i) {
        if (r.type[i] != kDeath || (r.ready & (1u << i)) != 0 || r.readyAt[i] == 0) continue;
        const int64_t t = time(i);
        if (latest == 0) {
            at[i] = t;
            last = i;
            latest = t;
        } else if (t < latest) {
            at[last] = t;
            last = 0;
            latest = 0;
            for (int j = 0; j < kMax; ++j) {
                if (r.type[j] != kDeath && (latest == 0 || at[j] - latest >= 0)) {
                    latest = at[j];
                    last = j;
                }
            }
        }
    }
    const auto pair = [&](int32_t lacking, int a, int b) -> int64_t {
        if (lacking == 1) return at[a] != 0 && (at[b] == 0 || at[a] < at[b]) ? at[a] : at[b];
        if (lacking == 2) return at[b] < at[a] ? at[a] : at[b];
        return 0;
    };
    const int64_t blood = pair(lack[0], 0, 1), unholy = pair(lack[1], 2, 3), frost = pair(lack[2], 4, 5);
    const int64_t later = unholy > frost ? unholy : frost;
    return later < blood ? blood : later;
}

}  // namespace wowee::game::runes
