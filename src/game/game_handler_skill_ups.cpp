// game_handler_skill_ups.cpp - GameHandler's side of the skill-point
// purchase (SkillUps): the player's skills, points and the wire.
#include "game/game_handler.hpp"
#include "game/skill_ups.hpp"
#include "game/update_field_table.hpp"
#include "network/world_socket.hpp"

namespace wowee::game {

const SkillUps::RaceClassInfo* GameHandler::skillRaceClassInfo(uint32_t skillId) {
    skillUps_.load(services_.assetManager);
    return skillUps_.raceClassInfo(skillId, getPlayerRace(), getPlayerClass());
}

uint32_t GameHandler::spellSkillLine(uint32_t spellId) {
    skillUps_.load(services_.assetManager);
    return skillUps_.abilitySkillLine(spellId, getPlayerRace(), getPlayerClass());
}

uint32_t GameHandler::getSkillPoints() const {
    const auto self = entityController_->getEntityManager().getEntity(playerGuid);
    return self ? self->getField(fieldIndex(UF::PLAYER_CHARACTER_POINTS2)) : 0;
}

int32_t GameHandler::getAdjustedSkillPoints() const {
    return static_cast<int32_t>(getSkillPoints()) - static_cast<int32_t>(skillUps_.pendingCost());
}

GameHandler::SkillPurchaseInfo GameHandler::getSkillPurchaseInfo(uint32_t skillId) {
    SkillPurchaseInfo out;
    auto it = playerSkills_.find(skillId);
    const auto* info = skillRaceClassInfo(skillId);
    if (it == playerSkills_.end() || !info) return out;
    const PlayerSkill& skill = it->second;
    out.pending = skillUps_.pending(skillId);
    // A tier for sale where 0x8 says so, or 0x4 on a skill not yet begun.
    if ((info->flags & 0x8) || (skill.step == 0 && (info->flags & 0x4)))
        out.stepCost = skillUps_.tierCost(*info, skill.step);
    // The value 0x0051a250 reads, with its permanent bonus.
    const uint32_t value = skill.value ? skill.value + skill.bonusPerm : 0u;
    out.rankCost = skillUps_.rankCost(*info, static_cast<int32_t>(value + 1 + out.pending));
    out.minLevel = info->minLevel;
    out.costType = info->costIndex + 1;
    out.heldAtOne = (info->flags & 0x400) != 0;
    out.abandonable = skill.step != 0 && (info->flags & 0x20) != 0;
    return out;
}

void GameHandler::addSkillUp(uint32_t skillId) {
    auto it = playerSkills_.find(skillId);
    if (it == playerSkills_.end()) return;
    const PlayerSkill& skill = it->second;
    const uint32_t value = skill.value ? skill.value + skill.bonusPerm : 0u;
    const uint32_t max = skill.maxValue ? skill.maxValue + skill.bonusPerm : 0u;
    if (skillUps_.add(skillId, skillRaceClassInfo(skillId), value, max, getSkillPoints()))
        fireAddonEvent("SKILL_LINES_CHANGED", {});
}

void GameHandler::removeSkillUp(uint32_t skillId) {
    auto it = playerSkills_.find(skillId);
    if (it == playerSkills_.end()) return;
    const PlayerSkill& skill = it->second;
    const uint32_t value = skill.value ? skill.value + skill.bonusPerm : 0u;
    if (skillUps_.remove(skillId, skillRaceClassInfo(skillId), value))
        fireAddonEvent("SKILL_LINES_CHANGED", {});
}

void GameHandler::acceptSkillUps() {
    const auto bought = skillUps_.accept();
    if (bought.empty() || !socket) return;
    // CMSG_SKILL_BUY_RANK: a count, then each skill and its ranks.
    network::Packet packet(wireOpcode(Opcode::CMSG_SKILL_BUY_RANK));
    packet.writeUInt32(static_cast<uint32_t>(bought.size()));
    for (const auto& [id, count] : bought) {
        packet.writeUInt32(id);
        packet.writeUInt32(count);
    }
    socket->send(packet);
}

void GameHandler::cancelSkillUps() {
    skillUps_.cancel();
    fireAddonEvent("SKILL_LINES_CHANGED", {});
}

void GameHandler::buySkillTier(uint32_t skillId) {
    auto it = playerSkills_.find(skillId);
    const auto* info = skillRaceClassInfo(skillId);
    if (it == playerSkills_.end() || !info || !skillUps_.hasSkillLine(skillId) || !socket) return;
    const uint16_t step = it->second.step;
    if (!((info->flags & 0x8) || (step == 0 && (info->flags & 0x4)))) return;
    const uint32_t cost = skillUps_.tierCost(*info, step);
    if (cost == 0 || cost > getSkillPoints()) return;
    // CMSG_SKILL_BUY_STEP: the skill.
    network::Packet packet(wireOpcode(Opcode::CMSG_SKILL_BUY_STEP));
    packet.writeUInt32(skillId);
    socket->send(packet);
}

}  // namespace wowee::game
