#pragma once

#include <vector>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <glm/glm.hpp>

namespace wowee {
namespace pipeline { class DBCFile; class AssetManager; }

namespace rendering {

/**
 * Time-of-day lighting parameters sampled from DBC curves
 */
struct LightingParams {
    glm::vec3 ambientColor{0.4f, 0.4f, 0.5f};      // ch1: fill lighting
    glm::vec3 diffuseColor{1.0f, 0.95f, 0.8f};     // ch0: directional sun/moon colour
    /// The way the directional light travels, render space, unit length. Not
    /// from the DBC: the client's fixed time curve (daynight::directionalLightDir,
    /// 0x007eea90).
    glm::vec3 directionalDir{0.0f, -1.0f, 0.5f};
    /// From the eye toward the sun and the moon sprites (0x007eecc0), render
    /// space, unit length. Their own curves, on the light's side of the sky.
    glm::vec3 sunDir{0.0f, 0.0f, 1.0f};
    glm::vec3 moonDir{0.0f, 0.0f, -1.0f};

    glm::vec3 fogColor{0.5f, 0.6f, 0.7f};          // ch7: fog, and the sky's horizon
    float fogStart = 100.0f;                        // Fog start distance (yards)
    float fogEnd = 1000.0f;                         // Fog end distance (yards)
    /// Float band 1: fogStart as a fraction of fogEnd. The client blends the
    /// lights' end and fraction and only then works out the start (0x007f16f0).
    float fogStartScalar = 0.1f;
    /// The power the fog is drawn with (DNInfo[0x14]): 1, linear, before map
    /// 530; from there on worked out from each light's authored range and
    /// lerped with the rest (0x007ecd80, 0x007ed4c0).
    float fogExponent = 1.0f;

    glm::vec3 skyTopColor{0.5f, 0.7f, 1.0f};       // ch2: sky zenith
    glm::vec3 skyMiddleColor{0.7f, 0.85f, 1.0f};   // ch3
    glm::vec3 skyBand1Color{0.9f, 0.95f, 1.0f};    // ch4
    glm::vec3 skyBand2Color{1.0f, 0.98f, 0.9f};    // ch5
    glm::vec3 skySmogColor{0.7f, 0.7f, 0.7f};      // ch6: the band just above the horizon

    /// ch8's red: how opaque the terrain's baked shadows (MCSH) are drawn
    /// (0x007ee750 pairs it with ambient / 3 as the shadow colour).
    float shadowOpacity = 1.0f;
    /// ch9: the sun and moon sprites' colour (0x007f3230 copies DNInfo[9]
    /// into the sun, moon and Blue Child quads, 0xd38e34/e54/f70).
    glm::vec3 sunColor{1.0f, 1.0f, 1.0f};
    /// The cloud texture's three colours (0x007efae0): ch10 where the sun
    /// lights it, ch11 by how thin the cloud is, ch12 underneath everything.
    glm::vec3 cloudSunColor{1.0f, 1.0f, 1.0f};     // ch10
    glm::vec3 cloudShadeColor{0.5f, 0.5f, 0.5f};   // ch11
    glm::vec3 cloudBaseColor{0.3f, 0.3f, 0.3f};    // ch12
    /// The water's colours (0x008a2bf0 builds the liquid colour ramps from
    /// them): ocean from ch14 in the shallows to ch15 in the deep, rivers and
    /// lakes from ch16 to ch17.
    glm::vec3 oceanCloseColor{0.1f, 0.3f, 0.5f};   // ch14
    glm::vec3 oceanFarColor{0.04f, 0.16f, 0.38f};  // ch15
    glm::vec3 riverCloseColor{0.1f, 0.3f, 0.5f};   // ch16
    glm::vec3 riverFarColor{0.1f, 0.28f, 0.55f};   // ch17

    /// Float band 3: cloud cover, 0 clear to 1 overcast (0x007efd00 reads it
    /// at 0xd38c34 as the cloud texture's coverage).
    float cloudDensity = 0.3f;

