#pragma once

// AnimationData.dbc as the client walks it: the animation a model plays for
// one it lacks, and the animation a unit is put in for the one it is asked.
//
// Two lookups, one under the other. 0x00826350 is the model's own: asked an
// animation it has no sequence for, it follows the row's Fallback (+0x14)
// until it reaches one it has - turning the playback round where a row it
// passes has Flags 0x10 and holding it still where one has 0x20 - and ends on
// Stand, else Closed, else its first sequence. Every animation a model is
// told to play goes through it (0x00832ab0), a seat's ride animation as much
// as a cast. 0x007176f0 is the unit's, ahead of that: an animation it is
// asked becomes the one of its animation tier (UNIT_FIELD_BYTES_1 byte 3,
// +0xb80) with that behaviour - FlyRun for Run at tier 3 - where the model
// has it, else along the Fallbacks, else the tier's stand, else the same
// again at the tier's parent (0x00a349b4).

#include <algorithm>
#include <bitset>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace wowee {
namespace pipeline {
class DBCFile;
}
namespace rendering {
namespace anim {

/// What 0x007176b0 answers for an id with no row; also the end of the ids
/// the walks follow (0x1f9 is the last).
inline constexpr uint32_t kNoBehavior = 0x1fa;
inline constexpr uint32_t kAnimationIdLimit = 0x1fa;

/// AnimationData Flags (+0x10): the fallback plays backwards (Walkbackwards
/// on Walk), or is held where it stands (Dead on Death's last frame).
inline constexpr uint32_t kAnimFlagReverse = 0x10;
inline constexpr uint32_t kAnimFlagHold = 0x20;

/// A row as the client keeps it (0x00ad30e4, eight dwords: ID, Name,
/// WeaponFlags, BodyFlags, Flags, Fallback, BehaviorID, BehaviorTier).
struct AnimationDataRecord {
    uint32_t id = 0;
    uint32_t flags = 0;     ///< +0x10
    uint32_t fallback = 0;  ///< +0x14
    uint32_t behavior = 0;  ///< +0x18
    uint32_t tier = 0;      ///< +0x1c
};

/// The rows in file order (0x00ad30e4, 0x00ad30d0 of them) and by id
/// (0x00ad30e8 from 0x00ad30d8 to 0x00ad30d4).
class AnimationDataTable {
public:
    AnimationDataTable() = default;
    explicit AnimationDataTable(std::vector<AnimationDataRecord> rows) : rows_(std::move(rows)) {
        if (rows_.empty()) return;
        uint32_t lo = rows_.front().id, hi = lo;
        for (const auto& r : rows_) {
            lo = std::min(lo, r.id);
            hi = std::max(hi, r.id);
        }
        minId_ = lo;
        byId_.assign(static_cast<size_t>(hi - lo) + 1, -1);
        for (size_t i = 0; i < rows_.size(); ++i) byId_[rows_[i].id - lo] = static_cast<int32_t>(i);
    }

