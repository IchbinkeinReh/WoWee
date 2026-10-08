#include "core/local_time.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/light_coords.hpp"
#include "rendering/light_band_block.hpp"
#include "rendering/day_night.hpp"
#include "rendering/spell_kit.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/wmo_doodad_light.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>

namespace wowee {
namespace rendering {


// WoW's Light.dbc stores time-of-day as half-minutes (0..2879).
// 24 hours × 60 minutes × 2 = 2880 half-minute ticks per day cycle.
constexpr uint16_t kHalfMinutesPerDay = 2880;

LightingManager::LightingManager() {
    // Only for a client with no light to blend: Light.dbc missing, or no row
    // for the map and no global default. The client's fixed set for that
    // (0x007f3230 with DAT_00d39008 0): white, a grey ambient, and fog to the
    // far clip from half way with an exponent of 4.
    fallbackParams_ = noLightParams();
    currentParams_ = fallbackParams_;
}

LightingManager::~LightingManager() {
}

bool LightingManager::initialize(pipeline::AssetManager* assetManager) {
    if (!assetManager) {
        LOG_ERROR("LightingManager::initialize: null AssetManager");
        return false;
    }


    // Load DBCs (non-fatal if missing, will use fallback lighting)
    loadLightDbc(assetManager);
    loadLightParamsDbc(assetManager);
    loadLightSkyboxDbc(assetManager);
    loadLightBandDbcs(assetManager);
    loadLiquidTypeDbc(assetManager);

    initialized_ = true;
    LOG_INFO("LightingManager initialized: ", lightVolumesByMap_.size(), " maps with lighting");
    return true;
}

bool LightingManager::loadLightDbc(pipeline::AssetManager* assetManager) {
    auto dbcData = assetManager->readFile("DBFilesClient\\Light.dbc");
    if (dbcData.empty()) {
        LOG_WARNING("Light.dbc not found, using fallback lighting");
        return false;
    }

    auto dbc = std::make_unique<pipeline::DBCFile>();
    if (!dbc->load(dbcData)) {
        LOG_ERROR("Failed to load Light.dbc");
        return false;
    }

    uint32_t recordCount = dbc->getRecordCount();
    LOG_INFO("Loading Light.dbc: ", recordCount, " light volumes");

    // Parse light volumes
    // Light.dbc structure (WotLK 3.3.5a):
    // 0: uint32 ID
    // 1: uint32 MapID
    // 2-4: float X, Z, Y (note: z and y swapped!)
    // 5: float FalloffStart (inner radius)
    // 6: float FalloffEnd (outer radius)
    // 7-14: uint32 LightParamsID[8], indexed by the client (0x007eb180):
    //       0 normal, 1 underwater, 2 storm, 3 storm underwater, 4 death.
    //       Columns 8 and 9 were read as "rain" and "underwater" - the other
    //       way round - so swimming showed the storm set and rain the
    //       underwater one.

    const auto* activeLayout = pipeline::getActiveDBCLayout();
    const auto* lL = activeLayout ? activeLayout->getLayout("Light") : nullptr;

    for (uint32_t i = 0; i < recordCount; ++i) {
        LightVolume volume;
        volume.lightId = dbc->getUInt32(i, lL ? (*lL)["ID"] : 0);
        volume.mapId = dbc->getUInt32(i, lL ? (*lL)["MapID"] : 1);

        // Into world space: thirty-sixths of a yard on the tile grid's axes,
        // which are mirrored and swapped against the world's. Checked against
        // six zones whose world coordinates are known - Tirisfal, Undercity,
        // Stormwind, Ironforge, Westfall and Booty Bay - each of which lands
        // inside a volume only under this mapping.
        const float dbcX = dbc->getFloat(i, lL ? (*lL)["X"] : 2);
        const float dbcZ = dbc->getFloat(i, lL ? (*lL)["Z"] : 3);
        const float dbcY = dbc->getFloat(i, lL ? (*lL)["Y"] : 4);
        volume.position = lightPositionToWorld(dbcX, dbcY, dbcZ);

        volume.innerRadius =
            dbc->getFloat(i, lL ? (*lL)["InnerRadius"] : 5) / LIGHT_COORD_UNITS_PER_YARD;
        volume.outerRadius =
            dbc->getFloat(i, lL ? (*lL)["OuterRadius"] : 6) / LIGHT_COORD_UNITS_PER_YARD;

        static constexpr const char* kParamsColumns[LightVolume::PARAMS_SLOT_COUNT] = {
            "LightParamsID", "LightParamsIDUnderwater", "LightParamsIDStorm",
            "LightParamsIDStormUnderwater", "LightParamsIDDeath"};
        for (uint32_t slot = 0; slot < LightVolume::PARAMS_SLOT_COUNT; ++slot) {
            const uint32_t field = lL ? (*lL)[kParamsColumns[slot]] : 7 + slot;
            if (field < dbc->getFieldCount()) {
                volume.lightParamsIds[slot] = dbc->getUInt32(i, field);
            }
        }

        // A light at (0,0,0) in the file is its map's default: the one the
        // client starts from before any other light is applied, everywhere on
        // the map (0x007ecb30 puts it in slot 0 of the map's list, and
        // 0x007f1360 walks the others from slot 1). Converted, it landed at a
        // corner of the map with radius 0 and never counted, so every place
        // outside a zone light - and every instance, whose only light it
        // usually is - fell to invented constants.
        //
        // Light ID 1 stands in for a map that has no default of its own.
        lightsById_[volume.lightId] = volume;
        if (volume.lightId == 1) {
            globalDefaultLight_ = volume;
            hasGlobalDefaultLight_ = true;
        }
        if (dbcX == 0.0f && dbcY == 0.0f && dbcZ == 0.0f) {
            defaultLightByMap_[volume.mapId] = volume;
            continue;
        }

        // Add to map-specific list
        lightVolumesByMap_[volume.mapId].push_back(volume);
    }

    LOG_INFO("Loaded ", lightVolumesByMap_.size(), " maps with lighting volumes");
    return true;
}

bool LightingManager::loadLightParamsDbc(pipeline::AssetManager* assetManager) {
    auto dbcData = assetManager->readFile("DBFilesClient\\LightParams.dbc");
    if (dbcData.empty()) {
        LOG_WARNING("LightParams.dbc not found");
        return false;
    }

    auto dbc = std::make_unique<pipeline::DBCFile>();
    if (!dbc->load(dbcData)) {
        LOG_ERROR("Failed to load LightParams.dbc");
        return false;
    }

    uint32_t recordCount = dbc->getRecordCount();
    LOG_INFO("Loaded LightParams.dbc: ", recordCount, " profiles");

    // Create profile entries (will be populated by band loading). The
    // constants 0x007ebff0 copies out of the row: HighlightSky, Glow and the
    // four water alphas. 3.x put CloudTypeID before Glow, which the layouts
    // say per expansion.
    const auto* lpL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("LightParams") : nullptr;
    const uint32_t fieldCount = dbc->getFieldCount();
    const auto column = [&](const char* name) -> uint32_t {
        const uint32_t f = lpL ? lpL->tryField(name) : 0xFFFFFFFFu;
        return f < fieldCount ? f : 0xFFFFFFFFu;
    };
    const uint32_t skyboxCol = lpL ? column("LightSkyboxID") : (fieldCount > 2 ? 2u : 0xFFFFFFFFu);
    const uint32_t highlightCol = column("HighlightSky");
    const uint32_t glowCol = column("Glow");
    const uint32_t alphaCols[4] = {column("WaterShallowAlpha"), column("WaterDeepAlpha"),
                                   column("OceanShallowAlpha"), column("OceanDeepAlpha")};
    for (uint32_t i = 0; i < recordCount; ++i) {
        uint32_t paramId = dbc->getUInt32(i, lpL ? (*lpL)["LightParamsID"] : 0);
        LightParamsProfile profile;
        profile.lightParamsId = paramId;
        if (skyboxCol != 0xFFFFFFFFu) profile.lightSkyboxId = dbc->getUInt32(i, skyboxCol);
        if (highlightCol != 0xFFFFFFFFu) profile.highlightSky = static_cast<float>(dbc->getUInt32(i, highlightCol));
        if (glowCol != 0xFFFFFFFFu) profile.glow = dbc->getFloat(i, glowCol);
        float* alphas[4] = {&profile.waterShallowAlpha, &profile.waterDeepAlpha,
                            &profile.oceanShallowAlpha, &profile.oceanDeepAlpha};
        for (int a = 0; a < 4; ++a) {
            if (alphaCols[a] != 0xFFFFFFFFu) *alphas[a] = dbc->getFloat(i, alphaCols[a]);
        }
        lightParamsProfiles_[paramId] = profile;
    }

    return true;
}

bool LightingManager::loadLightSkyboxDbc(pipeline::AssetManager* assetManager) {
    auto dbcData = assetManager->readFile("DBFilesClient\\LightSkybox.dbc");
    if (dbcData.empty()) {
        LOG_WARNING("LightSkybox.dbc not found");
        return false;
    }

    pipeline::DBCFile dbc;
    if (!dbc.load(dbcData) || dbc.getFieldCount() < 2) {
        LOG_ERROR("Failed to load LightSkybox.dbc");
        return false;
    }

    lightSkyboxPaths_.clear();
    lightSkyboxFlags_.clear();
    // Flags (3.x, field 2): 0x1 runs the model's animation with the time of
    // day (0x007ecf20), 0x2 keeps the procedural sky under it (0x007f3230
    // reads +8 of the row).
    const auto* skL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("LightSkybox") : nullptr;
    const uint32_t flagsCol = skL ? skL->tryField("Flags") : 0xFFFFFFFFu;
    for (uint32_t i = 0; i < dbc.getRecordCount(); ++i) {
        const uint32_t id = dbc.getUInt32(i, 0);
        std::string path = dbc.getString(i, 1);
        if (id != 0 && !path.empty()) lightSkyboxPaths_[id] = std::move(path);
        if (id != 0 && flagsCol < dbc.getFieldCount()) lightSkyboxFlags_[id] = dbc.getUInt32(i, flagsCol);
    }
    LOG_INFO("Loaded LightSkybox.dbc: ", lightSkyboxPaths_.size(), " model paths");
    return !lightSkyboxPaths_.empty();
}

bool LightingManager::loadLiquidTypeDbc(pipeline::AssetManager* assetManager) {
    // Only 3.x has the darkening columns; older layouts name none of them and
    // the client of those days had no such code.
    const auto* lqL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("LiquidType") : nullptr;
    if (!lqL || lqL->tryField("MaxDarkenDepth") == 0xFFFFFFFFu) return false;
    auto dbcData = assetManager->readFile("DBFilesClient\\LiquidType.dbc");
    if (dbcData.empty()) return false;
    pipeline::DBCFile dbc;
    if (!dbc.load(dbcData)) return false;
    const uint32_t fields = dbc.getFieldCount();
    const uint32_t cols[5] = {lqL->tryField("MaxDarkenDepth"), lqL->tryField("FogDarkenIntensity"),
                              lqL->tryField("AmbDarkenIntensity"), lqL->tryField("DirDarkenIntensity"),
                              lqL->tryField("LightID")};
    for (uint32_t c : cols) {
        if (c >= fields) return false;
    }
    const uint32_t flagsCol = lqL->tryField("Flags");
    liquidTypes_.clear();
    for (uint32_t i = 0; i < dbc.getRecordCount(); ++i) {
        LiquidTypeLight l;
        l.maxDarkenDepth = dbc.getFloat(i, cols[0]);
        l.ambDarken = dbc.getFloat(i, cols[2]);
        l.dirDarken = dbc.getFloat(i, cols[3]);
        l.lightId = dbc.getUInt32(i, cols[4]);
        // Flags 0x20, 0x40 and 0x100 decide a WMO's fog under this liquid
        // (0x007f16f0 reads the row's +0x8).
        if (flagsCol < fields) l.flags = dbc.getUInt32(i, flagsCol);
        liquidTypes_[dbc.getUInt32(i, lqL->tryField("ID") < fields ? lqL->tryField("ID") : 0)] = l;
    }
    LOG_INFO("Loaded LiquidType.dbc: ", liquidTypes_.size(), " liquids");
    return true;
}

bool LightingManager::loadLightBandDbcs(pipeline::AssetManager* assetManager) {
    // Load LightIntBand.dbc for RGB color curves (18 channels per LightParams)
    auto intBandData = assetManager->readFile("DBFilesClient\\LightIntBand.dbc");
    if (!intBandData.empty()) {
        auto dbc = std::make_unique<pipeline::DBCFile>();
        if (dbc->load(intBandData)) {
            LOG_INFO("Loaded LightIntBand.dbc: ", dbc->getRecordCount(), " color bands");

            // Parse int bands
            // Structure: ID, Entry (block index), NumValues, Time[16], Color[16]
            // Block index = LightParamsID * 18 + channel
            const auto* libL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("LightIntBand") : nullptr;
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                const uint32_t blockIndex =
                    dbc->getUInt32(i, libL ? (*libL)["BlockIndex"] : 0);
                const LightBandSlot slot =
                    lightBandSlot(blockIndex, LIGHT_INT_CHANNELS);
                if (!slot.valid) continue;

                auto it = lightParamsProfiles_.find(slot.lightParamsId);
                if (it == lightParamsProfiles_.end()) continue;

                const uint32_t channelIndex = slot.channel;
                if (channelIndex >= LightParamsProfile::COLOR_CHANNEL_COUNT) continue;

                ColorBand& band = it->second.colorBands[channelIndex];
                band.numKeyframes = dbc->getUInt32(i, libL ? (*libL)["NumKeyframes"] : 1);
                if (band.numKeyframes > 16) band.numKeyframes = 16;

                // Read time keys (field 3-18) - stored as uint16 half-minutes
                uint32_t timeKeyBase = libL ? (*libL)["TimeKey0"] : 2;
                for (uint8_t k = 0; k < band.numKeyframes && k < 16; ++k) {
                    uint32_t timeValue = dbc->getUInt32(i, timeKeyBase + k);
                    band.times[k] = static_cast<uint16_t>(timeValue % kHalfMinutesPerDay);  // Clamp to valid range
                }

                // Read color values (field 19-34) - stored as BGRA packed uint32
                uint32_t valueBase = libL ? (*libL)["Value0"] : 18;
                for (uint8_t k = 0; k < band.numKeyframes && k < 16; ++k) {
                    uint32_t colorBGRA = dbc->getUInt32(i, valueBase + k);
                    band.colors[k] = dbcColorToVec3(colorBGRA);
                }
            }
        }
    }