    /// LightParams' own values, lerped with the rest (0x007ec220). Defaults
    /// are what 0x007f3230 puts in when there is no light at all.
    float highlightSky = 0.0f;       ///< field 1: the dawn and dusk sky glow
    float glow = 0.5f;               ///< field 4: the full-screen glow's strength
    float waterShallowAlpha = 0.5f;  ///< field 5
    float waterDeepAlpha = 1.0f;     ///< field 6
    float oceanShallowAlpha = 0.75f; ///< field 7
    float oceanDeepAlpha = 1.0f;     ///< field 8
};

/// `a` moved `w` of the way toward `b`, as the client applies one light over
/// what is already there (0x007ed4c0). The directions are the client's curves
/// and the same in both, so they are left as `a` has them.
inline LightingParams lerpLightingParams(const LightingParams& a, const LightingParams& b, float w) {
    LightingParams out = a;
    out.ambientColor = glm::mix(a.ambientColor, b.ambientColor, w);
    out.diffuseColor = glm::mix(a.diffuseColor, b.diffuseColor, w);
    out.fogColor = glm::mix(a.fogColor, b.fogColor, w);
    out.skyTopColor = glm::mix(a.skyTopColor, b.skyTopColor, w);
    out.skyMiddleColor = glm::mix(a.skyMiddleColor, b.skyMiddleColor, w);
    out.skyBand1Color = glm::mix(a.skyBand1Color, b.skyBand1Color, w);
    out.skyBand2Color = glm::mix(a.skyBand2Color, b.skyBand2Color, w);
    out.skySmogColor = glm::mix(a.skySmogColor, b.skySmogColor, w);
    out.shadowOpacity = glm::mix(a.shadowOpacity, b.shadowOpacity, w);
    out.sunColor = glm::mix(a.sunColor, b.sunColor, w);
    out.cloudSunColor = glm::mix(a.cloudSunColor, b.cloudSunColor, w);
    out.cloudShadeColor = glm::mix(a.cloudShadeColor, b.cloudShadeColor, w);
    out.cloudBaseColor = glm::mix(a.cloudBaseColor, b.cloudBaseColor, w);
    out.oceanCloseColor = glm::mix(a.oceanCloseColor, b.oceanCloseColor, w);
    out.oceanFarColor = glm::mix(a.oceanFarColor, b.oceanFarColor, w);
    out.riverCloseColor = glm::mix(a.riverCloseColor, b.riverCloseColor, w);
    out.riverFarColor = glm::mix(a.riverFarColor, b.riverFarColor, w);
    out.fogStart = glm::mix(a.fogStart, b.fogStart, w);
    out.fogEnd = glm::mix(a.fogEnd, b.fogEnd, w);
    out.fogStartScalar = glm::mix(a.fogStartScalar, b.fogStartScalar, w);
    out.fogExponent = glm::mix(a.fogExponent, b.fogExponent, w);
    out.cloudDensity = glm::mix(a.cloudDensity, b.cloudDensity, w);
    out.highlightSky = glm::mix(a.highlightSky, b.highlightSky, w);
    out.glow = glm::mix(a.glow, b.glow, w);
    out.waterShallowAlpha = glm::mix(a.waterShallowAlpha, b.waterShallowAlpha, w);
    out.waterDeepAlpha = glm::mix(a.waterDeepAlpha, b.waterDeepAlpha, w);
    out.oceanShallowAlpha = glm::mix(a.oceanShallowAlpha, b.oceanShallowAlpha, w);
    out.oceanDeepAlpha = glm::mix(a.oceanDeepAlpha, b.oceanDeepAlpha, w);
    return out;
}

/**
 * Light volume from Light.dbc (spatial lighting)
 */
struct LightVolume {
    uint32_t lightId = 0;
    uint32_t mapId = 0;
    glm::vec3 position{0.0f};  // World position (note: DBC stores as x,z,y!)
    float innerRadius = 0.0f;   // Full weight radius
    float outerRadius = 0.0f;   // Fade-out radius

