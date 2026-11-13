#pragma once

/// What the quality presets mean, for both places that offer them.
///
/// The settings window and the login screen each had their own copy of these
/// numbers, and they had drifted: the same preset named the same thing meant a
/// different picture depending on which one you chose it from. High was 350
/// yards of shadow with 4x MSAA and no FXAA in game, and 250 yards with 2x and
/// FXAA on at the login screen; Medium differed in clutter and multisampling;
/// Low still turned shadows off there after the client stopped honouring that.
///
/// It carries no includes, so the login screen, the settings panel and a test
/// can all read the same rows.

#include "ui/graphics_defaults.hpp"

namespace wowee {
namespace ui {

/// What each quality preset means, in the order Low, Medium, High, Ultra,
/// Super-Ultra (the last added at the end, so the stored indices 0-4 keep
/// their meaning).
///
/// That order is what the config file holds. graphics_preset is written as an
/// index - 0 for Custom, then one per row here - so a preset added at the front
/// or in the middle changes what every saved file means, and a player who chose
/// Ultra comes back on High. Add at the end, or migrate the stored value.
///
/// The same is true of the antiAliasing column below: it is an index into a
/// dropdown whose order is also persisted, so a row here can be wrong in two
/// directions at once if either list is reordered. See the note on
/// SettingDesc::choices.
///
/// One row per preset, where there used to be a block per preset - the same
/// ten settings assigned and then pushed at the renderer four times over. The
/// blocks had drifted apart, as four copies of one fact do:
///
///   * Low set a shadow distance of 100 and never told the renderer, so the
///     field said 100 and the shadows stayed at whatever the last preset left.
///   * Only Ultra had an opinion about FXAA. Going from Ultra to Low turned
///     everything else down and left FXAA running.
///   * Low left the normal map strength at whatever it was, which does not
///     matter while normal mapping is off and does the moment it is switched
///     back on by hand.
///
/// Reading them as a table is also what lets a preset be recognised again
/// afterwards without writing the numbers out a second time.
struct GraphicsPresetValues {
    float viewDistance;
    bool  shadows;
    float shadowDistance;
    int   antiAliasing;      ///< index into the four the panel offers
    bool  fxaa;
    bool  normalMapping;
    float normalMapStrength;
    int   groundClutter;     ///< percent
    /// Ground cover from the terrain's own ground-effect data. Stated on every
    /// row rather than only the one that wants it: a preset that leaves a field
    /// alone is what left FXAA running after a drop from Ultra to Low.
    bool  grass;
    int   grassDensity;      ///< percent of what the terrain asks for
    int   grassHeight;       ///< percent
    int   grassDistance;     ///< yards
    /// The extras the original client does not have: the lens flare, the
    /// sharp-point starfield, sun shafts, light shafts and mist, fog tinted
    /// by the sky, and the enhanced water. Off on the client's own levels, Low
    /// to Ultra, so those give the client's own picture; Super-Ultra is the
    /// user's picture and has them on (2026-11-13). Each stays in the panel
    /// to be set by hand.
    float lensFlare;
    bool  sharpStars;
    bool  sunShafts;
    int   lightShafts;       ///< index into Off|Low|Medium|High
    float fogSkyBlend;
    /// The client's 'specular' cvar (registered at 0x0078e1a0 with the
    /// default "0" at 0x009e14a0). The exe holds no quality presets: its only
    /// other source is the hardware row of VideoHardware.dbc (0x0076b3f0
    /// copies its +0x2c into the detected defaults, written out by
    /// 0x0078e1a0), so no fixed preset here turns it on.
    bool  specular;
    /// The rest of the client's own quality levels (FrameXML
    /// GraphicsQualityLevels.lua, the Effects panel's rows), in this
    /// client's units: the doodad distance (EnvironmentDetail 0.5-1.5, as a
    /// percentage), the clutter radius in yards (ClutterRadius), the particle
    /// density (0.1-1.0, as a percentage) and the weather level
    /// (WeatherIntensity 0-3). Low left all four where they were, so the
    /// doodads it is meant to cut kept their full distance.
    int   environmentDetail;     ///< percent
    int   groundClutterDistance; ///< yards
    int   particleDensity;       ///< percent
    int   weatherDetail;         ///< 0..3
    /// Character textures kept in block formats (the Texture compression
    /// setting). On for Low, which is for the cards with the least memory;
    /// off elsewhere, the composited skins being a little lossy.
    bool  textureCompression;
    /// Interactive water (WoWee only, user decision 2026-11-11): the traced
    /// water's bow waves and wakes behind what moves through it. Off on Low
    /// and Medium, on from High up.
    bool  interactiveWater;
};

// Note the shadows column: Low turns them off, as the client's own Low does
// (its ShadowQuality 0: baked terrain shadows only), and the rest leave them
// on.
//
// Every row once left them on, because shadows off still drew the whole
// shadow pass and only stopped the shaders reading it - a preset asking for
// off saved nothing. Off now skips the pass (Renderer::planShadowPass), which
// on a phone is several milliseconds a frame.
// The last four columns are the client's levels 1, 3, 4 and 5 (Low, Good,
// High, Ultra), the same rows the view distances below come from.
// And the FXAA column: off on every row. Ultra used to turn it on over 8x
// MSAA, where there is nothing left for it to find and its sub-pixel filter
// only softened the resolve. It stays in the panel for anyone who wants it,
// which mostly means anyone running without MSAA.
// Super-Ultra (user decision 2026-11-13) comes after the client's own Ultra
// (FrameXML GlobalStrings VIDEO_QUALITY_LABEL5 "Ultra", GraphicsQualityLevels
// [5] "ULTRA mode") and is the user's own picture, from their settings file
// of that day: ray traced lighting with shadows, occlusion and bounce at full
// resolution, the extras (lens flare, sharp stars, sun shafts, light shafts
// on High) on, normal maps at 0.4, and the shadow map off - the traced sun
// and moon shadows stand in for it. The rows it sets beyond these columns are
// kSuperUltraExtras below. Grass is off on every row (user decision, the same
// day): it costs main-thread time and has known faults.
// The view distances are the client's own quality levels' farclip
// (FrameXML GraphicsQualityLevels.lua): Low 177, Good 727, High 1057 and Ultra
// 1277 - its Fair, 507, has no row here.
constexpr GraphicsPresetValues kGraphicsPresets[] = {
    /* Low    */ { .viewDistance = 177.0f, .shadows = false, .shadowDistance = 100.0f, .antiAliasing = 0, .fxaa = false, .normalMapping = false, .normalMapStrength = 0.3f, .groundClutter = 25,  .grass = false, .grassDensity = 70, .grassHeight = 50, .grassDistance = 215, .lensFlare = 0.0f, .sharpStars = false, .sunShafts = false, .lightShafts = 0, .fogSkyBlend = 0.0f, .specular = false, .environmentDetail = 50,  .groundClutterDistance = 70,  .particleDensity = 10,  .weatherDetail = 0, .textureCompression = true, .interactiveWater = false},
    /* Medium */ { .viewDistance = 727.0f, .shadows = true,  .shadowDistance = 200.0f, .antiAliasing = 1, .fxaa = false, .normalMapping = true,  .normalMapStrength = 0.3f, .groundClutter = 60,  .grass = false, .grassDensity = 70, .grassHeight = 50, .grassDistance = 215, .lensFlare = 0.0f, .sharpStars = false, .sunShafts = false, .lightShafts = 0, .fogSkyBlend = 0.0f, .specular = false, .environmentDetail = 100, .groundClutterDistance = 120, .particleDensity = 60,  .weatherDetail = 2, .textureCompression = false, .interactiveWater = false},
    /* High   */ {.viewDistance = 1057.0f, .shadows = true,  .shadowDistance = 350.0f, .antiAliasing = 2, .fxaa = false, .normalMapping = true,  .normalMapStrength = 0.3f, .groundClutter = 100, .grass = false, .grassDensity = 70, .grassHeight = 50, .grassDistance = 215, .lensFlare = 0.0f, .sharpStars = false, .sunShafts = false, .lightShafts = 0, .fogSkyBlend = 0.0f, .specular = false, .environmentDetail = 125, .groundClutterDistance = 130, .particleDensity = 80,  .weatherDetail = 3, .textureCompression = false, .interactiveWater = true},
    /* Ultra  */ {.viewDistance = 1277.0f, .shadows = true,  .shadowDistance = 500.0f, .antiAliasing = 3, .fxaa = false, .normalMapping = true,  .normalMapStrength = 0.3f, .groundClutter = 150, .grass = false, .grassDensity = 70, .grassHeight = 50, .grassDistance = 215, .lensFlare = 0.0f, .sharpStars = false, .sunShafts = false, .lightShafts = 0, .fogSkyBlend = 0.0f, .specular = false, .environmentDetail = 150, .groundClutterDistance = 140, .particleDensity = 100, .weatherDetail = 3, .textureCompression = false, .interactiveWater = true},
    /* Super-Ultra */ {.viewDistance = 1277.0f, .shadows = false, .shadowDistance = 500.0f, .antiAliasing = 3, .fxaa = false, .normalMapping = true,  .normalMapStrength = 0.4f, .groundClutter = 150, .grass = false, .grassDensity = 70, .grassHeight = 50, .grassDistance = 215, .lensFlare = 1.0f, .sharpStars = true,  .sunShafts = true,  .lightShafts = 3, .fogSkyBlend = 0.0f, .specular = false, .environmentDetail = 150, .groundClutterDistance = 140, .particleDensity = 100, .weatherDetail = 3, .textureCompression = false, .interactiveWater = true},
};

/// The settings Super-Ultra sets beyond the columns above: the rest of the
/// graphics pages (Graphics, Shadows, Detail, Water, Ray Tracing with its eye
/// adaptation, Upscaling), as the user's settings have them. Low to Ultra
/// put these back to their defaults (presetRowValues). Where the
/// user's file had no row the value is the schema's default - what their
/// client ran with. (Frame generation, off in their file, is a row only
/// in builds with FSR 3 frame generation, so it is not one of these.)
struct GraphicsPresetExtra {
    const char* key;   ///< the schema key
    float value;
};
inline constexpr GraphicsPresetExtra kSuperUltraExtras[] = {
    {"fogstrength", 1.0f},       {"mistdensity", 1.0f},
    {"shadowresolution", 1.0f},  {"shadowcascades", 2.0f},
    {"texturefiltering", 4.0f},
    {"rtwater", 1.0f},           {"rtswimwaves", 1.0f},
    {"rtwaveamp", 100.0f},       {"rtwavefreq", 100.0f},
    {"raytracedlighting", 3.0f}, {"rtfullres", 1.0f},
    {"rtroomsun", 1.0f},         {"rtsoftlamps", 0.0f},
    {"rtrealisticfalloff", 0.0f}, {"rtminbrightness", 0.0f},
    {"rteyebrighten", 30.0f},    {"rteyeseconds", 2.5f},
    {"rteyesensitivity", 0.0f},
    {"rtsunrays", 1.0f},         {"rtaorays", 1.0f},
    {"rtpointrays", 1.0f},       {"rtbouncerays", 1.0f},
    {"upscaling", 0.0f},         {"fsrquality", 3.0f},
    {"fsrsharpness", 1.6f},
};

/// The settings the columns above set, by schema key, in the order a preset
/// sets them.
inline constexpr const char* kGraphicsPresetKeys[] = {
    "viewdistance", "shadows", "shadowdistance", "antialiasing", "fxaa",
    "normalmapping", "normalmapstrength",
    "groundclutter",
    "grassenabled", "grassdensity", "grassheight", "grassdistance",
    "lensflare", "sharpstars", "sunshafts", "lightshafts", "fogskyblend",
    "specular",
    "environmentdetail", "groundclutterdistance", "particledensity", "weatherdetail",
    "texturecompression", "interactivewater",
};

/// The pages a preset is the whole of (user decision 2026-11-13): every row
/// on them that a preset has no column for is put back to its default -
/// or, for Super-Ultra, to its kSuperUltraExtras value where it has one - so
/// that Ultra after Super-Ultra is Ultra, not Ultra with ray tracing left on.
/// Not Display (window, resolution, frame cap, brightness: the screen's, not
/// the picture's quality) and not the Debug pages.
inline constexpr const char* kGraphicsPresetPages[] = {
    "Graphics", "Shadows", "Detail", "Grass", "Water", "Ray Tracing", "Upscaling",
};

/// The number of presets, not counting Custom - which is not a set of values
/// but the name for "these are whatever you made them".
inline constexpr int kGraphicsPresetCount =
    static_cast<int>(sizeof(kGraphicsPresets) / sizeof(kGraphicsPresets[0]));

}  // namespace ui
}  // namespace wowee