    // Load LightFloatBand.dbc for fog/intensity curves (6 channels per LightParams)
    auto floatBandData = assetManager->readFile("DBFilesClient\\LightFloatBand.dbc");
    if (!floatBandData.empty()) {
        auto dbc = std::make_unique<pipeline::DBCFile>();
        if (dbc->load(floatBandData)) {
            LOG_INFO("Loaded LightFloatBand.dbc: ", dbc->getRecordCount(), " float bands");

            // Parse float bands
            // Structure: ID, Entry (block index), NumValues, Time[16], Value[16]
            // Block index = LightParamsID * 6 + channel
            const auto* lfbL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("LightFloatBand") : nullptr;
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                const uint32_t blockIndex =
                    dbc->getUInt32(i, lfbL ? (*lfbL)["BlockIndex"] : 0);
                const LightBandSlot slot =
                    lightBandSlot(blockIndex, LIGHT_FLOAT_CHANNELS);
                if (!slot.valid) continue;

                auto it = lightParamsProfiles_.find(slot.lightParamsId);
                if (it == lightParamsProfiles_.end()) continue;

                const uint32_t channelIndex = slot.channel;
                if (channelIndex >= LightParamsProfile::FLOAT_CHANNEL_COUNT) continue;

                FloatBand& band = it->second.floatBands[channelIndex];
                band.numKeyframes = dbc->getUInt32(i, lfbL ? (*lfbL)["NumKeyframes"] : 1);
                if (band.numKeyframes > 16) band.numKeyframes = 16;

                // Read time keys (field 3-18)
                uint32_t timeKeyBase = lfbL ? (*lfbL)["TimeKey0"] : 2;
                for (uint8_t k = 0; k < band.numKeyframes && k < 16; ++k) {
                    uint32_t timeValue = dbc->getUInt32(i, timeKeyBase + k);
                    band.times[k] = static_cast<uint16_t>(timeValue % kHalfMinutesPerDay);  // Clamp to valid range
                }

                // Read float values (field 19-34)
                uint32_t valueBase = lfbL ? (*lfbL)["Value0"] : 18;
                for (uint8_t k = 0; k < band.numKeyframes && k < 16; ++k) {
                    band.values[k] = dbc->getFloat(i, valueBase + k);
                }
            }
        }
    }