    /// Light.dbc's LightParams columns, the first five of which the client
    /// reads by index (0x007eb180, with the slot 0x007f3230 picks): 1 when the
    /// camera is in liquid, +2 for the storm set it blends toward in weather,
    /// and 4 for the death override.
    enum ParamsSlot : uint32_t {
        PARAMS_NORMAL = 0,
        PARAMS_UNDERWATER = 1,
        PARAMS_STORM = 2,
        PARAMS_STORM_UNDERWATER = 3,
        PARAMS_DEATH = 4,
        PARAMS_SLOT_COUNT = 5
    };
    uint32_t lightParamsIds[PARAMS_SLOT_COUNT] = {};
};

/**
 * Color band with time-of-day keyframes
 */
struct ColorBand {
    uint8_t numKeyframes = 0;
    uint16_t times[16];        // Time keyframes (half-minutes since midnight)
    glm::vec3 colors[16];      // Color values (RGB 0-1)
};

/**
 * Float band with time-of-day keyframes
 */
struct FloatBand {
    uint8_t numKeyframes = 0;
    uint16_t times[16];        // Time keyframes (half-minutes since midnight)
    float values[16];          // Float values
};

/**
 * LightParams profile with 18 color bands + 6 float bands
 */
struct LightParamsProfile {
    uint32_t lightParamsId = 0;
    uint32_t lightSkyboxId = 0;
    /// LightParams.dbc's constants (0x007ebff0 copies them into DNInfo
    /// 0x15, 0x16 and 0x1b-0x1e), defaults as 0x007f3230 has them.
    float highlightSky = 0.0f;
    float glow = 0.5f;
    float waterShallowAlpha = 0.5f;
    float waterDeepAlpha = 1.0f;
    float oceanShallowAlpha = 0.75f;
    float oceanDeepAlpha = 1.0f;

    // 18 color channels (IntBand)
    //
    // Read off the file rather than from the order they are usually listed in.
    // The sky gradient anchors the rest: for Stormwind at noon, channels 2 to 5
    // are (0,31,73), (58,162,207), (153,220,245), (175,218,224) - a deep blue
    // overhead paling to the horizon, which can only be the sky and fixes every
    // index around it.
    //
    // Channel 0 is not the ambient. Across zones it is the warm, bright one -
    // (199,168,134) over Dun Morogh's snow, (255,224,169) in Teldrassil,
    // (255,136,0) in Stormwind - while channel 1 is the dark cool one that
    // carries the zone's cast: (31,82,125) in Dun Morogh, violet (125,72,130)
    // in Teldrassil and Winterspring. A scene ambient multiplies every surface,
    // so reading channel 0 into it put Stormwind's orange over the whole city
    // and left it looking like Durotar, whose channel 0 is (255,204,148).
    //
    // Channel 7 is the fog. Channel 6 is the sky's smog layer and is a neutral
    // grey - (180,180,180) at Stormwind noon - which is the pale grey the fog
    // blend further down was written to work around.
    enum ColorChannel {
        DIFFUSE_COLOR = 0,
        AMBIENT_COLOR = 1,
        SKY_TOP_COLOR = 2,
        SKY_MIDDLE_COLOR = 3,
        SKY_BAND1_COLOR = 4,
        SKY_BAND2_COLOR = 5,
        SKY_SMOG_COLOR = 6,
        FOG_COLOR = 7,
        SHADOW_COLOR = 8,
        SUN_COLOR = 9,
        CLOUD_SUN_COLOR = 10,
        CLOUD_SHADE_COLOR = 11,
        CLOUD_BASE_COLOR = 12,
        OCEAN_CLOSE_COLOR = 14,
        OCEAN_FAR_COLOR = 15,
        RIVER_CLOSE_COLOR = 16,
        RIVER_FAR_COLOR = 17,
        COLOR_CHANNEL_COUNT = 18
    };

    ColorBand colorBands[COLOR_CHANNEL_COUNT];

    // 6 float channels (FloatBand)
    enum FloatChannel {
        FOG_END = 0,
        FOG_START_SCALAR = 1,  // Multiplier for fog start
        // Channel 2 is not a density. Measured over every band in the file,
        // 98% of its 2509 samples are exactly 1.0 - it is the switch for how
        // much the sun and moon show through cloud, and reading it as cloud
        // density left almost every zone under solid overcast at all hours,
        // which is what hid the skybox.
        CELESTIAL_GLOW_THROUGH = 2,
        // Channel 3 is the curve that actually varies: 0 to 5, mean 0.50.
        CLOUD_DENSITY = 3,
        // ... more channels
        FLOAT_CHANNEL_COUNT = 6
    };

