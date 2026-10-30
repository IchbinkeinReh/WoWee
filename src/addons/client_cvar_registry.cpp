#include "addons/client_cvar_registry.hpp"

#include <cctype>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

namespace wowee::addons {

namespace {

struct ClientCVar {
    const char* name;
    uint32_t flags;
};

// Every CVar the client registers by name (0x00767fc0 with a literal name),
// with the flags it registers it with, in case-insensitive order. Read out of
// the 3.3.5a build 12340 binary; the names built in a loop are matched by
// clientCVarFlags below instead.
constexpr ClientCVar kClientCVars[] = {
    {"accountList", 0x0},
    {"accountName", 0x40},
    {"accounttype", 0x0},
    {"addFriendInfoShown", 0x10},
    {"advancedWatchFrame", 0x10},
    {"advancedWorldMap", 0x20},
    {"alwaysCompareItems", 0x10},
    {"alwaysShowActionBars", 0x10},
    {"assistAttack", 0x20},
    {"asyncHandlerTimeout", 0x1},
    {"asyncThreadSleep", 0x1},
    {"auctionDisplayOnCharacter", 0x10},
    {"autoClearAFK", 0x10},
    {"autoCompleteResortNamesOnRecency", 0x10},
    {"autoCompleteUseContext", 0x10},
    {"autoCompleteWhenEditingFromCenter", 0x10},
    {"autoDismount", 0x10},
    {"autoDismountFlying", 0x10},
    {"autoFilledMultiCastSlots", 0x20},
    {"autoInteract", 0x10},
    {"autojoinBGVoice", 0x10},
    {"autojoinPartyVoice", 0x10},
    {"autoLootDefault", 0x10},
    {"autoQuestProgress", 0x10},
    {"autoQuestWatch", 0x10},
    {"autoRangedCombat", 0x10},
    {"autoSelfCast", 0x20},
    {"autoStand", 0x10},
    {"autoUnshift", 0x10},
    {"baseMip", 0x1},
    {"blockTrades", 0x20},
    {"bspcache", 0x1},
    {"buffDurations", 0x10},
    {"calendarShowBattlegrounds", 0x20},
    {"calendarShowDarkmoon", 0x20},
    {"calendarShowLockouts", 0x20},
    {"calendarShowResets", 0x20},
    {"calendarShowWeeklyHolidays", 0x20},
    {"cameraBobbing", 0x0},
    {"cameraBobbingSmoothSpeed", 0x10},
    {"cameraCustomViewSmoothing", 0x10},
    {"cameraDistanceMax", 0x10},
    {"cameraDistanceMaxFactor", 0x10},
    {"cameraDistanceMoveSpeed", 0x10},
    {"cameraDistanceSmoothSpeed", 0x10},
    {"cameraDive", 0x10},
    {"cameraFlyingMountHeightSmoothSpeed", 0x10},
    {"cameraFoVSmoothSpeed", 0x10},
    {"cameraGroundSmoothSpeed", 0x10},
    {"cameraHeightIgnoreStandState", 0x10},
    {"cameraHeightSmoothSpeed", 0x10},
    {"cameraPitchMoveSpeed", 0x10},
    {"cameraPitchSmoothMax", 0x10},
    {"cameraPitchSmoothMin", 0x10},
    {"cameraPitchSmoothSpeed", 0x10},
    {"cameraPivot", 0x10},
    {"cameraPivotDXMax", 0x10},
    {"cameraPivotDYMin", 0x10},
    {"cameraSavedDistance", 0x20},
    {"cameraSavedPitch", 0x20},
    {"cameraSavedVehicleDistance", 0x20},
    {"camerasmooth", 0x10},
    {"cameraSmoothPitch", 0x10},
    {"cameraSmoothStyle", 0x10},
    {"cameraSmoothTimeMax", 0x10},
    {"cameraSmoothTimeMin", 0x10},
    {"cameraSmoothTrackingStyle", 0x10},
    {"cameraSmoothYaw", 0x10},
    {"cameraSubmergeFinalPitch", 0x10},
    {"cameraSubmergePitch", 0x10},
    {"cameraSurfaceFinalPitch", 0x10},
    {"cameraSurfacePitch", 0x10},
    {"cameraTargetSmoothSpeed", 0x10},
    {"cameraTerrainTilt", 0x10},
    {"cameraTerrainTiltTimeMax", 0x10},
    {"cameraTerrainTiltTimeMin", 0x10},
    {"cameraView", 0x10},
    {"cameraViewBlendStyle", 0x10},
    {"cameraWaterCollision", 0x10},
    {"cameraYawMoveSpeed", 0x10},
    {"cameraYawSmoothMax", 0x10},
    {"cameraYawSmoothMin", 0x10},
    {"cameraYawSmoothSpeed", 0x10},
    {"ChatAmbienceVolume", 0x0},
    {"chatBubbles", 0x10},
    {"chatBubblesParty", 0x10},
    {"chatMouseScroll", 0x10},
    {"ChatMusicVolume", 0x0},
    {"ChatSoundVolume", 0x0},
    {"chatStyle", 0x10},
    {"checkAddonVersion", 0x0},
    {"CinematicJoystick", 0x0},
    {"colorblindMode", 0x10},
    {"colorChatNamesByClass", 0x10},
    {"CombatDamage", 0x10},
    {"CombatHealing", 0x10},
    {"combatLogOn", 0x20},
    {"CombatLogPeriodicSpells", 0x10},
    {"combatLogRetentionTime", 0x10},
    {"combatTextFloatMode", 0x10},
    {"componentCompress", 0x1},
    {"componentTextureLevel", 0x1},
    {"componentThread", 0x1},
    {"consolidateBuffs", 0x20},
    {"conversationMode", 0x10},
    {"converted", 0x0},
    {"currencyTokensBackpack1", 0x20},
    {"currencyTokensBackpack2", 0x20},
    {"currencyTokensUnused1", 0x20},
    {"currencyTokensUnused2", 0x20},
    {"dbCompress", 0x0},
    {"decorateAccountName", 0x0},
    {"deselectOnClick", 0x10},
    {"DesktopGamma", 0x0},
    {"displayFreeBagSlots", 0x10},
    {"displayWorldPVPObjectives", 0x10},
    {"dontShowEquipmentSetsOnItems", 0x10},
    {"enableCombatText", 0x10},
    {"EnableMicrophone", 0x0},
    {"enablePVPNotifyAFK", 0x10},
    {"EnableVoiceChat", 0x0},
    {"enableWowMouse", 0x1},
    {"environmentDetail", 0x1},
    {"equipmentManager", 0x20},
    {"ErrorFilter", 0x0},
    {"ErrorLevelMax", 0x0},
    {"ErrorLevelMin", 0x0},
    {"Errors", 0x0},
    {"expansionMovie", 0x0},
    {"extShadowQuality", 0x1},
    {"farclip", 0x1},
    {"farClipOverride", 0x1},
    {"fctAllSpellMechanics", 0x10},
    {"fctAuras", 0x10},
    {"fctCombatState", 0x10},
    {"fctComboPoints", 0x10},
    {"fctDamageReduction", 0x10},
    {"fctDodgeParryMiss", 0x10},
    {"fctEnergyGains", 0x10},
    {"fctFriendlyHealers", 0x10},
    {"fctHonorGains", 0x10},
    {"fctLowManaHealth", 0x10},
    {"fctPeriodicEnergyGains", 0x10},
    {"fctReactives", 0x10},
    {"fctRepChanges", 0x10},
    {"fctSpellMechanics", 0x10},
    {"fctSpellMechanicsOther", 0x10},
    {"ffx", 0x1},
    {"ffxDeath", 0x1},
    {"ffxGlow", 0x1},
    {"ffxNetherWorld", 0x1},
    {"ffxRectangle", 0x1},
    {"ffxSpecial", 0x1},
    {"fixedFunction", 0x3},
    {"flaggedTutorials", 0x110},
    {"footstepBias", 0x1},
    {"FootstepSounds", 0x0},
    {"forceEnglishNames", 0x0},
    {"friendsSmallView", 0x20},
    {"friendsViewButtons", 0x20},
    {"fullSizeFocusFrame", 0x20},
    {"g_accountUsesToken", 0x0},
    {"gameTip", 0x0},
    {"Gamma", 0x0},
    {"groundEffectDensity", 0x1},
    {"groundEffectDist", 0x1},
    {"guildMemberNotify", 0x10},
    {"guildRecruitmentChannel", 0x10},
    {"guildShowOffline", 0x10},
    {"gxApi", 0x3},
    {"gxAspect", 0x3},
    {"gxColorBits", 0x3},
    {"gxCursor", 0x3},
    {"gxDepthBits", 0x3},
    {"gxFixLag", 0x3},
    {"gxMaximize", 0x3},
    {"gxMultisample", 0x3},
    {"gxMultisampleQuality", 0x3},
    {"gxOverride", 0x1},
    {"gxRefresh", 0x3},
    {"gxResolution", 0x3},
    {"gxStereoConvergence", 0x1},
    {"gxStereoEnabled", 0x1},
    {"gxStereoSeparation", 0x1},
    {"gxTextureCacheSize", 0x1},
    {"gxTripleBuffer", 0x3},
    {"gxVSync", 0x3},
    {"gxWindow", 0x3},
    {"heapAllocTracking", 0x0},
    {"hidePartyInRaid", 0x10},
    {"horizonFarclipScale", 0x1},
    {"horizonNearclipScale", 0x1},
    {"hwDetect", 0x1},
    {"hwPCF", 0x1},
    {"InboundChatVolume", 0x0},
    {"Joystick", 0x0},
    {"lastCharacterIndex", 0x0},
    {"lastTalkedToGM", 0x10},
    {"lfdCollapsedHeaders", 0x120},
    {"lfdSelectedDungeons", 0x120},
    {"lfgAutoFill", 0x10},
    {"lfgAutoJoin", 0x10},
    {"lfgSelectedRoles", 0x120},
    {"locale", 0x0},
    {"lockActionBars", 0x10},
    {"lod", 0x1},
    {"lootUnderMouse", 0x10},
    {"M2BatchDoodads", 0x0},
    {"M2BatchParticles", 0x0},
    {"M2Faster", 0x0},
    {"M2FasterDebug", 0x0},
    {"M2ForceAdditiveParticleSort", 0x0},
    {"M2UseClipPlanes", 0x0},
    {"M2UseThreads", 0x0},
    {"M2UseZFill", 0x0},
    {"mapObjLightLOD", 0x1},
    {"mapQuestDifficulty", 0x20},
    {"mapShadows", 0x1},
    {"maxFPS", 0x1},
    {"maxFPSBk", 0x1},
    {"MaxLights", 0x1},
    {"minimapInsideZoom", 0x20},
    {"minimapPortalMax", 0x20},
    {"minimapTrackedInfo", 0x20},
    {"minimapZoom", 0x20},
    {"miniWorldMap", 0x20},
    {"mouseInvertPitch", 0x10},
    {"mouseInvertYaw", 0x10},
    {"mouseSpeed", 0x0},
    {"movie", 0x0},
    {"movieSubtitle", 0x0},
    {"nameplateAllowOverlap", 0x20},
    {"nameplateShowEnemies", 0x20},
    {"nameplateShowEnemyGuardians", 0x20},
    {"nameplateShowEnemyPets", 0x20},
    {"nameplateShowEnemyTotems", 0x20},
    {"nameplateShowFriendlyGuardians", 0x20},
    {"nameplateShowFriendlyPets", 0x20},
    {"nameplateShowFriendlyTotems", 0x20},
    {"nameplateShowFriends", 0x20},
    {"nearclip", 0x1},
    {"objectFade", 0x1},
    {"objectFadeZFill", 0x1},
    {"ObjectSelectionCircle", 0x0},
    {"occlusion", 0x1},
    {"OutboundChatVolume", 0x0},
    {"particleDensity", 0x1},
    {"partyBackgroundOpacity", 0x10},
    {"partyStatusText", 0x10},
    {"pathDistTol", 0x0},
    {"pendingInviteInfoShown", 0x10},
    {"PetMeleeDamage", 0x10},
    {"PetSpellDamage", 0x10},
    {"petStatusText", 0x10},
    {"playerStatLeftDropdown", 0x20},
    {"playerStatRightDropdown", 0x20},
    {"playerStatusText", 0x10},
    {"POIShiftComplete", 0x0},
    {"portal", 0x0},
    {"predictedHealth", 0x10},
    {"predictedPower", 0x10},
    {"previewTalents", 0x20},
    {"processAffinityMask", 0x2},
    {"profanityFilter", 0x10},
    {"projectedTextures", 0x1},
    {"PushToTalkButton", 0x0},
    {"PushToTalkSound", 0x10},
    {"questFadingDisable", 0x10},
    {"questLogCollapseFilter", 0x20},
    {"questPOI", 0x20},
    {"readContest", 0x0},
    {"readEULA", 0x0},
    {"readScanning", 0x0},
    {"readTerminationWithoutNotice", 0x0},
    {"readTOS", 0x0},
    {"realmList", 0x0},
    {"realmListbn", 0x0},
    {"realmName", 0x0},
    {"removeChatDelay", 0x10},
    {"rotateMinimap", 0x10},
    {"screenEdgeFlash", 0x10},
    {"screenshotFormat", 0x1},
    {"screenshotQuality", 0x1},
    {"scriptErrors", 0x10},
    {"scriptProfile", 0x0},
    {"secureAbilityToggle", 0x10},
    {"serverAlert", 0x0},
    {"serviceTypeFilter", 0x10},
    {"shadowCull", 0x0},
    {"shadowInstancing", 0x0},
    {"shadowLevel", 0x1},
    {"shadowLOD", 0x1},
    {"shadowScissor", 0x0},
    {"ShowAllSpellRanks", 0x10},
    {"showArenaEnemyCastbar", 0x20},
    {"showArenaEnemyFrames", 0x20},
    {"showArenaEnemyPets", 0x20},
    {"showBattlefieldMinimap", 0x20},
    {"showCastableBuffs", 0x20},
    {"showCastableDebuffs", 0x20},
    {"ShowClassColorInNameplate", 0x20},
    {"showClock", 0x10},
    {"showDispelDebuffs", 0x20},
    {"ShowErrors", 0x0},
    {"showfootprintparticles", 0x1},
    {"showfootprints", 0x1},
    {"showGameTips", 0x0},
    {"showItemLevel", 0x10},
    {"showKeyring", 0x20},
    {"showLootSpam", 0x10},
    {"showNewbieTips", 0x10},
    {"showPartyBackground", 0x10},
    {"showPartyPets", 0x20},
    {"showQuestObjectivesOnMap", 0x20},
    {"showQuestTrackingTooltips", 0x20},
    {"showRaidRange", 0x20},
    {"showTargetCastbar", 0x10},
    {"showTargetOfTarget", 0x10},
    {"showTimestamps", 0x10},
    {"showToastBroadcast", 0x10},
    {"showToastConversation", 0x10},
    {"showToastFriendRequest", 0x10},
    {"showToastOffline", 0x10},
    {"showToastOnline", 0x10},
    {"showToastWindow", 0x10},
    {"showTokenFrame", 0x20},
    {"showTokenFrameHonor", 0x20},
    {"showToolsUI", 0x0},
    {"showTutorials", 0x10},
    {"showVKeyCastbar", 0x10},
    {"SkyCloudLOD", 0x0},
    {"Sound_AmbienceVolume", 0x0},
    {"Sound_ChaosMode", 0x0},
    {"Sound_DSPBufferSize", 0x0},
    {"Sound_EnableAllSound", 0x0},
    {"Sound_EnableAmbience", 0x0},
    {"Sound_EnableArmorFoleySoundForOthers", 0x0},
    {"Sound_EnableArmorFoleySoundForSelf", 0x0},
    {"Sound_EnableDSPEffects", 0x0},
    {"Sound_EnableEmoteSounds", 0x0},
    {"Sound_EnableErrorSpeech", 0x0},
    {"Sound_EnableHardware", 0x0},
    {"Sound_EnableMixMode2", 0x0},
    {"Sound_EnableMode2", 0x0},
    {"Sound_EnableMusic", 0x0},
    {"Sound_EnablePetSounds", 0x0},
    {"Sound_EnableReverb", 0x0},
    {"Sound_EnableSFX", 0x0},
    {"Sound_EnableSoftwareHRTF", 0x0},
    {"Sound_EnableSoundWhenGameIsInBG", 0x0},
    {"Sound_ListenerAtCharacter", 0x0},
    {"Sound_MasterVolume", 0x0},
    {"Sound_MaxCacheableSizeInBytes", 0x0},
    {"Sound_MaxCacheSizeInBytes", 0x0},
    {"Sound_MusicVolume", 0x0},
    {"Sound_NumChannels", 0x0},
    {"Sound_OutputDriverIndex", 0x0},
    {"Sound_OutputDriverName", 0x0},
    {"Sound_OutputQuality", 0x0},
    {"Sound_SFXVolume", 0x0},
    {"Sound_VoiceChatInputDriverIndex", 0x0},
    {"Sound_VoiceChatInputDriverName", 0x0},
    {"Sound_VoiceChatOutputDriverIndex", 0x0},
    {"Sound_VoiceChatOutputDriverName", 0x0},
    {"Sound_ZoneMusicNoDelay", 0x0},
    {"SoundMemoryCache", 0x2},
    {"spamFilter", 0x10},
    {"specular", 0x1},
    {"spellEffectLevel", 0x1},
    {"SplineOpt", 0x1},
    {"StartTalkingDelay", 0x0},
    {"StartTalkingTime", 0x0},
    {"statusTextPercentage", 0x10},
    {"stopAutoAttackOnTargetChange", 0x20},
    {"StopTalkingDelay", 0x0},
    {"StopTalkingTime", 0x0},
    {"synchronizeBindings", 0x0},
    {"synchronizeConfig", 0x0},
    {"synchronizeMacros", 0x0},
    {"synchronizeSettings", 0x0},
    {"taintLog", 0x0},
    {"talentFrameShown", 0x10},
    {"targetOfTargetMode", 0x10},
    {"targetStatusText", 0x10},
    {"terrainAlphaBitDepth", 0x1},
    {"texLodBias", 0x1},
    {"threatPlaySounds", 0x10},
    {"threatShowNumeric", 0x10},
    {"threatWarning", 0x20},
    {"threatWorldText", 0x10},
    {"timeMgrAlarmEnabled", 0x10},
    {"timeMgrAlarmMessage", 0x10},
    {"timeMgrAlarmTime", 0x10},
    {"timeMgrUseLocalTime", 0x10},
    {"timeMgrUseMilitaryTime", 0x10},
    {"timingMethod", 0x2},
    {"timingTestError", 0x6},
    {"toastDuration", 0x10},
    {"trackedAchievements", 0x120},
    {"trackedQuests", 0x120},
    {"trackerFilter", 0x20},
    {"trackerSorting", 0x20},
    {"UberTooltips", 0x10},
    {"uiScale", 0x0},
    {"unitHighlights", 0x10},
    {"UnitNameEnemyGuardianName", 0x10},
    {"UnitNameEnemyPetName", 0x10},
    {"UnitNameEnemyPlayerName", 0x10},
    {"UnitNameEnemyTotemName", 0x10},
    {"UnitNameFriendlyGuardianName", 0x10},
    {"UnitNameFriendlyPetName", 0x10},
    {"UnitNameFriendlyPlayerName", 0x10},
    {"UnitNameFriendlyTotemName", 0x10},
    {"UnitNameNonCombatCreatureName", 0x10},
    {"UnitNameNPC", 0x10},
    {"UnitNameOwn", 0x10},
    {"UnitNamePlayerGuild", 0x10},
    {"UnitNamePlayerPVPTitle", 0x10},
    {"useEnglishAudio", 0x0},
    {"useUiScale", 0x0},
    {"useWeatherShaders", 0x0},
    {"videoOptionsVersion", 0x3},
    {"violenceLevel", 0x10},
    {"VoiceActivationSensitivity", 0x0},
    {"VoiceChatMode", 0x0},
    {"VoiceChatSelfMute", 0x0},
    {"watchFrameBaseAlpha", 0x10},
    {"watchFrameIgnoreCursor", 0x10},
    {"watchFrameState", 0x10},
    {"watchFrameWidth", 0x0},
    {"waterLOD", 0x1},
    {"weatherDensity", 0x0},
    {"wholeChatWindowClickable", 0x10},
    {"widescreen", 0x0},
    {"windowResizeLock", 0x1},
    {"worldMapOpacity", 0x20},
    {"worldPoolUsage", 0x1},
    {"xpBarText", 0x10},
};

bool equalsNoCase(std::string_view a, const char* b) {
    const size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

/// Strip one of `parts` off the front of `rest`, case-insensitively.
bool takePart(std::string_view& rest, std::initializer_list<const char*> parts) {
    for (const char* part : parts) {
        const size_t n = std::strlen(part);
        if (rest.size() >= n && equalsNoCase(rest.substr(0, n), part)) {
            rest.remove_prefix(n);
            return true;
        }
    }
    return false;
}

/// The camera's CVars registered in loops (0x005fd910): the eight views'
/// distance, pitch and yaw (0x50: hidden), and the smoothing and terrain tilt
/// tables, five styles each (0x10).
std::optional<uint32_t> loopCVarFlags(std::string_view name) {
    std::string_view rest = name;
    // Longest first, so "cameraSmoothViewData" is not read as "cameraSmooth".
    if (takePart(rest, {"cameraSmoothViewData"})) {
        if (takePart(rest, {"Never", "Smarter", "Smart", "Always", "Spline"}) &&
            takePart(rest, {"Distance", "Pitch", "Yaw"}) &&
            takePart(rest, {"Delay", "Factor"}) && rest.empty()) {
            return 0x10u;
        }
        return std::nullopt;
    }
    rest = name;
    if (takePart(rest, {"cameraSmooth"})) {
        if (takePart(rest, {"Never", "Smarter", "Smart", "Always", "Spline"}) &&
            takePart(rest, {"Idle", "Stop", "Track", "Move", "Strafe", "Turn", "Fear"}) &&
            takePart(rest, {"Delay", "Factor"}) && rest.empty()) {
            return 0x10u;
        }
        return std::nullopt;
    }
    rest = name;
    if (takePart(rest, {"cameraTerrainTilt"})) {
        if (takePart(rest, {"Never", "Smarter", "Smart", "Always", "Spline"}) &&
            takePart(rest, {"Fall", "Fear", "Idle", "Jump", "Move", "Strafe", "Swim", "Taxi",
                            "Track", "Turn"}) &&
            takePart(rest, {"Absorb", "Delay", "Factor"}) && rest.empty()) {
            return 0x10u;
        }
        return std::nullopt;
    }
    rest = name;
    if (takePart(rest, {"camera"}) && takePart(rest, {"Distance", "Pitch", "Yaw"})) {
        for (const char* view : {"", "A", "B", "C", "D", "E", "Com", "Barber Shop"}) {
            if (equalsNoCase(rest, view)) return 0x50u;
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<uint32_t> clientCVarFlags(std::string_view name) {
    for (const ClientCVar& cvar : kClientCVars) {
        if (equalsNoCase(name, cvar.name)) return cvar.flags;
    }
    return loopCVarFlags(name);
}

std::span<const std::string_view> clientCVarNames() {
    // Built once from the table rather than kept beside it: the table is a
    // constexpr array of `const char*`, and every caller wants a list of names
    // it can sort and iterate without knowing the row's other field.
    static const std::vector<std::string_view> names = [] {
        std::vector<std::string_view> list;
        list.reserve(std::span(kClientCVars).size());
        for (const ClientCVar& cvar : kClientCVars) list.push_back(cvar.name);
        return list;
    }();
    return names;
}

}  // namespace wowee::addons