    LOG_INFO("Loaded bands for ", lightParamsProfiles_.size(), " LightParams profiles");
    return true;
}

void LightingManager::update(const glm::vec3& playerPos, uint32_t mapId,
                              float gameTime,
                              float weatherIntensity, const CameraLiquid& liquid,
                              bool deathOverride) {
    const bool cameraInLiquid = liquid.submerged;
    if (!initialized_) return;

    // Update time
    if (!manualTime_) {
        if (gameTime >= 0.0f) {
            // Server-sent game time, in hours since midnight, already run on
            // from the server's last word at its speed (GameHandler::getGameTime).
            timeOfDay_ = std::fmod(gameTime / 24.0f, 1.0f);  // 0.0-1.0
        } else {
            // Fallback: use real time for day/night cycle
            std::time_t now = std::time(nullptr);
            const std::tm localTime = core::localTime(now);
            float secondsSinceMidnight = localTime.tm_hour * 3600.0f +
                                          localTime.tm_min * 60.0f +
                                          localTime.tm_sec;
            timeOfDay_ = secondsSinceMidnight / 86400.0f;  // 0.0-1.0
        }
    }
    // else: manualTime_ is set, use timeOfDay_ as-is

    // The light bands are keyed in half-minutes (0-2879). There is no
    // per-zone clock: Duskwood is dark because its LightParams rows are dark
    // at every hour, not because the client stops its sky at night.
    const uint16_t timeHalfMinutes = static_cast<uint16_t>(
        timeOfDay_ * static_cast<float>(kHalfMinutesPerDay)) % kHalfMinutesPerDay;

    // The client's light at this spot (0x007f3230, 0x007f1360).
    //
    // It starts from the map's default light and lerps every light within its
    // outer radius over it, farthest first, each by its own linear falloff -
    // no normalising and no cap on how many. Where the local lights fade out,
    // the default shows through. This was two volumes, renormalised to sum to
    // one, with the default never counted: a light at the edge of its falloff
    // counted in full, stepping past the outer radius dropped to invented
    // fallback colours, and an instance with only a default light was lit by
    // those everywhere.
    //
    // Every sample is the normal or the underwater set (the camera in
    // liquid), blended toward the matching storm set by min(1, 4 x weather
    // intensity) for any weather (0x007f3920, 0x007ee510).
    const float storm = daynight::stormBlend(weatherIntensity);
    activeVolumes_ = findLightVolumes(playerPos, mapId);
    // Map 530 and up draw the later fog: each light's end out at the far
    // clip and its authored range turned into an exponent (0x007816f0 sets
    // the mode, 0x007ecd80 applies it to every light it samples).
    fogExponent_ = daynight::mapUsesFogExponent(mapId);

    LightingParams newParams = fallbackParams_;
    // Sky models and how much of each, as 0x007ed4c0 keeps them: the default
    // light's at 1, then each light adds its weight to the model it names,
    // capped at 1, three models at most.
    std::vector<std::pair<uint32_t, float>> skyTargets;
    const auto addSky = [&](uint32_t skyboxId, float w) {
        if (skyboxId == 0 || w <= 0.0f) return;
        for (auto& [id, weight] : skyTargets) {
            if (id == skyboxId) { weight = std::min(1.0f, weight + w); return; }
        }
        if (skyTargets.size() < 3) skyTargets.emplace_back(skyboxId, w);
    };

    const LightVolume* base = defaultLight(mapId);
    // Under a liquid whose LiquidType names a light of its own, that light is
    // the whole of it, in place of the default and the nearby lights
    // (0x007f3230: the row's +0x28 decides the branch).
    const LiquidTypeLight* liquidRow = nullptr;
    if (cameraInLiquid) {
        auto lq = liquidTypes_.find(liquid.liquidType);
        if (lq != liquidTypes_.end()) liquidRow = &lq->second;
    }
    const LightVolume* liquidLight = nullptr;
    if (liquidRow && liquidRow->lightId != 0) {
        auto ll = lightsById_.find(liquidRow->lightId);
        if (ll != lightsById_.end()) liquidLight = &ll->second;
    }
    if (liquidLight) {
        activeVolumes_.clear();
        uint32_t skyboxId = 0;
        newParams = sampleLight(*liquidLight, true, storm, timeHalfMinutes, skyboxId);
        addSky(skyboxId, 1.0f);
    } else if (base) {
        uint32_t skyboxId = 0;
        newParams = sampleLight(*base, cameraInLiquid, storm, timeHalfMinutes, skyboxId);
        addSky(skyboxId, 1.0f);
    }
    for (const auto& wv : activeVolumes_) {
        uint32_t skyboxId = 0;
        const LightingParams sampled =
            sampleLight(*wv.volume, cameraInLiquid, storm, timeHalfMinutes, skyboxId);
        newParams = lerpLightingParams(newParams, sampled, wv.weight);
        addSky(skyboxId, wv.weight);
    }

    // The death light (0x007f3230, override index 4 from ScreenEffect via
    // 0x007ecec0): the map default light's death set replaces everything the
    // blend gave, except the glow, the water alphas and the sky models, which
    // are kept; the death set's own sky model goes up on top at full weight.
    uint32_t deathSkyboxId = 0;
    if (deathOverride && !liquidLight && base && base->lightParamsIds[LightVolume::PARAMS_DEATH] != 0) {
        auto it = lightParamsProfiles_.find(base->lightParamsIds[LightVolume::PARAMS_DEATH]);
        if (it != lightParamsProfiles_.end()) {
            LightingParams death = sampleLightParams(&it->second, timeHalfMinutes);
            death.glow = newParams.glow;
            death.waterShallowAlpha = newParams.waterShallowAlpha;
            death.waterDeepAlpha = newParams.waterDeepAlpha;
            death.oceanShallowAlpha = newParams.oceanShallowAlpha;
            death.oceanDeepAlpha = newParams.oceanDeepAlpha;
            newParams = death;
            deathSkyboxId = it->second.lightSkyboxId;
        }
    }

    // Light mode 2's colours, from the blend before it is darkened
    // (0x007ee750, called ahead of the 0x007f3230 tail).
    {
        const auto toByte = [](const glm::vec3& c) {
            return glm::ivec3(glm::round(glm::clamp(c, 0.0f, 1.0f) * 255.0f));
        };
        const auto avg = pipeline::wmo_doodad_light::averagedOutsideLight(toByte(newParams.diffuseColor),
                                                                           toByte(newParams.ambientColor));
        newParams.averagedDirectColor = glm::vec3(avg.direct) / 255.0f;
        newParams.averagedAmbientColor = glm::vec3(avg.ambient) / 255.0f;
    }

    // Darker with depth, by the LiquidType's own amounts: the ambient and the
    // direct light each scaled in HSV value - which for a colour is a plain
    // scale - by 1 - min(depth, max)/max x intensity (0x007f3230 tail,
    // 0x007ed790). That tail darkens the fog colour too, but FUN_007816f0
    // calls 0x007f16f0 after it, which sets the fog colour afresh from ch7
    // (DAT_00d38b8c = DAT_00d38bf4), so the fog is never darker for depth.
    if (liquidRow) {
        newParams.ambientColor *= daynight::liquidDarkenScale(liquid.depth, liquidRow->maxDarkenDepth,
                                                              liquidRow->ambDarken);
        newParams.diffuseColor *= daynight::liquidDarkenScale(liquid.depth, liquidRow->maxDarkenDepth,
                                                              liquidRow->dirDarken);
    }

    // A spell kit's light tint (CharProc 6) after the darkening: the sky's
    // colours (0x007f0530), the ambient and direct light and the sun and
    // moon (0x007f3230). Not the fog: 0x007f16f0 sets that afresh from ch7.
    newParams.spellTintColour = spellTintColour_;
    newParams.spellTintAmount = spellTintAmount_;
    if (spellTintAmount_ != 0) {
        for (glm::vec3* colour : {&newParams.skyTopColor, &newParams.skyMiddleColor, &newParams.skyBand1Color,
                                  &newParams.skyBand2Color, &newParams.skySmogColor, &newParams.ambientColor,
                                  &newParams.diffuseColor, &newParams.sunColor})
            *colour = spell_kit::tintColour(*colour, spellTintColour_, spellTintAmount_);
    }

    // The light's direction and the sun and moon are not in the DBC: the
    // client places them on fixed time curves (0x007eea90, 0x007eecc0).
    newParams.directionalDir = daynight::directionalLightDir(timeOfDay_);
    newParams.sunDir = daynight::sunDirection(timeOfDay_);
    newParams.moonDir = daynight::moonDirection(timeOfDay_);

    // Fog as the client draws it (0x007f16f0): linear, ending at the blended
    // fog end or the far clip, whichever is nearer, starting at the blended
    // fraction of that, in the fog channel's colour exactly.
    const daynight::FogRange fog =
        daynight::clientFogRange(newParams.fogEnd, newParams.fogStartScalar, farClip_);
    newParams.fogStart = fog.start;
    newParams.fogEnd = fog.end;

    // Inside a WMO with fog of its own (MFOG): its end within the far clip,
    // the start that fraction of it, the end then no nearer than 30 yards
    // (0x007ed1b0); blended in from the zone's fog by how far in the camera
    // is, all of it 25 yards in (0x007f16f0). With the camera in liquid the
    // record's second fog is the one, if the LiquidType and the fog's flags
    // allow it, and LiquidType flag 0x40 makes it the whole of the fog
    // rather than a blend (daynight::wmoFogChoice).
    newParams.zoneFogColor = newParams.fogColor;
    if (interiorFog_) {
        const uint32_t liquidFlags = liquidRow ? liquidRow->flags : 0u;
        const daynight::WmoFogChoice choice =
            daynight::wmoFogChoice(cameraInLiquid, liquidFlags, interiorFog_->flags);
        if (choice != daynight::WmoFogChoice::None) {
            const bool liquidFog = choice == daynight::WmoFogChoice::Liquid;
            float end = liquidFog ? interiorFog_->liquidEnd : interiorFog_->end;
            if (farClip_ > 0.0f && end > farClip_) end = farClip_;
            float start =
                (liquidFog ? interiorFog_->liquidStartScalar : interiorFog_->startScalar) * end;
            if (end < 30.0f) end = 30.0f;
            // With the later fog the record's range becomes its exponent and
            // its end the far clip, the start no nearer than the eye
            // (0x007ed1b0).
            float exponent = 1.0f;
            if (fogExponent_ && farClip_ > 0.0f) {
                exponent = daynight::clientFogExponent(start, end, farClip_);
                end = farClip_;
                if (start < 0.0f) start = 0.0f;
            }
            const glm::vec3 color = liquidFog ? interiorFog_->liquidColor : interiorFog_->color;
            // Whole, it replaces the zone's fog itself (0x007f16f0 copies it
            // over 0xd38b8c), so the outside is fogged in it too.
            const bool whole = daynight::wmoLiquidFogIsWhole(choice, liquidFlags);
            const float b = whole ? 1.0f : daynight::wmoFogBlend(interiorFog_->distanceInside);
            if (whole) newParams.zoneFogColor = color;
            newParams.fogEnd = glm::mix(newParams.fogEnd, end, b);
            newParams.fogStart = glm::mix(newParams.fogStart, start, b);
            newParams.fogColor = glm::mix(newParams.fogColor, color, b);
            newParams.fogExponent = glm::mix(newParams.fogExponent, exponent, b);
        }
    }

    // With the later fog, the camera in liquid doubles the exponent
    // (0x007f16f0, after the WMO blend).
    if (fogExponent_ && cameraInLiquid) newParams.fogExponent *= 2.0f;

    // Optional, and off by default: fog pulled toward the sky's middle band.
    // The client's fog is ch7 exactly; this was on at 0.7 to make up for a sky
    // that did not end in the fog colour at the horizon.
    if (fogSkyBlend_ > 0.0f) {
        newParams.fogColor = glm::mix(newParams.fogColor, newParams.skyMiddleColor,
                                      glm::clamp(fogSkyBlend_, 0.0f, 1.0f));
        newParams.zoneFogColor = glm::mix(newParams.zoneFogColor, newParams.skyMiddleColor,
                                          glm::clamp(fogSkyBlend_, 0.0f, 1.0f));
    }

    // How much fog, as a multiplier on the distances the zone asks for. 1.0,
    // the default, is the client's fog; 2.0 puts the same gradient at half the
    // distance, 0.5 doubles it, and zero is no fog at all.
    const float strength = glm::clamp(fogStrength_, 0.0f, 2.0f);
    if (strength <= 0.001f) {
        newParams.fogStart = 1.0e6f;
        newParams.fogEnd = 1.0e6f + 1.0f;
    } else if (std::abs(strength - 1.0f) > 0.001f) {
        const float scale = 1.0f / strength;
        newParams.fogStart *= scale;
        newParams.fogEnd *= scale;
    }

    // Ambient and diffuse go out as the DBC has them. They used to be scaled
    // down together whenever they summed past 1, which the client never does
    // (0x007816f0 hands the raw colours to the shaders); it darkened most
    // zones at noon by up to half.

    // Which sky models are overhead, and how much of each: this frame's
    // weights as they are (0x007f3230 writes them to 0xd38b70 every frame).
    // A border crossing still fades, because the light weights it comes from
    // fall off linearly with distance. A light with no sky model contributes
    // to no layer.
    {
        const auto flagsOf = [&](uint32_t skyboxId) {
            auto f = lightSkyboxFlags_.find(skyboxId);
            return f == lightSkyboxFlags_.end() ? 0u : f->second;
        };
        skyboxLayers_.clear();
        for (const auto& [skyboxId, weight] : skyTargets) {
            auto skyIt = lightSkyboxPaths_.find(skyboxId);
            if (skyIt == lightSkyboxPaths_.end() || skyIt->second.empty()) continue;
            skyboxLayers_.push_back({.path = skyIt->second, .weight = weight,
                                     .flags = flagsOf(skyboxId), .deathOverride = false});
        }
        if (deathSkyboxId != 0) {
            auto skyIt = lightSkyboxPaths_.find(deathSkyboxId);
            if (skyIt != lightSkyboxPaths_.end() && !skyIt->second.empty()) {
                std::erase_if(skyboxLayers_,
                              [&](const SkyboxLayer& l) { return l.path == skyIt->second; });
                skyboxLayers_.push_back({.path = skyIt->second, .weight = 1.0f,
                                         .flags = flagsOf(deathSkyboxId), .deathOverride = true});
            }
        }
        std::sort(skyboxLayers_.begin(), skyboxLayers_.end(),
                  [](const SkyboxLayer& a, const SkyboxLayer& b) { return a.weight > b.weight; });
    }

    // What the sky is being told, whenever it changes. Only on a change, and
    // rate limited, so standing still is silent; INFO, so it costs nothing
    // until somebody asks for it with WOWEE_LOG_LEVEL=info.
    {
        const float skyLuma = 0.2126f * newParams.skyTopColor.r +
                              0.7152f * newParams.skyTopColor.g +
                              0.0722f * newParams.skyTopColor.b;
        const std::string topSkybox = skyboxLayers_.empty() ? std::string() : skyboxLayers_.front().path;
        // The two lights applied last, which show the most.
        uint32_t firstVolume = 0, secondVolume = 0;
        const size_t n = activeVolumes_.size();
        if (n > 0) firstVolume = activeVolumes_[n - 1].volume->lightId;
        if (n > 1) secondVolume = activeVolumes_[n - 2].volume->lightId;
        const float hours = timeOfDay_ * 24.0f;
        const bool changed =
            topSkybox != diagSkyboxPath_ ||
            firstVolume != diagFirstVolume_ || secondVolume != diagSecondVolume_ ||
            std::abs(hours - diagHours_) > 0.02f ||
            std::abs(skyLuma - diagSkyLuma_) > 0.01f;
        ++diagCallsSinceLog_;
        if (changed && diagCallsSinceLog_ >= 30) {
            diagCallsSinceLog_ = 0;
            LOG_INFO("sky: map=", mapId, " hour=", hours,
                     " skyLuma=", skyLuma, " nearest=", firstVolume, "/", secondVolume,
                     " inRange=", n, " storm=", storm, " inLiquid=", cameraInLiquid,
                     " skybox=", topSkybox.empty() ? "-" : topSkybox);
            diagSkyboxPath_ = topSkybox;
            diagFirstVolume_ = firstVolume;
            diagSecondVolume_ = secondVolume;
            diagHours_ = hours;
            diagSkyLuma_ = skyLuma;
        }
    }

    // This frame's light as it is. The client copies what it blended
    // straight into the light it draws with (0x007f3230 -> 0x007ed910) and
    // eases nothing over time; only a scripted light override fades in and
    // out (0x007f1360, modes 1 and 2). Moving between lights is already
    // gradual through their linear falloff. This used to chase the blend at
    // exp(-5 dt), so a teleport, a death or a cave mouth took a second or
    // more to arrive at a light the client shows at once.
    currentParams_ = newParams;
}

