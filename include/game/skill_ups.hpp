#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wowee::pipeline { class AssetManager; }

namespace wowee::game {

/// The skill-point purchase the client still carries from before trainers
/// sold ranks: points bought into a skill one rank at a time and sent off
/// together (AddSkillUp, RemoveSkillUp, AcceptSkillUps, CancelSkillUps,
/// GetAdjustedSkillPoints), and a tier bought outright (BuySkillTier).
///
/// What a rank costs comes from SkillCostsData.dbc through the skill's
/// SkillLine.SkillCostsID and its SkillRaceClassInfo cost column
/// (0x00810f50); a tier's from SkillTiers.dbc (0x00810460). The points
/// spent are the player's PLAYER_CHARACTER_POINTS2 (0x005cd970(1)). Every
/// pending rank is held per skill (+0x10 of the client's skill entry) with
/// their total (0xc22b00).
class SkillUps {
public:
    /// One SkillRaceClassInfo.dbc row (0x00810ed0 picks it).
    struct RaceClassInfo {
        uint32_t skillId = 0;
        uint32_t raceMask = 0;
        uint32_t classMask = 0;
        uint32_t flags = 0;       ///< +0x10: 0x4/0x8 a tier for sale, 0x20 may be unlearned, 0x400 rank held at 1
        uint32_t minLevel = 0;    ///< +0x14
        uint32_t tierId = 0;      ///< +0x18, SkillTiers.dbc
        uint32_t costIndex = 0;   ///< +0x1c, the SkillCostsData column
    };

    /// Reads the four tables once; harmless to call again.
    void load(pipeline::AssetManager* assets);
    bool loaded() const { return loaded_; }

    /// The row for this race and class: the first whose masks are empty or
    /// hold them (0x00810ed0).
    const RaceClassInfo* raceClassInfo(uint32_t skillId, uint8_t race, uint8_t classId) const;
    bool hasSkillLine(uint32_t skillId) const { return skillCostsId_.count(skillId) != 0; }
    /// What raising the skill to `rank` costs (0x00810f50): the cost column
    /// of the rank'th row of the skill's SkillCostsData, 0 past the last or
    /// with none.
    uint32_t rankCost(const RaceClassInfo& info, int32_t rank) const;
    /// What the tier after `tier` costs (0x00810460), 0 with none.
    uint32_t tierCost(const RaceClassInfo& info, uint32_t tier) const;

    uint32_t pending(uint32_t skillId) const;
    uint32_t pendingCost() const { return total_; }

    /// AddSkillUp (0x005cdbf0): one more rank while the skill is below its
    /// maximum and the points cover it. True when it was added.
    bool add(uint32_t skillId, const RaceClassInfo* info, uint32_t value, uint32_t max,
             uint32_t points);
    /// RemoveSkillUp (0x005cdd20): one rank fewer, giving its cost back.
    bool remove(uint32_t skillId, const RaceClassInfo* info, uint32_t value);
    /// AcceptSkillUps (0x005ce5d0): with points spent, the skills and their
    /// ranks to send, and all of them forgotten; with none, nothing.
    std::vector<std::pair<uint32_t, uint32_t>> accept();
    /// CancelSkillUps (0x005cd820): every pending rank dropped.
    void cancel();

private:
    bool loaded_ = false;
    std::vector<RaceClassInfo> raceClass_;
    std::unordered_map<uint32_t, uint32_t> skillCostsId_;  ///< SkillLine id -> SkillCostsID
    std::unordered_map<uint32_t, std::vector<std::array<uint32_t, 3>>> costs_;
    std::unordered_map<uint32_t, std::array<uint32_t, 16>> tierCosts_;
    std::map<uint32_t, uint32_t> pending_;
    uint32_t total_ = 0;
};

}  // namespace wowee::game
