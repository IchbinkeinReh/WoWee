#include "addons/lua_arg_guard.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace wowee::addons {

namespace {

bool fits(lua_State* L, int at, char what) {
    switch (what) {
        case 'n': return lua_isnumber(L, at) != 0;
        case 's': return lua_isstring(L, at) != 0;
        case 't': return lua_istable(L, at);
        case 'o': return lua_istable(L, at) || lua_isnoneornil(L, at);
        default: return true;
    }
}

int guarded(lua_State* L) {
    const auto* guard = static_cast<const ArgGuard*>(lua_touserdata(L, lua_upvalueindex(2)));
    for (int i = 0; guard->arguments[i] != 0; ++i)
        if (!fits(L, i + 1, guard->arguments[i])) return luaL_error(L, "%s", guard->usage);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

int answering(lua_State* L) {
    const auto* answer = static_cast<const NothingAnswer*>(lua_touserdata(L, lua_upvalueindex(2)));
    const bool bySpellId = lua_gettop(L) == 1 && lua_isnumber(L, 1);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    const int n = lua_gettop(L);
    if (n > 1 || (n == 1 && lua_toboolean(L, 1))) return n;
    lua_settop(L, 0);
    for (const char* v = answer->values; *v != 0; ++v) {
        switch (*v) {
            case '0': lua_pushnumber(L, 0); break;
            case '1': lua_pushnumber(L, 1); break;
            case 'e': lua_pushstring(L, ""); break;
            case 'f': lua_pushboolean(L, 0); break;
            case 'U': lua_getglobal(L, "UNKNOWN"); break;
            case 'S':
                if (bySpellId) {
                    lua_pushnil(L);
                    lua_pushnil(L);
                }
                break;
            default: lua_pushnil(L); break;
        }
    }
    return lua_gettop(L);
}

constexpr NothingAnswer kNothings[] = {
    {"GetAchievementComparisonInfo", ""},
    {"GetAchievementInfo", ""},
    {"GetAchievementInfoFromCriteria", ""},
    {"GetBattlefieldScore", "n000000nnn00"},
    {"GetBattlefieldStatInfo", "nnn"},
    {"GetBattlefieldVehicleInfo", ""},
    {"GetBattlegroundInfo", ""},
    {"GetChannelRosterInfo", ""},
    {"GetContainerItemInfo", ""},
    {"GetCurrencyListInfo", "nnnnnnnnn"},
    {"GetFriendInfo", "n0UUnenn"},
    {"GetGuildRosterInfo", "nn00nnnnnen"},
    {"GetItemInfo", ""},
    {"GetLFGQueueStats", ""},
    {"GetLootRollItemInfo", ""},
    {"GetMapLandmarkInfo", "nn000nn"},
    {"GetMerchantItemCostItem", "n0"},
    {"GetMerchantItemInfo", "nn0101n"},
    {"GetPetitionInfo", "nnn0nn0"},
    {"GetQuestLogChoiceInfo", "nn10n"},
    {"GetQuestLogQuestText", "nn"},
    {"GetQuestLogRewardInfo", "nn10n"},
    {"GetRaidRosterInfo", "n011nnnnnnnnnn"},
    {"GetRewardSpell", "nnnn"},
    {"GetSavedInstanceInfo", "n0n0ff0f0e"},
    {"GetShapeshiftFormInfo", ""},
    {"GetSpellInfo", ""},
    {"GetSpellLink", "S"},
    {"GetStationeryInfo", "nnn"},
    {"GetTradeSkillInfo", "nn0nn"},
    {"GetTradeSkillReagentInfo", "nnnn"},
    {"HasPetSpells", "nn"},
};

constexpr ArgGuard kGuards[] = {
    {"BNGetConversationInfo", "n", "Usage: BNGetConversationInfo(channel)"},
    {"BNGetConversationMemberInfo", "nn", "Usage: BNGetConversationMemberInfo(channel,index)"},
    {"BNGetCustomMessageTable", "t", "Usage: BNGetCustomMessageTable(table)"},
    {"BNGetFriendInfo", "n", "Usage: BNGetFriendInfo(index)"},
    {"BNGetFriendInfoByID", "n", "Usage: BNGetFriendInfoByID(ID)"},
    {"BNGetFriendToonInfo", "nn", "Usage: BNGetToonInfo(friendIndex,toonIndex)"},
    {"BNGetNumConversationMembers", "n", "Usage: BNGetNumConversationMembers(channel)"},
    {"BNGetNumFOF", "n", "Usage: BNGetNumFOF(ID,mutual,non)"},
    {"BNGetNumFriendToons", "n", "Usage: BNGetNumFriendToons(index)"},
    {"BNGetToonInfo", "n", "Usage: BNGetToonInfo(ID)"},
    {"CalendarEventGetInvite", "n", "Usage: CalendarEventGetInvite(index)"},
    {"CalendarGetDayEvent", "nnn", "Usage: CalendarGetDayEvent([-1.0.1], monthDay, index)"},
    {"CalendarGetHolidayInfo", "nnn", "Usage: CalendarGetHolidayInfo([-1,0,1], monthDay, index)"},
    {"CanComplainInboxItem", "n", "Usage: InboxItemCanComplain(index)"},
    {"CanInspect", "s", "Usage: CanInspect(unit [, showError])"},
    {"CancelAuction", "n", "Usage: SetSelectedAuctionItem(index)"},
    {"GetAchievementComparisonInfo", "n", "Usage: GetAchievementComparisonInfo(achievementID)"},
    {"GetAchievementInfo", "n", "Usage: GetAchievementInfo(achievementID)"},
    {"GetAchievementInfoFromCriteria", "n", "Usage: GetAchievementInfoFromCriteria(criteriaID)"},
    {"GetActionCooldown", "n", "Usage: GetActionCooldown(slot)"},
    {"GetAddOnInfo", "s", "Usage: GetAddOnInfo(index or \"name\")"},
    {"GetAttackPowerForStat", "nn", "Usage: Script_GetAttackPowerForStat(stat, value)"},
    {"GetAuctionItemInfo", "sn", "Usage: GetAuctionItemInfo(\"type\", index)"},
    {"GetBackpackCurrencyInfo", "n", "Usage: GetBackpackCurrencyInfo(index)"},
    {"GetBattlefieldScore", "n", "Usage: GetBattlefieldScore(index)"},
    {"GetBattlefieldVehicleInfo", "n", "Usage: GetBattlefieldVehicleInfo(index)"},
    {"GetBattlegroundInfo", "n", "Usage: GetBattlegroundInfo(index)"},
    {"GetBinding", "n", "Usage: GetBinding(index[, mode])"},
    {"GetBuybackItemInfo", "n", "Usage: GetBuybackItemInfo(slot)"},
    {"GetCategoryNumAchievements", "n", "Usage: GetCategoryNumAchievements(categoryID)"},
    {"GetChannelName", "s", "Usage: GetChannelName([channelIndex] or [channelName])"},
    {"GetComboPoints", "s", "Usage: GetComboPoints(\"unit\"[, \"target\"]"},
    {"GetContainerItemInfo", "nn", "Usage: GetContainerItemInfo(index, slot)"},
    {"GetContainerNumFreeSlots", "n", "Usage: GetContainerFreeSlots(index)"},
    {"GetContainerNumSlots", "n", "Usage: GetContainerNumSlots(index)"},
    {"GetCurrencyListInfo", "n", "Usage: GetCurrencyListInfo(index)"},
    {"GetFriendInfo", "s", "Usage: GetFriendInfo(index or name)"},
    {"GetGlyphLink", "n", "Usage: GetGlyphLink(index[, talentGroup])"},
    {"GetGuildRosterInfo", "n", "Usage: GetGuildRosterInfo(index)"},
    {"GetIgnoreName", "n", "Usage: GetIgnoreName(index)"},
    {"GetInspectArenaTeamData", "n", "Usage: GetInspectArenaTeamData(index)"},
    {"GetItemInfo", "s", "Usage: GetItemInfo(itemID|\"name\"|\"itemlink\")"},
    {"GetLFDChoiceCollapseState", "o", "Usage: GetLFDChoiceCollapseState([table])"},
    {"GetLFDChoiceEnabledState", "o", "Usage: GetLFDChoiceEnabledState([table])"},
    {"GetLFDChoiceInfo", "o", "Usage: GetLFDChoiceInfo([table])"},
    {"GetLFDChoiceOrder", "o", "Usage: GetLFDChoiceOrder([table])"},
    {"GetLFGDungeonRewardInfo", "nn", "Usage: GetLFGDungeonRewardInfo(dungeonID, lootIndex)"},
    {"GetLFRChoiceOrder", "o", "Usage: GetLFRChoiceOrder([table])"},
    {"GetLanguageByIndex", "n", "Usage: GetLanguageByIndex(index)"},
    {"GetLootRollItemInfo", "n", "Usage: GetLootRollItemInfo(id)"},
    {"GetMasterLootCandidate", "n", "Usage: GetMasterLootCandidate(index)"},
    {"GetMuteName", "n", "Usage: GetMuteName(index)"},
    {"GetNextCompleatedTutorial", "n", "Usage: GetNextCompleatedTutorial(\"tutorial\")"},
    {"GetPetActionCooldown", "n", "Usage: GetPetActionCooldown(index)"},
    {"GetPetActionSlotUsable", "n", "Usage: GetPetActionSlotsUsable(index)"},
    {"GetPetitionNameInfo", "n", "Usage: GetPetitionNameInfo(index)"},
    {"GetPlayerInfoByGUID", "s", "Usage: GetPlayerInfoByGUID(\"playerGUID\")"},
    {"GetPrevCompleatedTutorial", "n", "Usage: GetPrevCompleatedTutorial(\"tutorial\")"},
    {"GetQuestLogChoiceInfo", "n", "Usage: GetQuestLogRewardInfo(index)"},
    {"GetQuestLogItemDrop", "n", "Usage: GetQuestLogItemDrop(index)"},
    {"GetQuestLogRewardInfo", "n", "Usage: GetQuestLogRewardInfo(index)"},
    {"GetRaidRosterInfo", "n", "Usage: GetRaidRosterInfo(index)"},
    {"GetRuneCooldown", "n", "Usage: GetRuneCooldown(slot)"},
    {"GetSavedInstanceInfo", "n", "Usage: GetSavedInstanceInfo(index)"},
    {"GetSkillLineInfo", "n", "Usage: GetSkillLineInfo(index)"},
    {"GetStationeryInfo", "n", "Usage: GetStationeryInfo(index)"},
    {"GetTalentInfo", "n", "Usage: GetTalentInfo(tabIndex, talentIndex[, isInspect[, isPet[, groupIndex]]])"},
    {"GetTalentLink", "nn", "Usage: GetTalentLink(tabIndex, talentIndex[, isInspect[, isPet[, groupIndex[, isPreview]]]])"},
    {"GetTalentPrereqs", "nn", "Usage: GetTalentPrereqs(tabIndex, talentIndex[, isInspect[, isPet[, groupIndex]]])"},
    {"GetTradePlayerItemInfo", "n", "Usage: GetTradePlayerItemInfo(index)"},
    {"GetTradeSkillCooldown", "n", "Usage: GetTradeSkillCooldown(index)"},
    {"GetTradeSkillInfo", "n", "Usage: GetTradeSkillInfo(index)"},
    {"GetTradeSkillReagentInfo", "nn", "Usage: GetTradeSkillReagentInfo(index, reagentIndex)"},
    {"GetTradeSkillTools", "n", "Usage: GetTradeSkillTools(index)"},
    {"GetTradeTargetItemInfo", "n", "Usage: GetTradeTargetItemInfo(index)"},
    {"GuildControlSaveRank", "s", "Usage: GuildControlSaveRank(name)"},
    {"IsAddOnLoaded", "s", "Usage: IsAddOnLoaded(index or \"name\")"},
    {"IsAutoRepeatAction", "n", "Usage: IsAutorepeatAction(slot)"},
    {"IsCurrentAction", "n", "Usage: IsCurrentAction(slot)"},
    {"IsTitleKnown", "n", "Usage: IsTitleKnown(titleMaskID)"},
    {"IsTutorialFlagged", "n", "Usage: Trigger(\"tutorial\")"},
    {"IsUsableAction", "n", "Usage: IsUsableAction(slot)"},
    {"SpellCanTargetUnit", "s", "Usage: SpellCanTargetUnit(\"unit\")"},
    {"UnitArmor", "s", "Usage: UnitArmor(\"unit\")"},
    {"UnitCanCooperate", "s", "Usage: UnitCanCooperate(\"unit\", \"otherUnit\")"},
    {"UnitCharacterPoints", "s", "Usage: UnitCharacterPoints(\"unit\")"},
    {"UnitClass", "s", "Usage: UnitClass(\"unit\")"},
    {"UnitClassBase", "s", "Usage: UnitClassBase(\"unit\")"},
    {"UnitCreatureFamily", "s", "Usage: UnitCreatureFamily(\"unit\")"},
    {"UnitCreatureType", "s", "Usage: UnitCreatureType(\"unit\")"},
    {"UnitFactionGroup", "s", "Usage: UnitFactionGroup(\"unit\")"},
    {"UnitHasLFGDeserter", "s", "Usage: UnitHasLFGDeserter(\"unit\")"},
    {"UnitHasLFGRandomCooldown", "s", "Usage: UnitHasLFGRandomCooldown(\"unit\")"},
    {"UnitHealth", "s", "Usage: UnitHealth(\"unit\")"},
    {"UnitInRange", "s", "Usage: UnitInRange(\"unit\")"},
    {"UnitInVehicle", "s", "Usage: UnitInVehicle(\"unit\")"},
    {"UnitIsDead", "s", "Usage: UnitIsDead(\"unit\")"},
    {"UnitIsFriend", "s", "Usage: UnitIsFriend(\"unit\", \"otherUnit\")"},
    {"UnitIsGhost", "s", "Usage: UnitIsGhost(\"unit\")"},
    {"UnitIsSameServer", "ss", "Usage: UnitIsSameServer(\"unit\", \"otherUnit\")"},
    {"UnitLevel", "s", "Usage: UnitLevel(\"unit\")"},
    {"UnitMana", "s", "Usage: UnitPower(\"unit\"[, type])"},
    {"UnitManaMax", "s", "Usage: UnitPowerMax(\"unit\"[, type])"},
    {"UnitName", "s", "Usage: UnitName(\"unit\")"},
    {"UnitPower", "s", "Usage: UnitPower(\"unit\"[, type])"},
    {"UnitPowerMax", "s", "Usage: UnitPowerMax(\"unit\"[, type])"},
    {"UnitPowerType", "s", "Usage: UnitPowerType(\"unit\"[, index])"},
    {"UnitRace", "s", "Usage: UnitRace(\"unit\")"},
    {"UnitRangedDamage", "s", "Usage: UnitRangedDamage(\"unit\")"},
    {"UnitResistance", "sn", "Usage: UnitResistance(\"unit\", resistanceIndex)"},
    {"UnitSelectionColor", "s", "Usage: UnitSelectionColor(\"unit\")"},
    {"UnitSex", "s", "Usage: UnitSex(\"unit\")"},
    {"UnitStat", "sn", "Usage: UnitStat(\"unit\", statIndex)"},
    {"UnitUsingVehicle", "s", "Usage: UnitUsingVehicle(\"unit\")"},
};

}  // namespace

std::span<const ArgGuard> argGuards() { return kGuards; }

std::span<const NothingAnswer> nothingAnswers() { return kNothings; }

void answerNothing(lua_State* L, std::span<const NothingAnswer> answers) {
    for (const NothingAnswer& answer : answers) {
        lua_getglobal(L, answer.name);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        lua_pushlightuserdata(L, const_cast<NothingAnswer*>(&answer));
        lua_pushcclosure(L, answering, 2);
        lua_setglobal(L, answer.name);
    }
}

void guardArguments(lua_State* L, std::span<const ArgGuard> guards) {
    for (const ArgGuard& guard : guards) {
        lua_getglobal(L, guard.name);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        lua_pushlightuserdata(L, const_cast<ArgGuard*>(&guard));
        lua_pushcclosure(L, guarded, 2);
        lua_setglobal(L, guard.name);
    }
}

}  // namespace wowee::addons