std::vector<LightingManager::WeightedVolume> LightingManager::findLightVolumes(const glm::vec3& playerPos, uint32_t mapId) const {
    auto it = lightVolumesByMap_.find(mapId);
    if (it == lightVolumesByMap_.end() || it->second.empty()) {
        return {};
    }

    // Every light within its outer radius, as 0x007f1360 gathers them: lights
    // under 3 yards across are skipped, and the default is not among them.
    struct Near {
        const LightVolume* volume;
        float distSq;
    };
    std::vector<Near> near;
    for (const auto& volume : it->second) {
        if (volume.outerRadius < daynight::kMinLightOuterRadius) continue;
        const glm::vec3 toPlayer = playerPos - volume.position;
        const float distSq = glm::dot(toPlayer, toPlayer);
        if (distSq < volume.outerRadius * volume.outerRadius) {
            near.push_back({&volume, distSq});
        }
    }

    // Farthest first, so the nearest is applied last and shows most
    // (0x007ed0a0); co-located lights by inner radius, larger first. The
    // light id settles exact ties so the order cannot flicker between frames.
    std::sort(near.begin(), near.end(), [](const Near& a, const Near& b) {
        if (daynight::lightAppliedBefore(a.distSq, a.volume->innerRadius, a.volume->position,
                                         b.distSq, b.volume->innerRadius, b.volume->position)) {
            return true;
        }
        if (daynight::lightAppliedBefore(b.distSq, b.volume->innerRadius, b.volume->position,
                                         a.distSq, a.volume->innerRadius, a.volume->position)) {
            return false;
        }
        return a.volume->lightId < b.volume->lightId;
    });

    std::vector<WeightedVolume> weighted;
    weighted.reserve(near.size());
    for (const Near& n : near) {
        const float w = daynight::lightFalloffWeight(std::sqrt(n.distSq), n.volume->innerRadius,
                                                     n.volume->outerRadius);
        weighted.push_back({.volume = n.volume, .weight = w});
    }

    // Which lights this map turned out to have around the player, once per
    // map rather than once per frame.
    if (mapId != diagLoggedMapId_) {
        diagLoggedMapId_ = mapId;
        std::string named;
        for (size_t i = 0; i < weighted.size() && i < 3; ++i) {
            const LightVolume& v = *weighted[weighted.size() - 1 - i].volume;
            named += " [" + std::to_string(v.lightId) + " inner=" +
                     std::to_string(static_cast<int>(v.innerRadius)) + " outer=" +
                     std::to_string(static_cast<int>(v.outerRadius)) + "]";
        }
        const LightVolume* def = defaultLight(mapId);
        LOG_INFO("Light volumes on map ", mapId, ": default ",
                 def ? std::to_string(def->lightId) : std::string("none"), ", ",
                 weighted.size(), " in range,", named.empty() ? " none" : named);
    }

    return weighted;
}

