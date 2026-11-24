#pragma once

#include <cstdint>
#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace wowee {
namespace game {

/// The threat list of one unit as the client keeps it: whom its server named
/// the victim (CGUnit_C +0xfd8) and, in the order they came, everyone on the
/// list (+0xfe0). An entry stays until it is taken off; nothing is sorted and
/// nobody is the victim for having the most threat.
class ThreatTable {
public:
    struct Entry {
        uint64_t victim = 0;
        uint8_t status = 1;       ///< +0x28: 1 and 2 (as much as the victim or more), the victim 3 (another at 2) and 4
        uint8_t percent = 0xff;   ///< +0x29: of the victim's threat, at most 250; 255 not reckoned yet
        int32_t threat = 0;       ///< +0x2c
    };

    /// What a change of the list calls for.
    struct Changes {
        /// UNIT_THREAT_SITUATION_UPDATE is queued for these, in order.
        std::vector<uint64_t> situations;
        /// UNIT_THREAT_LIST_UPDATE is queued for the unit itself.
        bool list = false;
        /// The player's threat floaters (0x00719220), from and to.
        std::vector<std::pair<uint8_t, uint8_t>> floaters;
        /// SoundEntries of threatPlaySounds.
        std::vector<uint32_t> sounds;
        /// Who is on the list now and was not (0x0052b370).
        std::vector<uint64_t> joined;
    };

    /// The player's threat reached the victim's (0x0071c3b0).
    static constexpr uint32_t kSoundReached = 0x3b9e;
    /// The player's status as the victim changed (0x00737620).
    static constexpr uint32_t kSoundVictim = 0x3b9f;

    /// SMSG_THREAT_UPDATE and, naming the `victim`, SMSG_HIGHEST_THREAT_UPDATE
    /// (0x007417a0).
    [[nodiscard]] Changes update(std::optional<uint64_t> victim,
                                 const std::vector<std::pair<uint64_t, int32_t>>& threats, uint64_t player) {
        Changes c;
        if (victim) {
            victim_ = *victim;
            if (victim_ != 0) (void)entryOf(victim_, player, c);
        }
        bool all = victim.has_value();
        std::vector<uint64_t> named;
        for (const auto& [guid, threat] : threats) {
            entryOf(guid, player, c).threat = threat;
            all = all || guid == victim_;
            named.push_back(guid);
        }
        bool changed = false;
        if (const Entry* top = find(victim_); top && victim_ != 0) {
            const int32_t topThreat = top->threat;
            if (all) {
                for (Entry& e : entries_)
                    if (e.victim != victim_ && reckon(e, topThreat, player, c)) changed = true;
            } else {
                for (const uint64_t guid : named)
                    if (guid != victim_ && reckon(*find(guid), topThreat, player, c)) changed = true;
            }
        }
        if (victim || changed) reckonVictim(player, c);
        c.list = true;
        return c;
    }

    /// SMSG_THREAT_REMOVE (0x00737750): nothing of one not on the list.
    [[nodiscard]] Changes remove(uint64_t guid, uint64_t player) {
        Changes c;
        const Entry* e = find(guid);
        if (!e) return c;
        bool again = false;
        if (guid == victim_) victim_ = 0;
        else again = e->status > 1;
        entries_.erase(entries_.begin() + (e - entries_.data()));
        c.situations.push_back(guid);
        if (again) reckonVictim(player, c);
        c.list = true;
        return c;
    }

    /// SMSG_THREAT_CLEAR, and the unit leaving the world (0x007345c0):
    /// nothing of an empty list.
    [[nodiscard]] Changes clear() {
        Changes c;
        if (entries_.empty()) return c;
        for (const Entry& e : entries_) c.situations.push_back(e.victim);
        entries_.clear();
        victim_ = 0;
        c.list = true;
        return c;
    }

    [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }
    [[nodiscard]] uint64_t victim() const { return victim_; }

