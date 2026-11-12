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
    /// The default string it is registered with, or null where the
    /// registration computes one at run time (a register, not a literal) or
    /// reads it out of a table.
    const char* defaultValue;
};

// Every CVar the client registers by name (0x00767fc0 with a literal name),
// with the flags and the default it registers it with, in case-insensitive order. Read out of
// the 3.3.5a build 12340 binary; the names built in a loop are matched by
// clientCVarFlags below instead.
constexpr ClientCVar kClientCVars[] = {
    {"accountList", 0x0, ""},
    {"accountName", 0x40, ""},
    {"accounttype", 0x0, ""},
    {"addFriendInfoShown", 0x10, "0"},
    {"advancedWatchFrame", 0x10, "0"},
    {"advancedWorldMap", 0x20, "0"},
    {"alwaysCompareItems", 0x10, "0"},
    {"alwaysShowActionBars", 0x10, "0"},
    {"assistAttack", 0x20, "0"},
    {"asyncHandlerTimeout", 0x1, "100"},
    {"asyncThreadSleep", 0x1, "0"},
    {"auctionDisplayOnCharacter", 0x10, "0"},
    {"autoClearAFK", 0x10, "1"},
    {"autoCompleteResortNamesOnRecency", 0x10, "1"},
    {"autoCompleteUseContext", 0x10, "1"},
    {"autoCompleteWhenEditingFromCenter", 0x10, "1"},
    {"autoDismount", 0x10, "1"},
    {"autoDismountFlying", 0x10, "0"},
    {"autoFilledMultiCastSlots", 0x20, "0"},
    {"autoInteract", 0x10, nullptr},
    {"autojoinBGVoice", 0x10, "0"},
    {"autojoinPartyVoice", 0x10, "1"},
    {"autoLootDefault", 0x10, "0"},
    {"autoQuestProgress", 0x10, "1"},
    {"autoQuestWatch", 0x10, "1"},
    {"autoRangedCombat", 0x10, "1"},
    {"autoSelfCast", 0x20, "1"},
    {"autoStand", 0x10, "1"},
    {"autoUnshift", 0x10, "1"},
    {"baseMip", 0x1, "0"},
    {"blockTrades", 0x20, "0"},
    {"bspcache", 0x1, "1"},
    {"buffDurations", 0x10, "1"},
    {"calendarShowBattlegrounds", 0x20, "0"},
    {"calendarShowDarkmoon", 0x20, "1"},
    {"calendarShowLockouts", 0x20, "1"},
    {"calendarShowResets", 0x20, "0"},
    {"calendarShowWeeklyHolidays", 0x20, "1"},
    {"cameraBobbing", 0x0, "0"},
    {"cameraBobbingSmoothSpeed", 0x10, "0.8"},
    {"cameraCustomViewSmoothing", 0x10, "0"},
    {"cameraDistanceMax", 0x10, "15.0"},
    {"cameraDistanceMaxFactor", 0x10, "1.0"},
    {"cameraDistanceMoveSpeed", 0x10, "8.33"},
    {"cameraDistanceSmoothSpeed", 0x10, "8.33"},
    {"cameraDive", 0x10, "1"},
    {"cameraFlyingMountHeightSmoothSpeed", 0x10, "2.0"},
    {"cameraFoVSmoothSpeed", 0x10, "0.5"},
    {"cameraGroundSmoothSpeed", 0x10, "7.5"},
    {"cameraHeightIgnoreStandState", 0x10, "0"},
    {"cameraHeightSmoothSpeed", 0x10, "1.2"},
    {"cameraPitchMoveSpeed", 0x10, "90"},
    {"cameraPitchSmoothMax", 0x10, "30.0"},
    {"cameraPitchSmoothMin", 0x10, "0.0"},
    {"cameraPitchSmoothSpeed", 0x10, "45"},
    {"cameraPivot", 0x10, "1"},
    {"cameraPivotDXMax", 0x10, "0.05"},
    {"cameraPivotDYMin", 0x10, "0.00"},
    {"cameraSavedDistance", 0x20, nullptr},
    {"cameraSavedPitch", 0x20, nullptr},
    {"cameraSavedVehicleDistance", 0x20, "-1.0"},
    {"camerasmooth", 0x10, "1"},
    {"cameraSmoothPitch", 0x10, "1"},
    {"cameraSmoothStyle", 0x10, nullptr},
    {"cameraSmoothTimeMax", 0x10, "2.0"},
    {"cameraSmoothTimeMin", 0x10, "0.1"},
    {"cameraSmoothTrackingStyle", 0x10, nullptr},
    {"cameraSmoothYaw", 0x10, "1"},
    {"cameraSubmergeFinalPitch", 0x10, "5.0"},
    {"cameraSubmergePitch", 0x10, "18.0"},
    {"cameraSurfaceFinalPitch", 0x10, "5.0"},
    {"cameraSurfacePitch", 0x10, "0.0"},
    {"cameraTargetSmoothSpeed", 0x10, "90"},
    {"cameraTerrainTilt", 0x10, "0"},
    {"cameraTerrainTiltTimeMax", 0x10, "10.0"},
    {"cameraTerrainTiltTimeMin", 0x10, "3.0"},
    {"cameraView", 0x10, nullptr},
    {"cameraViewBlendStyle", 0x10, nullptr},
    {"cameraWaterCollision", 0x10, "1"},
    {"cameraYawMoveSpeed", 0x10, "180"},
    {"cameraYawSmoothMax", 0x10, "0.0"},
    {"cameraYawSmoothMin", 0x10, "0.0"},
    {"cameraYawSmoothSpeed", 0x10, "180"},
    {"ChatAmbienceVolume", 0x0, "0.3"},
    {"chatBubbles", 0x10, "1"},
    {"chatBubblesParty", 0x10, "1"},
    {"chatMouseScroll", 0x10, "1"},
    {"ChatMusicVolume", 0x0, "0.3"},
    {"ChatSoundVolume", 0x0, "0.4"},
    {"chatStyle", 0x10, "im"},
    {"checkAddonVersion", 0x0, "1"},
    {"CinematicJoystick", 0x0, "0"},
    {"colorblindMode", 0x10, "0"},
    {"colorChatNamesByClass", 0x10, "0"},
    {"CombatDamage", 0x10, "1"},
    {"CombatHealing", 0x10, "1"},
    {"combatLogOn", 0x20, "1"},
    {"CombatLogPeriodicSpells", 0x10, "1"},
    {"combatLogRetentionTime", 0x10, "300"},
    {"combatTextFloatMode", 0x10, "1"},
    {"componentCompress", 0x1, "1"},
    {"componentTextureLevel", 0x1, "8"},
    {"componentThread", 0x1, "1"},
    {"consolidateBuffs", 0x20, "0"},
    {"conversationMode", 0x10, "popout"},
    {"converted", 0x0, "0"},
    {"currencyTokensBackpack1", 0x20, "0"},
    {"currencyTokensBackpack2", 0x20, "0"},
    {"currencyTokensUnused1", 0x20, "0"},
    {"currencyTokensUnused2", 0x20, "0"},
    {"dbCompress", 0x0, "-1"},
    {"decorateAccountName", 0x0, "0"},
    {"deselectOnClick", 0x10, "1"},
    {"DesktopGamma", 0x0, "0"},
    {"displayFreeBagSlots", 0x10, "0"},
    {"displayWorldPVPObjectives", 0x10, "1"},
    {"dontShowEquipmentSetsOnItems", 0x10, "0"},
    {"enableCombatText", 0x10, "1"},
    {"EnableMicrophone", 0x0, "1"},
    {"enablePVPNotifyAFK", 0x10, "1"},
    {"EnableVoiceChat", 0x0, "0"},
    {"enableWowMouse", 0x1, "0"},
    {"environmentDetail", 0x1, "1.0"},
    {"equipmentManager", 0x20, "0"},
    {"ErrorFilter", 0x0, "all"},
    {"ErrorLevelMax", 0x0, "3"},
    {"ErrorLevelMin", 0x0, "2"},
    {"Errors", 0x0, "0"},
    {"expansionMovie", 0x0, "1"},
    {"extShadowQuality", 0x1, "0"},
    {"farclip", 0x1, "350"},
    {"farClipOverride", 0x1, "0"},
    {"fctAllSpellMechanics", 0x10, "0"},
    {"fctAuras", 0x10, "0"},
    {"fctCombatState", 0x10, "0"},
    {"fctComboPoints", 0x10, "0"},
    {"fctDamageReduction", 0x10, "0"},
    {"fctDodgeParryMiss", 0x10, "0"},
    {"fctEnergyGains", 0x10, "0"},
    {"fctFriendlyHealers", 0x10, "0"},
    {"fctHonorGains", 0x10, "0"},
    {"fctLowManaHealth", 0x10, "1"},
    {"fctPeriodicEnergyGains", 0x10, "0"},
    {"fctReactives", 0x10, "0"},
    {"fctRepChanges", 0x10, "0"},
    {"fctSpellMechanics", 0x10, "0"},
    {"fctSpellMechanicsOther", 0x10, "0"},
    {"ffx", 0x1, "1"},
    {"ffxDeath", 0x1, nullptr},
    {"ffxGlow", 0x1, nullptr},
    {"ffxNetherWorld", 0x1, "1"},
    {"ffxRectangle", 0x1, "1"},
    {"ffxSpecial", 0x1, nullptr},
    {"fixedFunction", 0x3, "0"},
    {"flaggedTutorials", 0x110, ""},
    {"footstepBias", 0x1, "0.125"},
    {"FootstepSounds", 0x0, "1"},
    {"forceEnglishNames", 0x0, "0"},
    {"friendsSmallView", 0x20, "0"},
    {"friendsViewButtons", 0x20, "0"},
    {"fullSizeFocusFrame", 0x20, "0"},
    {"g_accountUsesToken", 0x0, "0"},
    {"gameTip", 0x0, "0"},
    {"Gamma", 0x0, "1.0"},
    {"groundEffectDensity", 0x1, "16"},
    {"groundEffectDist", 0x1, "70.0"},
    {"guildMemberNotify", 0x10, "0"},
    {"guildRecruitmentChannel", 0x10, "1"},
    {"guildShowOffline", 0x10, "1"},
    {"gxApi", 0x3, nullptr},
    {"gxAspect", 0x3, "1"},
    {"gxColorBits", 0x3, nullptr},
    {"gxCursor", 0x3, "1"},
    {"gxDepthBits", 0x3, nullptr},
    {"gxFixLag", 0x3, nullptr},
    {"gxMaximize", 0x3, nullptr},
    {"gxMultisample", 0x3, nullptr},
    {"gxMultisampleQuality", 0x3, "0.0"},
    {"gxOverride", 0x1, ""},
    {"gxRefresh", 0x3, "75"},
    {"gxResolution", 0x3, nullptr},
    {"gxStereoConvergence", 0x1, "1"},
    {"gxStereoEnabled", 0x1, "0"},
    {"gxStereoSeparation", 0x1, "25"},
    {"gxTextureCacheSize", 0x1, "0"},
    {"gxTripleBuffer", 0x3, "0"},
    {"gxVSync", 0x3, "1"},
    {"gxWindow", 0x3, nullptr},
    {"heapAllocTracking", 0x0, "1"},
    {"hidePartyInRaid", 0x10, "0"},
    {"horizonFarclipScale", 0x1, "4.0"},
    {"horizonNearclipScale", 0x1, "0.7"},
    {"hwDetect", 0x1, "1"},
    {"hwPCF", 0x1, "1"},
    {"InboundChatVolume", 0x0, "1.0"},
    {"Joystick", 0x0, "0"},
    {"lastCharacterIndex", 0x0, "0"},
    {"lastTalkedToGM", 0x10, ""},
    {"lfdCollapsedHeaders", 0x120, ""},
    {"lfdSelectedDungeons", 0x120, ""},
    {"lfgAutoFill", 0x10, "0"},
    {"lfgAutoJoin", 0x10, "0"},
    {"lfgSelectedRoles", 0x120, "0"},
    {"locale", 0x0, "****"},
    {"lockActionBars", 0x10, "1"},
    {"lod", 0x1, "0"},
    {"lootUnderMouse", 0x10, "1"},
    {"M2BatchDoodads", 0x0, "1"},
    {"M2BatchParticles", 0x0, "1"},
    {"M2Faster", 0x0, "1"},
    {"M2FasterDebug", 0x0, "0"},
    {"M2ForceAdditiveParticleSort", 0x0, "0"},
    {"M2UseClipPlanes", 0x0, "1"},
    {"M2UseThreads", 0x0, "1"},
    {"M2UseZFill", 0x0, "1"},
    {"mapObjLightLOD", 0x1, "0"},
    {"mapQuestDifficulty", 0x20, "0"},
    {"mapShadows", 0x1, "1"},
    {"maxFPS", 0x1, "200"},
    {"maxFPSBk", 0x1, "30"},
    {"MaxLights", 0x1, "4"},
    {"minimapInsideZoom", 0x20, "3"},
    {"minimapPortalMax", 0x20, "99"},
    {"minimapTrackedInfo", 0x20, ""},
    {"minimapZoom", 0x20, "3"},
    {"miniWorldMap", 0x20, "0"},
    {"mouseInvertPitch", 0x10, "0"},
    {"mouseInvertYaw", 0x10, "0"},
    {"mouseSpeed", 0x0, nullptr},
    {"movie", 0x0, "1"},
    {"movieSubtitle", 0x0, "0"},
    {"nameplateAllowOverlap", 0x20, "1"},
    {"nameplateShowEnemies", 0x20, "0"},
    {"nameplateShowEnemyGuardians", 0x20, "1"},
    {"nameplateShowEnemyPets", 0x20, "1"},
    {"nameplateShowEnemyTotems", 0x20, "1"},
    {"nameplateShowFriendlyGuardians", 0x20, "1"},
    {"nameplateShowFriendlyPets", 0x20, "1"},
    {"nameplateShowFriendlyTotems", 0x20, "1"},
    {"nameplateShowFriends", 0x20, "0"},
    {"nearclip", 0x1, "0.2"},
    {"objectFade", 0x1, "1"},
    {"objectFadeZFill", 0x1, "0"},
    {"ObjectSelectionCircle", 0x0, "1"},
    {"occlusion", 0x1, "1"},
    {"OutboundChatVolume", 0x0, "1.0"},
    {"particleDensity", 0x1, "1.0"},
    {"partyBackgroundOpacity", 0x10, "0.5"},
    {"partyStatusText", 0x10, "0"},
    {"pathDistTol", 0x0, "1"},
    {"pendingInviteInfoShown", 0x10, "0"},
    {"PetMeleeDamage", 0x10, "1"},
    {"PetSpellDamage", 0x10, "1"},
    {"petStatusText", 0x10, "0"},
    {"playerStatLeftDropdown", 0x20, ""},
    {"playerStatRightDropdown", 0x20, ""},
    {"playerStatusText", 0x10, "0"},
    {"POIShiftComplete", 0x0, "0.6"},
    {"portal", 0x0, ""},
    {"predictedHealth", 0x10, "1"},
    {"predictedPower", 0x10, "1"},
    {"previewTalents", 0x20, "0"},
    {"processAffinityMask", 0x2, "0"},
    {"profanityFilter", 0x10, "1"},
    {"projectedTextures", 0x1, "0"},
    {"PushToTalkButton", 0x0, "`"},
    {"PushToTalkSound", 0x10, "0"},
    {"questFadingDisable", 0x10, "1"},
    {"questLogCollapseFilter", 0x20, "0"},
    {"questPOI", 0x20, "1"},
    {"readContest", 0x0, "0"},
    {"readEULA", 0x0, "0"},
    {"readScanning", 0x0, "0"},
    {"readTerminationWithoutNotice", 0x0, "0"},
    {"readTOS", 0x0, "0"},
    {"realmList", 0x0, "us.logon.worldofwarcraft.com:3724"},
    {"realmListbn", 0x0, ""},
    {"realmName", 0x0, ""},
    {"removeChatDelay", 0x10, "0"},
    {"rotateMinimap", 0x10, "0"},
    {"screenEdgeFlash", 0x10, "1"},
    {"screenshotFormat", 0x1, "jpeg"},
    {"screenshotQuality", 0x1, "3"},
    {"scriptErrors", 0x10, "0"},
    {"scriptProfile", 0x0, "0"},
    {"secureAbilityToggle", 0x10, "1"},
    {"serverAlert", 0x0, "SERVER_ALERT_URL"},
    {"serviceTypeFilter", 0x10, nullptr},
    {"shadowCull", 0x0, "1"},
    {"shadowInstancing", 0x0, "1"},
    {"shadowLevel", 0x1, "1"},
    {"shadowLOD", 0x1, "1"},
    {"shadowScissor", 0x0, "1"},
    {"ShowAllSpellRanks", 0x10, "1"},
    {"showArenaEnemyCastbar", 0x20, "1"},
    {"showArenaEnemyFrames", 0x20, "1"},
    {"showArenaEnemyPets", 0x20, "1"},
    {"showBattlefieldMinimap", 0x20, "0"},
    {"showCastableBuffs", 0x20, "0"},
    {"showCastableDebuffs", 0x20, "0"},
    {"ShowClassColorInNameplate", 0x20, "0"},
    {"showClock", 0x10, "1"},
    {"showDispelDebuffs", 0x20, "0"},
    {"ShowErrors", 0x0, "1"},
    {"showfootprintparticles", 0x1, "1"},
    {"showfootprints", 0x1, "1"},
    {"showGameTips", 0x0, "1"},
    {"showItemLevel", 0x10, "0"},
    {"showKeyring", 0x20, "0"},
    {"showLootSpam", 0x10, "1"},
    {"showNewbieTips", 0x10, "1"},
    {"showPartyBackground", 0x10, "0"},
    {"showPartyPets", 0x20, "1"},
    {"showQuestObjectivesOnMap", 0x20, "1"},
    {"showQuestTrackingTooltips", 0x20, "1"},
    {"showRaidRange", 0x20, "0"},
    {"showTargetCastbar", 0x10, "1"},
    {"showTargetOfTarget", 0x10, "0"},
    {"showTimestamps", 0x10, "none"},
    {"showToastBroadcast", 0x10, "1"},
    {"showToastConversation", 0x10, "1"},
    {"showToastFriendRequest", 0x10, "1"},
    {"showToastOffline", 0x10, "1"},
    {"showToastOnline", 0x10, "1"},
    {"showToastWindow", 0x10, "1"},
    {"showTokenFrame", 0x20, "0"},
    {"showTokenFrameHonor", 0x20, "0"},
    {"showToolsUI", 0x0, "-1"},
    {"showTutorials", 0x10, "1"},
    {"showVKeyCastbar", 0x10, "1"},
    {"SkyCloudLOD", 0x0, "0"},
    {"Sound_AmbienceVolume", 0x0, "0.6"},
    {"Sound_ChaosMode", 0x0, "0"},
    {"Sound_DSPBufferSize", 0x0, "0"},
    {"Sound_EnableAllSound", 0x0, "1"},
    {"Sound_EnableAmbience", 0x0, "1"},
    {"Sound_EnableArmorFoleySoundForOthers", 0x0, "1"},
    {"Sound_EnableArmorFoleySoundForSelf", 0x0, "1"},
    {"Sound_EnableDSPEffects", 0x0, nullptr},
    {"Sound_EnableEmoteSounds", 0x0, "1"},
    {"Sound_EnableErrorSpeech", 0x0, "1"},
    {"Sound_EnableHardware", 0x0, "0"},
    {"Sound_EnableMixMode2", 0x0, "0"},
    {"Sound_EnableMode2", 0x0, "0"},
    {"Sound_EnableMusic", 0x0, "1"},
    {"Sound_EnablePetSounds", 0x0, "1"},
    {"Sound_EnableReverb", 0x0, "0"},
    {"Sound_EnableSFX", 0x0, "1"},
    {"Sound_EnableSoftwareHRTF", 0x0, "0"},
    {"Sound_EnableSoundWhenGameIsInBG", 0x0, "0"},
    {"Sound_ListenerAtCharacter", 0x0, "1"},
    {"Sound_MasterVolume", 0x0, "1.0"},
    {"Sound_MaxCacheableSizeInBytes", 0x0, "1048576"},
    {"Sound_MaxCacheSizeInBytes", 0x0, "16777216"},
    {"Sound_MusicVolume", 0x0, "0.4"},
    {"Sound_NumChannels", 0x0, "32"},
    {"Sound_OutputDriverIndex", 0x0, "0"},
    {"Sound_OutputDriverName", 0x0, "Primary Sound Driver"},
    {"Sound_OutputQuality", 0x0, "1"},
    {"Sound_SFXVolume", 0x0, "1.0"},
    {"Sound_VoiceChatInputDriverIndex", 0x0, "0"},
    {"Sound_VoiceChatInputDriverName", 0x0, "Primary Sound Capture Driver"},
    {"Sound_VoiceChatOutputDriverIndex", 0x0, "0"},
    {"Sound_VoiceChatOutputDriverName", 0x0, "Primary Sound Driver"},
    {"Sound_ZoneMusicNoDelay", 0x0, "0"},
    {"SoundMemoryCache", 0x2, "4"},
    {"spamFilter", 0x10, "1"},
    {"specular", 0x1, "0"},
    {"spellEffectLevel", 0x1, "9"},
    {"SplineOpt", 0x1, "1"},
    {"StartTalkingDelay", 0x0, "0.0"},
    {"StartTalkingTime", 0x0, "1.0"},
    {"statusTextPercentage", 0x10, "0"},
    {"stopAutoAttackOnTargetChange", 0x20, "0"},
    {"StopTalkingDelay", 0x0, "0.0"},
    {"StopTalkingTime", 0x0, "2.0"},
    {"synchronizeBindings", 0x0, "1"},
    {"synchronizeConfig", 0x0, "1"},
    {"synchronizeMacros", 0x0, "1"},
    {"synchronizeSettings", 0x0, "1"},
    {"taintLog", 0x0, "0"},
    {"talentFrameShown", 0x10, "0"},
    {"targetOfTargetMode", 0x10, "5"},
    {"targetStatusText", 0x10, "0"},
    {"terrainAlphaBitDepth", 0x1, "8"},
    {"texLodBias", 0x1, "0.0"},
    {"textureCacheSize", 0x1, "32"},
    {"textureFilteringMode", 0x1, "1"},
    {"threatPlaySounds", 0x10, "1"},
    {"threatShowNumeric", 0x10, "0"},
    {"threatWarning", 0x20, "3"},
    {"threatWorldText", 0x10, "1"},
    {"timeMgrAlarmEnabled", 0x10, "0"},
    {"timeMgrAlarmMessage", 0x10, ""},
    {"timeMgrAlarmTime", 0x10, "0"},
    {"timeMgrUseLocalTime", 0x10, "0"},
    {"timeMgrUseMilitaryTime", 0x10, nullptr},
    {"timingMethod", 0x2, "0"},
    {"timingTestError", 0x6, "0"},
    {"toastDuration", 0x10, "4"},
    {"trackedAchievements", 0x120, ""},
    {"trackedQuests", 0x120, ""},
    {"trackerFilter", 0x20, "7"},
    {"trackerSorting", 0x20, "0"},
    {"UberTooltips", 0x10, "1"},
    {"UIFaster", 0x0, "3"},
    {"uiScale", 0x0, "1.0"},
    {"unitHighlights", 0x10, "1"},
    {"UnitNameEnemyGuardianName", 0x10, "0"},
    {"UnitNameEnemyPetName", 0x10, "1"},
    {"UnitNameEnemyPlayerName", 0x10, "1"},
    {"UnitNameEnemyTotemName", 0x10, "0"},
    {"UnitNameFriendlyGuardianName", 0x10, "0"},
    {"UnitNameFriendlyPetName", 0x10, "1"},
    {"UnitNameFriendlyPlayerName", 0x10, "1"},
    {"UnitNameFriendlyTotemName", 0x10, "0"},
    {"UnitNameNonCombatCreatureName", 0x10, "0"},
    {"UnitNameNPC", 0x10, "0"},
    {"UnitNameOwn", 0x10, "0"},
    {"UnitNamePlayerGuild", 0x10, "1"},
    {"UnitNamePlayerPVPTitle", 0x10, "1"},
    {"useEnglishAudio", 0x0, "0"},
    {"useUiScale", 0x0, "0"},
    {"useWeatherShaders", 0x0, "1"},
    {"videoOptionsVersion", 0x3, "0"},
    {"violenceLevel", 0x10, nullptr},
    {"VoiceActivationSensitivity", 0x0, "0.4"},
    {"VoiceChatMode", 0x0, "0"},
    {"VoiceChatSelfMute", 0x0, "0"},
    {"watchFrameBaseAlpha", 0x10, "0"},
    {"watchFrameIgnoreCursor", 0x10, "0"},
    {"watchFrameState", 0x10, "0"},
    {"watchFrameWidth", 0x0, "0"},
    {"waterLOD", 0x1, "0"},
    {"weatherDensity", 0x0, "2"},
    {"wholeChatWindowClickable", 0x10, "1"},
    {"widescreen", 0x0, "1"},
    {"windowResizeLock", 0x1, "0"},
    {"worldMapOpacity", 0x20, "0"},
    {"worldPoolUsage", 0x1, "Dynamic"},
    {"xpBarText", 0x10, "0"},
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

std::optional<std::string_view> clientCVarDefault(std::string_view name) {
    for (const ClientCVar& cvar : kClientCVars) {
        if (equalsNoCase(name, cvar.name)) {
            if (!cvar.defaultValue) return std::nullopt;
            return std::string_view(cvar.defaultValue);
        }
    }
    return std::nullopt;
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