const LightVolume* LightingManager::defaultLight(uint32_t mapId) const {
    auto it = defaultLightByMap_.find(mapId);
    if (it != defaultLightByMap_.end()) return &it->second;
    return hasGlobalDefaultLight_ ? &globalDefaultLight_ : nullptr;
}

const LightParamsProfile* LightingManager::profileFor(const LightVolume& volume, uint32_t slot) const {
    // The slot asked for, then the non-storm one it stands for, then normal.
    const uint32_t tries[3] = {slot, slot >= 2 ? slot - 2 : slot, LightVolume::PARAMS_NORMAL};
    for (uint32_t s : tries) {
        if (s >= LightVolume::PARAMS_SLOT_COUNT || volume.lightParamsIds[s] == 0) continue;
        auto it = lightParamsProfiles_.find(volume.lightParamsIds[s]);
        if (it != lightParamsProfiles_.end()) return &it->second;
    }
    return nullptr;
}

LightingParams LightingManager::sampleLight(const LightVolume& volume, bool cameraInLiquid,
                                            float storm, uint16_t timeHalfMinutes,
                                            uint32_t& skyboxId) const {
    const uint32_t slot = cameraInLiquid ? LightVolume::PARAMS_UNDERWATER : LightVolume::PARAMS_NORMAL;
    const LightParamsProfile* profile = profileFor(volume, slot);
    skyboxId = profile ? profile->lightSkyboxId : 0;
    LightingParams params = sampleLightParams(profile, timeHalfMinutes);
    if (storm > 0.0f) {
        const LightParamsProfile* stormProfile = profileFor(volume, slot + 2);
        params = lerpLightingParams(params, sampleLightParams(stormProfile, timeHalfMinutes), storm);
    }
    return params;
}

