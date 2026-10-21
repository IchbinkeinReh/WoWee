#include "game/skill_ups.hpp"

#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"

namespace wowee::game {

void SkillUps::load(pipeline::AssetManager* assets) {
    if (loaded_ || !assets || !assets->isInitialized()) return;
    loaded_ = true;
    // 3.3.5a columns, as the client's records lay them out.
    if (auto dbc = assets->loadDBC("SkillRaceClassInfo.dbc"); dbc && dbc->isLoaded() &&
        dbc->getFieldCount() >= 8) {
        raceClass_.reserve(dbc->getRecordCount());
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            raceClass_.push_back({.skillId = dbc->getUInt32(i, 1),
                                  .raceMask = dbc->getUInt32(i, 2),
                                  .classMask = dbc->getUInt32(i, 3),
                                  .flags = dbc->getUInt32(i, 4),
                                  .minLevel = dbc->getUInt32(i, 5),
                                  .tierId = dbc->getUInt32(i, 6),
                                  .costIndex = dbc->getUInt32(i, 7)});
        }
    }
    // SkillLineAbility: ID, skill, spell, race mask, class mask and the two
    // that invert them.
    if (auto dbc = assets->loadDBC("SkillLineAbility.dbc"); dbc && dbc->isLoaded() &&
        dbc->getFieldCount() >= 7) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i)
            abilities_.emplace(dbc->getUInt32(i, 2),
                               Ability{.skillId = dbc->getUInt32(i, 1),
                                       .raceMask = dbc->getUInt32(i, 3),
                                       .classMask = dbc->getUInt32(i, 4),
                                       .raceMaskNot = dbc->getUInt32(i, 5),
                                       .classMaskNot = dbc->getUInt32(i, 6)});
    }
    // SkillLine: ID, category, SkillCostsID (+8).
    if (auto dbc = assets->loadDBC("SkillLine.dbc"); dbc && dbc->isLoaded() &&
        dbc->getFieldCount() >= 3) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i)
            skillCostsId_[dbc->getUInt32(i, 0)] = dbc->getUInt32(i, 2);
    }
    // SkillCostsData: ID, SkillCostsID, three costs - grouped by
    // SkillCostsID in file order, the n'th row of a group being rank n.
    if (auto dbc = assets->loadDBC("SkillCostsData.dbc"); dbc && dbc->isLoaded() &&
        dbc->getFieldCount() >= 5) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i)
            costs_[dbc->getUInt32(i, 1)].push_back(
                {dbc->getUInt32(i, 2), dbc->getUInt32(i, 3), dbc->getUInt32(i, 4)});
    }
    // SkillTiers: ID, sixteen costs, sixteen values.
    if (auto dbc = assets->loadDBC("SkillTiers.dbc"); dbc && dbc->isLoaded() &&
        dbc->getFieldCount() >= 17) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            auto& tier = tierCosts_[dbc->getUInt32(i, 0)];
            for (uint32_t t = 0; t < 16; ++t) tier[t] = dbc->getUInt32(i, 1 + t);
        }
    }
}

const SkillUps::RaceClassInfo* SkillUps::raceClassInfo(uint32_t skillId, uint8_t race,
                                                       uint8_t classId) const {
    if (race == 0 || classId == 0) return nullptr;
    const uint32_t raceBit = 1u << ((race - 1u) & 31u);
    const uint32_t classBit = 1u << ((classId - 1u) & 31u);
    for (const auto& info : raceClass_) {
        if (info.skillId != skillId) continue;
        if (info.raceMask != 0 && !(info.raceMask & raceBit)) continue;
        if (info.classMask != 0 && !(info.classMask & classBit)) continue;
        return &info;
    }
    return nullptr;
}

uint32_t SkillUps::abilitySkillLine(uint32_t spellId, uint8_t race, uint8_t classId) const {
    if (race == 0 || classId == 0) return 0;
    const uint32_t raceBit = 1u << ((race - 1u) & 31u);
    const uint32_t classBit = 1u << ((classId - 1u) & 31u);
    // A multimap keeps equal keys in insertion order, which is the file's.
    const auto [lo, hi] = abilities_.equal_range(spellId);
    for (auto it = lo; it != hi; ++it) {
        const Ability& a = it->second;
        if (!raceClassInfo(a.skillId, race, classId)) continue;
        // 0x00810320 inverts a mask whose "not" column is set, and tests the
        // result the same way.
        const uint32_t races = a.raceMaskNot != 0 ? ~a.raceMask : a.raceMask;
        const uint32_t classes = a.classMaskNot != 0 ? ~a.classMask : a.classMask;
        if ((races == 0 || (races & raceBit) != 0) && (classes == 0 || (classes & classBit) != 0))
            return a.skillId;
    }
    return 0;
}

uint32_t SkillUps::rankCost(const RaceClassInfo& info, int32_t rank) const {
    auto line = skillCostsId_.find(info.skillId);
    if (line == skillCostsId_.end() || line->second == 0) return 0;
    auto group = costs_.find(line->second);
    if (group == costs_.end() || rank < 1 || rank > static_cast<int32_t>(group->second.size()) ||
        info.costIndex >= 3)
        return 0;
    return group->second[static_cast<size_t>(rank) - 1][info.costIndex];
}

uint32_t SkillUps::tierCost(const RaceClassInfo& info, uint32_t tier) const {
    if (info.tierId == 0 || tier >= 16) return 0;
    auto it = tierCosts_.find(info.tierId);
    return it == tierCosts_.end() ? 0 : it->second[tier];
}

uint32_t SkillUps::pending(uint32_t skillId) const {
    auto it = pending_.find(skillId);
    return it == pending_.end() ? 0 : it->second;
}

bool SkillUps::add(uint32_t skillId, const RaceClassInfo* info, uint32_t value, uint32_t max,
                   uint32_t points) {
    if (!info || !hasSkillLine(skillId)) return false;
    const uint32_t held = pending(skillId);
    if (held + value >= max) return false;
    const uint32_t cost = rankCost(*info, static_cast<int32_t>(held + 1 + value));
    if (total_ + cost > points) return false;
    pending_[skillId] = held + 1;
    total_ += cost;
    return true;
}

bool SkillUps::remove(uint32_t skillId, const RaceClassInfo* info, uint32_t value) {
    const uint32_t held = pending(skillId);
    if (held == 0 || !info || !hasSkillLine(skillId)) return false;
    const uint32_t cost = rankCost(*info, static_cast<int32_t>(held + value));
    pending_[skillId] = held - 1;
    total_ -= cost;
    return true;
}

std::vector<std::pair<uint32_t, uint32_t>> SkillUps::accept() {
    std::vector<std::pair<uint32_t, uint32_t>> bought;
    if (total_ == 0) return bought;
    for (const auto& [skillId, count] : pending_)
        if (skillId != 0 && count != 0) bought.emplace_back(skillId, count);
    pending_.clear();
    total_ = 0;
    return bought;
}

void SkillUps::cancel() {
    pending_.clear();
    total_ = 0;
}

}  // namespace wowee::game