    [[nodiscard]] const AnimationDataRecord* find(uint32_t id) const {
        if (id < minId_ || id - minId_ >= byId_.size()) return nullptr;
        const int32_t i = byId_[id - minId_];
        return i < 0 ? nullptr : &rows_[static_cast<size_t>(i)];
    }
    /// 0x007176b0: the row's BehaviorID, kNoBehavior without one.
    [[nodiscard]] uint32_t behavior(uint32_t id) const {
        const auto* r = find(id);
        return r ? r->behavior : kNoBehavior;
    }
    [[nodiscard]] const std::vector<AnimationDataRecord>& rows() const { return rows_; }
    [[nodiscard]] bool empty() const { return rows_.empty(); }

private:
    std::vector<AnimationDataRecord> rows_;
    uint32_t minId_ = 0;
    std::vector<int32_t> byId_;
};

/// How the model plays what 0x00826350 finds (its second short, read by
/// 0x00826b00): forward; backwards from the end; held on the first frame;
/// held on the last.
enum class PlayMode : uint8_t { Forward = 0, Reversed = 1, HoldStart = 2, HoldEnd = 3 };

/// The time a sequence of `duration` ms is sampled at after `time` ms of
/// play: backwards from its end where it plays reversed (0x00826b00 negates the
/// speed and starts it at its length), as it is otherwise. Every reader of a
/// unit's tracks - its bones, its events, its lights - samples through this.
inline float sampledSequenceTime(float time, uint32_t duration, PlayMode mode) {
    if (mode != PlayMode::Reversed) return time;
    return std::max(0.0f, static_cast<float>(duration) - time);
}

struct ModelAnimation {
    uint32_t id = 0;
    PlayMode mode = PlayMode::Forward;
};

/// 0x00826350: the sequence a model plays for `id`. `has` answers whether the
/// model has a sequence of an id (0x00825e00); `firstSequenceId` is its first
/// sequence's, the last resort after Stand and Closed.
template <class Has>
[[nodiscard]] ModelAnimation modelAnimation(const AnimationDataTable& data, uint32_t id, const Has& has,
                                            uint32_t firstSequenceId) {
    if (has(id)) return {id, PlayMode::Forward};
    const uint32_t standIn = has(0u) ? 0u : has(0x93u) ? 0x93u : firstSequenceId;
    std::bitset<kAnimationIdLimit> seen;
    int direction = 1;
    int turns = 0;
    uint32_t cur = id;
    for (;;) {
        const AnimationDataRecord* rec = data.find(cur);
        if (cur >= kAnimationIdLimit || seen[cur] || rec == nullptr || rec->fallback == cur)
            return {standIn, PlayMode::Forward};
        seen[cur] = true;
        if ((rec->flags & kAnimFlagReverse) != 0) {
            turns += direction;
            direction = -direction;
        }
        if ((rec->flags & kAnimFlagHold) != 0) {
            turns += direction;
            direction = 0;
        }
        cur = rec->fallback;
        if (has(cur)) break;
    }
    if (direction > 0) return {cur, PlayMode::Forward};
    if (direction < 0) return {cur, PlayMode::Reversed};
    return {cur, turns > 0 ? PlayMode::HoldEnd : PlayMode::HoldStart};
}

/// 0x00826050 (0x00825f40 under it): whether a model plays `id` from a
/// sequence of its own - the id's or one along its Fallbacks - rather than
/// from the stand-ins 0x00826350 ends on.
template <class Has>
[[nodiscard]] bool modelPlays(const AnimationDataTable& data, uint32_t id, const Has& has) {
    if (id >= kAnimationIdLimit) return false;
    std::bitset<kAnimationIdLimit> seen;
    uint32_t cur = id;
    while (!has(cur)) {
        const AnimationDataRecord* rec = data.find(cur);
        if (cur >= kAnimationIdLimit || seen[cur] || rec == nullptr || rec->fallback == cur) return false;
        seen[cur] = true;
        cur = rec->fallback;
    }
    return true;
}

/// 0x00717540: the animation of tier `tier` with behaviour `behavior` - the
/// id itself where it is a ground one asked at the ground tier, or one of a
/// tier of its own; else the first row of that behaviour and tier.
[[nodiscard]] inline std::optional<uint32_t> tierAnimation(const AnimationDataTable& data, uint32_t behavior,
                                                           uint32_t tier) {
    const auto& rows = data.rows();
    if (behavior >= rows.size()) return std::nullopt;
    const auto& atIndex = rows[behavior];
    if (tier == 0 && atIndex.behavior == behavior && atIndex.tier == 0) return atIndex.id;
    if (const auto* rec = data.find(behavior); rec != nullptr && rec->tier != 0) return behavior;
    for (const auto& r : rows) {
        if (r.behavior == behavior && r.tier == tier) return r.id;
    }
    return std::nullopt;
}

/// 0x00717600: the tier's stand - the first row of behaviour 0 at it.
[[nodiscard]] inline std::optional<uint32_t> tierStand(const AnimationDataTable& data, uint32_t tier) {
    const auto& rows = data.rows();
    if (rows.empty()) return std::nullopt;
    if (tier == 0 && rows[0].behavior == 0 && rows[0].tier == 0) return rows[0].id;
    if (const auto* rec = data.find(0); rec != nullptr && rec->tier != 0) return 0u;
    for (const auto& r : rows) {
        if (r.behavior == 0 && r.tier == tier) return r.id;
    }
    return std::nullopt;
}

/// 0x00a349b4: the tier a tier falls back to - Hover to Fly, the rest to
/// the ground.
[[nodiscard]] inline uint32_t parentTier(uint32_t tier) { return tier == 2 ? 3u : 0u; }

/// 0x007176f0: the animation a unit of animation tier `tier` is put in when
/// asked `id`. `has` is the model's 0x00825e00, as for modelAnimation. With
/// no AnimationData, or where nothing is found at the ground tier (where the
/// client would go round again), the id as asked.
template <class Has>
[[nodiscard]] uint32_t unitAnimation(const AnimationDataTable& data, uint32_t id, uint32_t tier, const Has& has) {
    if (data.empty()) return id;
    uint32_t t = tier;
    for (int pass = 0; pass < 4; ++pass) {
        std::bitset<kAnimationIdLimit> seen;
        uint32_t cur = id;
        for (;;) {
            if (const auto a = tierAnimation(data, cur, t); a && modelPlays(data, *a, has)) return *a;
            const AnimationDataRecord* rec = data.find(cur);
            if (cur >= kAnimationIdLimit || seen[cur] || rec == nullptr || rec->fallback == cur) break;
            seen[cur] = true;
            cur = rec->fallback;
        }
        if (const auto stand = tierStand(data, t); stand && has(*stand)) return *stand;
        if (t == 0) break;
        t = parentTier(t);
    }
    return id;
}

/// The table the game loaded (empty until it has): what the renderer's
/// lookups walk.
[[nodiscard]] const AnimationDataTable& animationData();
void setAnimationData(std::shared_ptr<const AnimationDataTable> table);
/// The rows of an AnimationData.dbc.
[[nodiscard]] std::shared_ptr<const AnimationDataTable> loadAnimationData(const pipeline::DBCFile& dbc);

}  // namespace anim
}  // namespace rendering
}  // namespace wowee