LightingParams LightingManager::sampleLightParams(const LightParamsProfile* profile, uint16_t timeHalfMinutes) const {
    if (!profile) return fallbackParams_;

    LightingParams params;

    // Sample color bands
    params.ambientColor = sampleColorBand(profile->colorBands[LightParamsProfile::AMBIENT_COLOR], timeHalfMinutes);
    params.diffuseColor = sampleColorBand(profile->colorBands[LightParamsProfile::DIFFUSE_COLOR], timeHalfMinutes);
    params.fogColor = sampleColorBand(profile->colorBands[LightParamsProfile::FOG_COLOR], timeHalfMinutes);
    params.skyTopColor = sampleColorBand(profile->colorBands[LightParamsProfile::SKY_TOP_COLOR], timeHalfMinutes);
    params.skyMiddleColor = sampleColorBand(profile->colorBands[LightParamsProfile::SKY_MIDDLE_COLOR], timeHalfMinutes);
    params.skyBand1Color = sampleColorBand(profile->colorBands[LightParamsProfile::SKY_BAND1_COLOR], timeHalfMinutes);
    params.skyBand2Color = sampleColorBand(profile->colorBands[LightParamsProfile::SKY_BAND2_COLOR], timeHalfMinutes);
    params.skySmogColor = sampleColorBand(profile->colorBands[LightParamsProfile::SKY_SMOG_COLOR], timeHalfMinutes);
    params.shadowOpacity = sampleColorBand(profile->colorBands[LightParamsProfile::SHADOW_COLOR], timeHalfMinutes).r;
    params.sunColor = sampleColorBand(profile->colorBands[LightParamsProfile::SUN_COLOR], timeHalfMinutes);
    params.cloudSunColor = sampleColorBand(profile->colorBands[LightParamsProfile::CLOUD_SUN_COLOR], timeHalfMinutes);
    params.cloudShadeColor = sampleColorBand(profile->colorBands[LightParamsProfile::CLOUD_SHADE_COLOR], timeHalfMinutes);
    params.cloudBaseColor = sampleColorBand(profile->colorBands[LightParamsProfile::CLOUD_BASE_COLOR], timeHalfMinutes);
    params.oceanCloseColor = sampleColorBand(profile->colorBands[LightParamsProfile::OCEAN_CLOSE_COLOR], timeHalfMinutes);
    params.oceanFarColor = sampleColorBand(profile->colorBands[LightParamsProfile::OCEAN_FAR_COLOR], timeHalfMinutes);
    params.riverCloseColor = sampleColorBand(profile->colorBands[LightParamsProfile::RIVER_CLOSE_COLOR], timeHalfMinutes);
    params.riverFarColor = sampleColorBand(profile->colorBands[LightParamsProfile::RIVER_FAR_COLOR], timeHalfMinutes);

    // Sample float bands. The fog distance is stored in the same
    // thirty-sixths of a yard as the light positions, and was being used raw:
    // Tirisfal's 12000 became 12000 yards, so the fog ended six times further
    // out than the far clip and nothing was ever hazed. It is 333 yards.
    //
    // The start is a fraction of the end rather than a distance. It is kept
    // as the fraction through the blend and turned into a distance at the
    // end, as the client does (0x007ebff0 stores it, 0x007f16f0 applies it).
    const daynight::LightFog fog = daynight::clientLightFog(
        sampleFloatBand(profile->floatBands[LightParamsProfile::FOG_END], timeHalfMinutes) /
            LIGHT_COORD_UNITS_PER_YARD,
        sampleFloatBand(profile->floatBands[LightParamsProfile::FOG_START_SCALAR], timeHalfMinutes),
        farClip_, fogExponent_);
    params.fogEnd = fog.end;
    params.fogStartScalar = fog.startScalar;
    params.fogExponent = fog.exponent;
    params.fogStart = params.fogEnd * params.fogStartScalar;
    params.cloudDensity = sampleFloatBand(profile->floatBands[LightParamsProfile::CLOUD_DENSITY], timeHalfMinutes);

    params.highlightSky = profile->highlightSky;
    params.glow = profile->glow;
    params.waterShallowAlpha = profile->waterShallowAlpha;
    params.waterDeepAlpha = profile->waterDeepAlpha;
    params.oceanShallowAlpha = profile->oceanShallowAlpha;
    params.oceanDeepAlpha = profile->oceanDeepAlpha;

    return params;
}

