#pragma once

// The TargetNearest family (0x00524fc0): what the keys that go through the
// targets go through, and in which order.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace wowee::game {

/// Which targets: the filter's case (0x00518e40).
enum class NearestMode : uint8_t { Any, Enemy, EnemyPlayer, Friend, FriendPlayer, PartyMember, RaidMember };

struct NearestTarget {
    uint64_t guid = 0;
    float distSq = 0.0f;
    bool ahead = false;
};

/// 0x00524440's place test: within thirty degrees of the facing to 41 yards,
/// all round to ten. `dx`, `dy`, `dz` is the way to it, `facing` the angle
/// looked along in the same axes.
inline std::optional<NearestTarget> nearestTargetAt(uint64_t guid, float dx, float dy, float dz, float facing) {
    constexpr float kCone = 0.5235988f;
    constexpr float kTurn = 6.2831855f;
    constexpr float kAheadSq = 1681.0f;
    constexpr float kRoundSq = 100.0f;
    float off = std::fmod(std::atan2(dy, dx) - facing, kTurn);
    if (off < 0.0f) off += kTurn;
    NearestTarget out;
    out.guid = guid;
    out.distSq = dx * dx + dy * dy + dz * dz;
    out.ahead = off < kCone || kTurn - kCone < off;
    if (out.distSq > (out.ahead ? kAheadSq : kRoundSq)) return std::nullopt;
    return out;
}

/// The list gone through and where in it one is. It is made anew three seconds
/// after the last time it was asked, for another of the keys, and at its end
/// once it is a second old.
class NearestTargetCycle {
public:
    /// The target to take, 0 for none. `collect()` answers the candidates and
    /// `still(guid)` whether one is one yet.
    template <class Collect, class Still>
    uint64_t next(NearestMode mode, bool reverse, uint64_t currentTarget, uint64_t nowMs,
                  Collect&& collect, Still&& still) {
        if (nowMs == 0) nowMs = 1;
        // A second round is one over a list just made, which ends.
        for (int round = 0; round < 3; ++round) {
            if (madeMs_ == 0 || usedMs_ == 0 || nowMs >= usedMs_ + kForgottenMs || list_.empty() || mode_ != mode) {
                at_ = 0;
                madeMs_ = usedMs_ = 0;
                mode_ = mode;
                list_ = collect();
                if (list_.empty()) return 0;
                // Those ahead first, the nearer first of each (0x005131d0).
                std::sort(list_.begin(), list_.end(), [](const NearestTarget& a, const NearestTarget& b) {
                    if (a.ahead != b.ahead) return a.ahead;
                    return a.distSq < b.distSq;
                });
                madeMs_ = nowMs;
            } else if (list_[at_].guid == currentTarget) {
                if (reverse) {
                    at_ = at_ == 0 ? list_.size() - 1 : at_ - 1;
                } else if (++at_ >= list_.size()) {
                    at_ = 0;
                    if (nowMs >= madeMs_ + kOldMs) {
                        madeMs_ = 0;
                        continue;
                    }
                }
            }
            usedMs_ = nowMs;
            size_t i = at_;
            bool anew = false;
            do {
                if (still(list_[i].guid)) {
                    at_ = i;
                    return list_[i].guid;
                }
                if (++i >= list_.size()) {
                    i = 0;
                    if (nowMs >= madeMs_ + kOldMs) {
                        anew = true;
                        break;
                    }
                }
            } while (i != at_);
            if (!anew) {
                list_.clear();
                return 0;
            }
            madeMs_ = 0;
        }
        return 0;
    }

    /// The target was let go (0x00524bf0 with none): the next key starts over.
    void targetCleared() { madeMs_ = 0; }
    void clear() {
        list_.clear();
        at_ = 0;
        madeMs_ = usedMs_ = 0;
    }

private:
    static constexpr uint64_t kForgottenMs = 3000;
    static constexpr uint64_t kOldMs = 1000;

    std::vector<NearestTarget> list_;
    size_t at_ = 0;           ///< 0x00bd08c8
    uint64_t madeMs_ = 0;     ///< 0x00bd08cc
    uint64_t usedMs_ = 0;     ///< 0x00bd08d0
    NearestMode mode_ = NearestMode::Any;   ///< 0x00bd08d4
};

}  // namespace wowee::game