    /// 0x007374c0: how one stands on the list. Without a victim named, its
    /// status alone, at 1.
    struct Standing {
        bool known = false;
        bool victim = false;
        uint8_t status = 0;
        uint8_t percent = 0;
        int32_t threat = 0;
    };
    [[nodiscard]] Standing standing(uint64_t guid) const {
        const Entry* e = find(guid);
        if (victim_ == 0 || !e) return {.status = static_cast<uint8_t>(e ? 1 : 0)};
        return {.known = true, .victim = guid == victim_, .status = e->status, .percent = e->percent,
                .threat = e->threat};
    }

private:
    [[nodiscard]] const Entry* find(uint64_t guid) const {
        const auto it = std::find_if(entries_.begin(), entries_.end(),
                                     [guid](const Entry& e) { return e.victim == guid; });
        return it != entries_.end() ? &*it : nullptr;
    }
    [[nodiscard]] Entry* find(uint64_t guid) {
        return const_cast<Entry*>(std::as_const(*this).find(guid));
    }
    /// 0x007416f0.
    Entry& entryOf(uint64_t guid, uint64_t player, Changes& c) {
        if (Entry* e = find(guid)) return *e;
        if (guid == player) playerNew_ = true;
        c.joined.push_back(guid);
        entries_.push_back({.victim = guid});
        return entries_.back();
    }
    /// 0x0071c3b0: one's share of the victim's threat; whether its status moved.
    bool reckon(Entry& e, int32_t topThreat, uint64_t player, Changes& c) {
        uint8_t percent = 0;
        if (topThreat == 0) {
            percent = 100;
        } else if (e.threat > 0) {
            const int64_t share = static_cast<int64_t>(e.threat) * 100 / topThreat;
            percent = static_cast<uint8_t>(share < 0 ? share : std::min<int64_t>(share, 250));
        }
        if (percent == e.percent) return false;
        e.percent = percent;
        const uint8_t now = percent < 100 ? 1 : 2;
        const bool moved = now != e.status;
        if (moved) {
            if (e.victim == player) {
                if (now == 2 && e.status == 1) c.sounds.push_back(kSoundReached);
                if (!playerNew_) c.floaters.emplace_back(e.status, now);
            }
            e.status = now;
            c.situations.push_back(e.victim);
        }
        if (e.victim == player) playerNew_ = false;
        return moved;
    }
    /// 0x00737620: the victim is safe, or is not while another is at 2.
    void reckonVictim(uint64_t player, Changes& c) {
        Entry* top = find(victim_);
        if (!top) return;
        const bool rivalled = std::any_of(entries_.begin(), entries_.end(), [this](const Entry& e) {
            return e.victim != victim_ && e.status == 2;
        });
        const uint8_t now = rivalled ? 3 : 4;
        if (now != top->status) {
            if (victim_ == player && !playerNew_) {
                c.floaters.emplace_back(top->status, now);
                c.sounds.push_back(kSoundVictim);
            }
            top->status = now;
            c.situations.push_back(victim_);
        }
        if (victim_ == player) playerNew_ = false;
    }

    uint64_t victim_ = 0;
    std::vector<Entry> entries_;
    bool playerNew_ = false;   ///< +0xa30 & 8
};

/// 0x007374c0: the percentage towards being the victim: 110 of its threat take
/// it in melee reach, 130 from farther off or where one is not in sight.
inline float scaledThreatPercent(const ThreatTable::Standing& s, bool inMeleeReach) {
    if (!s.known) return 0.0f;
    if (s.victim) return 100.0f;
    return static_cast<float>(s.percent) * (inMeleeReach ? 0.9090909f : 0.7692308f);
}

/// 0x00519d50: one's status over all the lists it is on, one after the other.
constexpr uint8_t worstThreatStatus(uint8_t soFar, uint8_t here) {
    if (soFar < 3) return soFar < here ? here : soFar;
    return soFar == 4 && here == 3 ? uint8_t{3} : soFar;
}

/// 0x0061a4f0 (0x00ad2d70): a status' colour, ARGB.
constexpr uint32_t threatStatusColor(uint32_t status) {
    constexpr uint32_t kColors[] = {0xffffffffu, 0xffb0b0b0u, 0xffffff77u, 0xffff9900u, 0xffff0000u};
    return kColors[status > 4 ? 1 : status];
}

} // namespace game
} // namespace wowee
