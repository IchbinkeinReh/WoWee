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

/// What each quality preset means, in the order Low, Medium, High, Ultra.
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
    /// by the sky, and the enhanced water. Off on every row, so picking any
    /// preset gives the client's own picture; each stays in the panel to be
    /// turned on by hand.
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
// The view distances are the client's own quality levels' farclip
// (FrameXML GraphicsQualityLevels.lua): Low 177, Good 727, High 1057 and Ultra
// 1277 - its Fair, 507, has no row here.
constexpr GraphicsPresetValues kGraphicsPresets[] = {
    /* Low    */ { .viewDistance = 177.0f, .shadows = false, .shadowDistance = 100.0f, .antiAliasing = 0, .fxaa = false, .normalMapping = false, .normalMapStrength = 0.3f, .groundClutter = 25,  .grass = false, .grassDensity = 70, .grassHeight = 50, .grassDistance = 215, .lensFlare = 0.0f, .sharpStars = false, .sunShafts = false, .lightShafts = 0, .fogSkyBlend = 0.0f, .specular = false, .environmentDetail = 50,  .groundClutterDistance = 70,  .particleDensity = 10,  .weatherDetail = 0},
    /* Medium */ { .viewDistance = 727.0f, .shadows = true,  .shadowDistance = 200.0f, .antiAliasing = 1, .fxaa = false, .normalMapping = true,  .normalMapStrength = 0.3f, .groundClutter = 60,  .grass = false, .grassDensity = 70, .grassHeight = 50, .grassDistance = 215, .lensFlare = 0.0f, .sharpStars = false, .sunShafts = false, .lightShafts = 0, .fogSkyBlend = 0.0f, .specular = false, .environmentDetail = 100, .groundClutterDistance = 120, .particleDensity = 60,  .weatherDetail = 2},
    /* High   */ {.viewDistance = 1057.0f, .shadows = true,  .shadowDistance = 350.0f, .antiAliasing = 2, .fxaa = false, .normalMapping = true,  .normalMapStrength = 0.3f, .groundClutter = 100, .grass = false, .grassDensity = 70, .grassHeight = 50, .grassDistance = 215, .lensFlare = 0.0f, .sharpStars = false, .sunShafts = false, .lightShafts = 0, .fogSkyBlend = 0.0f, .specular = false, .environmentDetail = 125, .groundClutterDistance = 130, .particleDensity = 80,  .weatherDetail = 3},
    /* Ultra  */ {.viewDistance = 1277.0f, .shadows = true,  .shadowDistance = 500.0f, .antiAliasing = 3, .fxaa = false, .normalMapping = true,  .normalMapStrength = 0.3f, .groundClutter = 150, .grass = true,  .grassDensity = 70, .grassHeight = 50, .grassDistance = 215, .lensFlare = 0.0f, .sharpStars = false, .sunShafts = false, .lightShafts = 0, .fogSkyBlend = 0.0f, .specular = false, .environmentDetail = 150, .groundClutterDistance = 140, .particleDensity = 100, .weatherDetail = 3},
};

/// The number of presets, not counting Custom - which is not a set of values
/// but the name for "these are whatever you made them".
inline constexpr int kGraphicsPresetCount =
    static_cast<int>(sizeof(kGraphicsPresets) / sizeof(kGraphicsPresets[0]));

}  // namespace ui
}  // namespace wowee