glm::vec3 LightingManager::sampleColorBand(const ColorBand& band, uint16_t timeHalfMinutes) const {
    if (band.numKeyframes == 0) {
        return glm::vec3(0.0f);  // black, as 0x007eb070 answers for no keys
    }

    if (band.numKeyframes == 1) {
        return band.colors[0];  // Single keyframe
    }

    // Safer initialization: default to wrapping last→first
    uint8_t idx1 = band.numKeyframes - 1;
    uint8_t idx2 = 0;

    // Find surrounding keyframes
    for (uint8_t i = 0; i < band.numKeyframes; ++i) {
        if (timeHalfMinutes < band.times[i]) {
            idx2 = i;
            idx1 = (i > 0) ? (i - 1) : (band.numKeyframes - 1);  // Wrap to last
            break;
        }
    }

    // Calculate interpolation factor
    uint16_t t1 = band.times[idx1];
    uint16_t t2 = band.times[idx2];

    // Handle midnight wrap
    uint16_t timeSpan = (t2 > t1) ? (t2 - t1) : (kHalfMinutesPerDay - t1 + t2);
    uint16_t elapsed = (timeHalfMinutes >= t1) ? (timeHalfMinutes - t1) : (kHalfMinutesPerDay - t1 + timeHalfMinutes);

    float t = (timeSpan > 0) ? (static_cast<float>(elapsed) / static_cast<float>(timeSpan)) : 0.0f;
    t = glm::clamp(t, 0.0f, 1.0f);

    // Linear interpolation
    return glm::mix(band.colors[idx1], band.colors[idx2], t);
}