    FloatBand floatBands[FLOAT_CHANNEL_COUNT];
};

/**
 * WoW DBC-driven lighting manager
 *
 * The client's DayNight light selection (Wow.exe 3.3.5a):
 * - Loads Light.dbc, LightParams.dbc, LightIntBand.dbc, LightFloatBand.dbc
 * - Starts from the map's default light (0x007ecb30) and lerps every nearby
 *   light over it, farthest first (0x007f1360, 0x007ee5d0)
 * - Samples each light's normal or underwater set, blended toward its storm
 *   set by the weather (0x007ee510, 0x007f3920)
 * - Places the light, the sun and the moon on the client's time curves
 *   (0x007eea90, 0x007eecc0)
 */
/// The liquid at the camera, as 0x00780620 reports it to 0x007f3230.
struct CameraLiquid {
    bool submerged = false;     ///< the camera is under the surface
    uint32_t liquidType = 0;    ///< LiquidType.dbc ID, 0 if not known
    float depth = 0.0f;         ///< yards from the surface down to the camera
};

class LightingManager {
public:
    using CameraLiquid = rendering::CameraLiquid;

    LightingManager();
    ~LightingManager();

    /**
     * Initialize lighting system and load DBCs
     */
    bool initialize(pipeline::AssetManager* assetManager);

    /**
     * Update lighting for current time and position
     * @param playerPos Position the lights are measured from
     * @param mapId Current map ID
     * @param gameTime Server game time in hours since midnight (-1: local time)
     * @param weatherIntensity Weather intensity 0-1, any kind of weather; the
     *        storm sets are blended in by min(1, 4 * intensity)
     * @param liquid The liquid the camera is under, if any: picks the
     *        underwater sets, and its LiquidType row darkens the light with
     *        depth or replaces it (0x007f3230)
     * @param deathOverride Whether the death light is up (the player is a
     *        ghost): the map default light's death set replaces the blend
     *        and its sky model goes up at full weight (0x007f3230 with the
     *        override index ScreenEffect sets, 0x007ecec0)
     */
    void update(const glm::vec3& playerPos, uint32_t mapId,
                float gameTime = -1.0f,
                float weatherIntensity = 0.0f, const CameraLiquid& liquid = {},
                bool deathOverride = false);

    /**
     * Get current lighting parameters
     */
    [[nodiscard]] const LightingParams& getLightingParams() const { return currentParams_; }

    /// How far the distance fog is pulled toward the sky's middle band. Not
    /// something the client does (its fog is ch7 exactly, 0x007f16f0): an
    /// opt-in setting, 0 by default.
    void setFogSkyBlend(float blend) { fogSkyBlend_ = blend; }
    [[nodiscard]] float getFogSkyBlend() const { return fogSkyBlend_; }
    /// How much distance fog, as a multiplier on the zone's own fog distances.
    /// 1 is the client's fog unchanged and the default, above 1 is thicker,
    /// 0 is none. See LightingManager::update.
    void setFogStrength(float strength) { fogStrength_ = strength; }
    [[nodiscard]] float getFogStrength() const { return fogStrength_; }
    /// The far clip the fog end is kept inside, as the client keeps it
    /// (0x007f16f0). 0 or less leaves the fog end alone.
    void setFarClip(float farClip) { farClip_ = farClip; }

    /// The WMO interior fog (MFOG) at the camera, if it is inside one: the
    /// zone's fog gives way to it over the first 25 yards in (0x007f16f0,
    /// 0x007ed1b0). Taken by the next update().
    struct InteriorFog {
        float end = 0.0f;
        float startScalar = 0.0f;
        glm::vec3 color{0.0f};
        /// The record's second fog, for the camera in liquid (MFOG +0x24).
        float liquidEnd = 0.0f;
        float liquidStartScalar = 0.0f;
        glm::vec3 liquidColor{0.0f};
        /// The MFOG flags (0x10, 0x100 read by 0x007f16f0).
        uint32_t flags = 0;
        float distanceInside = 0.0f;
    };
    void setInteriorFog(const std::optional<InteriorFog>& fog) { interiorFog_ = fog; }

