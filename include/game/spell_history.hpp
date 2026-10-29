#pragma once

/// The spell history the client keeps for the player and, beside it, for the
/// pet (Spell_C.cpp, "AUSPELLHISTORY"): one entry a cast, with when its
/// cooldown, its category's and its global cooldown began and how long they
/// run. Two lists at 0x00d3f5ac, 24 bytes apart - the player's and the pet's.
/// Times are in milliseconds of the client's clock (0x0086ae20).

#include <cstdint>
#include <optional>
#include <vector>

namespace wowee::game::spell_history {

/// One entry (0x30 bytes: +8 spell, +0xc item, +0x10 start, +0x14
/// duration, +0x18 category, +0x1c its start, +0x20 its duration, +0x24 held
/// until an event, +0x28 the global cooldown's category, +0x2c its length).
struct Entry {
    uint32_t spellId = 0;
    uint32_t itemId = 0;
    int64_t start = 0;
    int32_t duration = 0;
    uint32_t category = 0;
    int64_t categoryStart = 0;
    int32_t categoryDuration = 0;
    bool onHold = false;
    uint32_t gcdCategory = 0;
    int32_t gcd = 0;
};

/// 0x00807d40's window: a spell whose cooldown began less than this long ago.
inline constexpr int64_t kRecentMs = 1500;

class History {
public:
    /// 0x00805230: kept only with something to run, or held.
    void add(const Entry& e) {
        if (e.duration != 0 || e.categoryDuration != 0 || e.onHold || e.gcd != 0) entries_.push_back(e);
    }
    /// 0x00802a50.
    void clear() { entries_.clear(); }
    /// 0x00802b00: those not held whose cooldown and category's have run out.
    void prune(int64_t now) {
        std::erase_if(entries_, [now](const Entry& e) {
            return !e.onHold && (e.duration == 0 || now - e.start - e.duration >= 0) &&
                   (e.categoryDuration == 0 || now - e.categoryStart - e.categoryDuration >= 0);
        });
    }
    /// 0x00804720 (SMSG_PET_CAST_FAILED): the spell's global cooldown is
    /// taken off, and with one so, the list pruned. Whether a record matched
    /// is the answer the client acts on - 0x00804720 refreshes the cooldown
    /// displays only for a list it actually changed.
    bool dropGlobalCooldown(uint32_t spellId, int64_t now) {
        bool any = false;
        for (auto& e : entries_) {
            if (e.spellId != spellId) continue;
            e.gcdCategory = 0;
            e.gcd = 0;
            any = true;
        }
        if (any) prune(now);
        return any;
    }
    /// 0x00802970: SMSG_CLEAR_COOLDOWN (`remove`) takes the spell's entries
    /// off; SMSG_COOLDOWN_EVENT starts a held one's cooldown now.
    void event(uint32_t spellId, int64_t now, bool remove) {
        std::erase_if(entries_, [&](Entry& e) {
            if (e.spellId != spellId) return false;
            if (remove) return true;
            if (e.onHold) {
                e.gcd = 0;
                e.start = now;
                e.categoryStart = now;
                e.onHold = false;
            }
            return false;
        });
    }
    /// 0x00802a10 (SMSG_MODIFY_COOLDOWN): both starts move by `delta`.
    void modify(uint32_t spellId, int32_t delta) {
        for (auto& e : entries_) {
            if (e.spellId != spellId) continue;
            e.start += delta;
            e.categoryStart += delta;
        }
    }
    /// 0x00807d40: the spell's cooldown began less than 1.5 s ago.
    [[nodiscard]] bool startedRecently(uint32_t spellId, int64_t now) const {
        for (const auto& e : entries_) {
            if (e.spellId == spellId && now - e.start - kRecentMs < 0) return true;
        }
        return false;
    }
    [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }

    /// What 0x00807980 answers of a spell: the cooldown
    /// that runs longest of the spell's own, its category's (Category, or
    /// any whose SpellCategory row has Flags 0x2 - `shared`) and the global
    /// cooldown of its StartRecoveryCategory, with its start and length; a
    /// held entry counts as starting now, the spell's own a millisecond
    /// long and running a millisecond more, and leaves it disabled.
    struct Cooldown {
        bool running = false;  ///< any ends after now
        int64_t start = 0;
        int32_t duration = 0;
        bool enabled = true;
    };
    template <typename Shared>
    [[nodiscard]] Cooldown query(uint32_t spellId, uint32_t category, uint32_t startRecoveryCategory, int64_t now,
                                 Shared&& shared) const {
        return query(spellId, category, startRecoveryCategory, now, shared, std::nullopt,
                     [](uint32_t) { return false; });
    }
    /// With a rune cost the runes do not pay, ready at `runesReady`
    /// (0x005ee7e0): an entry of the spell's own, or of one sharing a rune
    /// type with it (`sharesRunes`), runs until then, as ten seconds. Cast
    /// from an item (`itemId`), the spell's own entries are that item's.
    template <typename Shared, typename SharesRunes>
    [[nodiscard]] Cooldown query(uint32_t spellId, uint32_t category, uint32_t startRecoveryCategory, int64_t now,
                                 Shared&& shared, std::optional<int64_t> runesReady,
                                 SharesRunes&& sharesRunes, uint32_t itemId = 0) const {
        Cooldown out;
        int64_t latest = now;
        const auto take = [&](int64_t end, int64_t start, int32_t duration, const Entry& e, bool enable) {
            if (end - latest < 0) return;
            out.start = start;
            out.duration = duration;
            latest = end;
            if (enable && out.enabled) out.enabled = !e.onHold;
        };
        for (const auto& e : entries_) {
            if (e.spellId == spellId && e.itemId == itemId && (e.duration != 0 || e.onHold))
                take((e.onHold ? now + 1 : e.start) + e.duration, e.onHold ? now : e.start, e.onHold ? 1 : e.duration,
                     e, true);
            if (((e.category != 0 && e.category == category) || shared(e.category)) && e.categoryDuration != 0)
                take((e.onHold ? now : e.categoryStart) + e.categoryDuration, e.onHold ? now : e.categoryStart,
                     e.categoryDuration, e, true);
            if (e.gcdCategory == startRecoveryCategory && e.gcd != 0) take(e.start + e.gcd, e.start, e.gcd, e, false);
            if (runesReady && (e.spellId == spellId || sharesRunes(e.spellId)) && *runesReady - latest >= 0) {
                latest = *runesReady;
                out.start = *runesReady - 10000;
                out.duration = 10000;
            }
        }
        out.running = latest != now;
        return out;
    }

private:
    std::vector<Entry> entries_;
};

/// 0x00805b10 (SMSG_PET_SPELLS): a cooldown sent with `remaining` left of
/// one the spell sets at `recovery` (the longer of the two, through the
/// modifiers) began that much less ago; none, it has not begun (0).
template <typename Mods>
[[nodiscard]] constexpr Entry fromRemaining(uint32_t spellId, uint32_t category, int32_t remaining,
                                            int32_t categoryRemaining, bool onHold, int32_t recovery,
                                            int32_t categoryRecovery, int64_t now, Mods&& mods) {
    Entry e;
    e.spellId = spellId;
    e.category = category;
    e.onHold = onHold;
    if (remaining > 0) {
        const int32_t total = mods(recovery > remaining ? recovery : remaining);
        e.duration = total;
        e.start = now - (total - remaining);
    }
    if (categoryRemaining > 0) {
        const int32_t total = categoryRecovery > categoryRemaining ? categoryRecovery : categoryRemaining;
        e.categoryDuration = total;
        e.categoryStart = now - (total - categoryRemaining);
    }
    return e;
}

/// 0x00805d70 / 0x0080e1b0: a global cooldown of StartRecoveryTime through
/// its modifiers; the usual 1.5 s (category 133) of a spell neither melee
/// nor ranged (DmgClass 2, 3), nor Attributes 0x12, shortened by the
/// caster's casting speed to 1 to 1.5 s.
[[nodiscard]] constexpr int32_t globalCooldown(uint32_t category, int32_t moddedTime, int32_t baseTime,
                                               uint32_t dmgClass, uint32_t attr, float castSpeed) {
    int32_t gcd = moddedTime;
    if (category == 133 && baseTime == 1500 && dmgClass != 2 && dmgClass != 3 && (attr & 0x12u) == 0) {
        const float hasted = static_cast<float>(gcd) * castSpeed - 0.5f;
        const int32_t r = static_cast<int32_t>(hasted >= 0.0f ? hasted + 0.5f : hasted - 0.5f);
        gcd = r < 1000 ? 1000 : (r > 1500 ? 1500 : r);
    }
    return gcd;
}

}  // namespace wowee::game::spell_history