float LightingManager::sampleFloatBand(const FloatBand& band, uint16_t timeHalfMinutes) const {
    if (band.numKeyframes == 0) {
        return 0.0f;  // as 0x007eaef0 answers for no keys
    }

    if (band.numKeyframes == 1) {
        return band.values[0];
    }

    // Safer initialization: default to wrapping last→first
    uint8_t idx1 = band.numKeyframes - 1;
    uint8_t idx2 = 0;

    // Find surrounding keyframes
    for (uint8_t i = 0; i < band.numKeyframes; ++i) {
        if (timeHalfMinutes < band.times[i]) {
            idx2 = i;
            idx1 = (i > 0) ? (i - 1) : (band.numKeyframes - 1);
            break;
        }
    }

    uint16_t t1 = band.times[idx1];
    uint16_t t2 = band.times[idx2];

    uint16_t timeSpan = (t2 > t1) ? (t2 - t1) : (kHalfMinutesPerDay - t1 + t2);
    uint16_t elapsed = (timeHalfMinutes >= t1) ? (timeHalfMinutes - t1) : (kHalfMinutesPerDay - t1 + timeHalfMinutes);

    float t = (timeSpan > 0) ? (static_cast<float>(elapsed) / static_cast<float>(timeSpan)) : 0.0f;
    t = glm::clamp(t, 0.0f, 1.0f);

    return glm::mix(band.values[idx1], band.values[idx2], t);
}

glm::vec3 LightingManager::dbcColorToVec3(uint32_t dbcColor) const {
    // Red is the high byte: the packed value is 0x00RRGGBB. Reading it the
    // other way round swaps red and blue, which turns Teldrassil's violet
    // canopies magenta and leaves every zone's light fighting its own sky.
    //
    // Checked against the file rather than the layout's name for it. Over the
    // 844 LightParams rows that carry a first channel, taking red from the
    // high byte makes 66% of them warm at noon and 64% blue at midnight;
    // taking it from the low byte gives 35% and 41%, which is worse than
    // chance in both directions - the mark of a colour read backwards.
    const uint8_t r = (dbcColor >> 16) & 0xFF;
    const uint8_t g = (dbcColor >> 8) & 0xFF;
    const uint8_t b = dbcColor & 0xFF;

    return glm::vec3(r / 255.0f, g / 255.0f, b / 255.0f);
}

} // namespace rendering
} // namespace wowee