    /**
     * Get current time of day (0.0-1.0)
     */
    [[nodiscard]] float getTimeOfDay() const { return timeOfDay_; }

    /** The time of day in hours, 0-24. */
    [[nodiscard]] float getTimeOfDayHours() const { return timeOfDay_ * 24.0f; }

    /// One of the original client's sky models, and how much of it is up.
    struct SkyboxLayer {
        std::string path;
        float weight = 0.0f;   ///< 0..1, as the client accumulates it
        /// LightSkybox.dbc's flags (field 2). 0x1: the model's animation runs
        /// with the time of day (0x007ecf20). 0x2: the procedural sky - dome,
        /// sun, moons and clouds - is drawn under it too (0x007f09b0).
        uint32_t flags = 0;
        /// The death light's model: up at full weight whatever the flags say,
        /// and the procedural sky goes (0x007efd00 tests 0xd38b5c alone).
        bool deathOverride = false;
    };
    static constexpr uint32_t kSkyboxFlagTimeOfDayAnim = 0x1;
    static constexpr uint32_t kSkyboxFlagCombineProcedural = 0x2;
    /// Whether the procedural sky - the dome, the sun and moons, the clouds -
    /// stays down under these models: some model is up past 0.99 without
    /// flag 0x2, or the death model is (0x007f09b0, 0x007efd00).
    [[nodiscard]] static bool skyboxHidesProceduralSky(const std::vector<SkyboxLayer>& layers) {
        for (const auto& l : layers) {
            if (l.weight > 0.99f && (l.deathOverride || (l.flags & kSkyboxFlagCombineProcedural) == 0)) {
                return true;
            }
        }
        return false;
    }
    /// How much the sun and moon glare is faded by the sky models: the death
    /// model's weight while it is up, else the heaviest one's (0x007ef6e0).
    [[nodiscard]] static float skyboxGlareWeight(const std::vector<SkyboxLayer>& layers) {
        float heaviest = 0.0f;
        for (const auto& l : layers) {
            if (l.deathOverride && l.weight > 0.0f) return l.weight < 1.0f ? l.weight : 1.0f;
            if (l.weight > heaviest) heaviest = l.weight;
        }
        return heaviest < 1.0f ? heaviest : 1.0f;
    }
    /// Every sky model the lights around the player name, heaviest first. The
    /// default light's model is up at 1; each nearby light adds its own weight
    /// to the model it names, at most 1, up to three models (0x007ed4c0).
    /// This frame's weights, as the client keeps them (0x007f3230).
    [[nodiscard]] const std::vector<SkyboxLayer>& getSkyboxLayers() const { return skyboxLayers_; }

    /**
     * Manually set time of day for testing
     */
    void setTimeOfDay(float tod) { timeOfDay_ = tod; manualTime_ = true; }

    /**
     * Use real time for day/night cycle
     */
    void useRealTime(bool use) { manualTime_ = !use; }

private:
    /**
     * Load Light.dbc
     */
    bool loadLightDbc(pipeline::AssetManager* assetManager);

    /**
     * Load LightParams.dbc for zone→light mapping
     */
    bool loadLightParamsDbc(pipeline::AssetManager* assetManager);

    bool loadLightSkyboxDbc(pipeline::AssetManager* assetManager);
    /// LiquidType.dbc's darkening and light columns (6-10).
    bool loadLiquidTypeDbc(pipeline::AssetManager* assetManager);

    /**
     * Load LightIntBand.dbc and LightFloatBand.dbc for time curves
     */
    bool loadLightBandDbcs(pipeline::AssetManager* assetManager);

    /// A light near the player and how much of it shows, in the order the
    /// client applies them: farthest first.
    struct WeightedVolume {
        const LightVolume* volume = nullptr;
        float weight = 0.0f;
    };

    /// Every light on the map within its outer radius of `playerPos`, other
    /// than the default, in the client's order (0x007f1360, 0x007ed0a0).
    [[nodiscard]] std::vector<WeightedVolume> findLightVolumes(const glm::vec3& playerPos, uint32_t mapId) const;

    /// The map's default light: its Light row at (0,0,0), or Light ID 1 when
    /// it has none (0x007ecb30). Null when neither exists.
    [[nodiscard]] const LightVolume* defaultLight(uint32_t mapId) const;

    /// One light's LightParams profile for a slot, falling back to the
    /// matching non-storm set and then the normal one when a column is empty.
    [[nodiscard]] const LightParamsProfile* profileFor(const LightVolume& volume, uint32_t slot) const;

    /// One light as the client samples it at this time (0x007ee510): the
    /// normal or underwater set, lerped toward the storm set by `storm`.
    /// `skyboxId` is the model the light names.
    [[nodiscard]] LightingParams sampleLight(const LightVolume& volume, bool cameraInLiquid,
                                             float storm, uint16_t timeHalfMinutes,
                                             uint32_t& skyboxId) const;

    /**
     * Sample lighting from LightParams profile
     */
    LightingParams sampleLightParams(const LightParamsProfile* profile, uint16_t timeHalfMinutes) const;

    /**
     * Sample color from band
     */
    [[nodiscard]] glm::vec3 sampleColorBand(const ColorBand& band, uint16_t timeHalfMinutes) const;

    /**
     * Sample float from band
     */
    [[nodiscard]] float sampleFloatBand(const FloatBand& band, uint16_t timeHalfMinutes) const;

    /**
     * Convert DBC BGR color to RGB vec3
     */
    [[nodiscard]] glm::vec3 dbcColorToVec3(uint32_t dbcColor) const;


    // Light volumes by map, the default lights not among them
    std::map<uint32_t, std::vector<LightVolume>> lightVolumesByMap_;
    // Each map's default light: its Light row at (0,0,0)
    std::map<uint32_t, LightVolume> defaultLightByMap_;
    // Light ID 1, the default for a map without one of its own
    LightVolume globalDefaultLight_;
    bool hasGlobalDefaultLight_ = false;

    // LightParams profiles by ID
    std::map<uint32_t, LightParamsProfile> lightParamsProfiles_;
    std::map<uint32_t, std::string> lightSkyboxPaths_;
    std::map<uint32_t, uint32_t> lightSkyboxFlags_;
    // Every Light row by ID, for a LiquidType's own light
    std::map<uint32_t, LightVolume> lightsById_;
    struct LiquidTypeLight {
        float maxDarkenDepth = 0.0f;
        float ambDarken = 0.0f;
        float dirDarken = 0.0f;
        uint32_t lightId = 0;
        uint32_t flags = 0;
    };
    std::map<uint32_t, LiquidTypeLight> liquidTypes_;

    // Current state
    LightingParams currentParams_;
    std::vector<WeightedVolume> activeVolumes_;
    float timeOfDay_ = 0.5f;  // Start at noon
    std::vector<SkyboxLayer> skyboxLayers_;
    float fogSkyBlend_ = 0.0f;
    float fogStrength_ = 1.0f;
    float farClip_ = 0.0f;
    /// This map draws the later fog (daynight::mapUsesFogExponent).
    bool fogExponent_ = false;
    std::optional<InteriorFog> interiorFog_;

    // Last values the sky diagnostic reported, so it prints on a change
    // rather than every frame. See LightingManager::update.
    /// The map the volume list was last named for, so it is named once per map
    /// rather than once per frame. findLightVolumes is const.
    mutable uint32_t diagLoggedMapId_ = 0xFFFFFFFFu;
    uint32_t diagCallsSinceLog_ = 0;
    uint32_t diagFirstVolume_ = 0xFFFFFFFFu;
    uint32_t diagSecondVolume_ = 0xFFFFFFFFu;
    float diagHours_ = -1.0f;
    float diagSkyLuma_ = -1.0f;
    std::string diagSkyboxPath_ = "\x01";
    bool manualTime_ = false;
    bool initialized_ = false;

    // Lighting when Light.dbc is missing altogether
    LightingParams fallbackParams_;
};

} // namespace rendering
} // namespace wowee
