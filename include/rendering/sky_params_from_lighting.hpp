#pragma once

/// Filling the sky's parameters from what the lighting manager resolved.
///
/// The renderer records a frame two ways - secondary command buffers on worker
/// threads, or inline on one - and each path built this by hand. Fifteen
/// assignments, written twice, forty lines apart.
///
/// Nothing about a field missing from one copy fails. The sky simply looks
/// different depending on whether parallel recording is on, which is a setting
/// most people never touch and nobody would think to bisect against. The last
/// three fields were added to both copies by hand, and the fourth would have
/// been too.

#include "rendering/day_night.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/sky_system.hpp"

namespace wowee::rendering {

/// The sky parameters for this frame.
///
/// `lighting` is null when there is no lighting manager, in which case the
/// colours keep the defaults SkyParams declares rather than being zeroed.
/// `gameTime` is the server's hour, or -1 when it has not sent one.
inline SkyParams skyParamsFromLighting(float timeOfDay, float gameTime,
                                       float weatherIntensity,
                                       const LightingParams* lighting,
                                       bool useOriginalSkybox) {
    SkyParams params;
    params.timeOfDay = timeOfDay;
    params.gameTime = gameTime;
    if (lighting) {
        params.directionalDir = lighting->directionalDir;
        params.sunDir = lighting->sunDir;
        params.moonDir = lighting->moonDir;
        params.sunColor = lighting->sunColor;
        params.skyTopColor = lighting->skyTopColor;
        params.skyMiddleColor = lighting->skyMiddleColor;
        params.skyBand1Color = lighting->skyBand1Color;
        params.skyBand2Color = lighting->skyBand2Color;
        params.skySmogColor = lighting->skySmogColor;
        params.skyFogColor = lighting->fogColor;
        params.cloudSunColor = lighting->cloudSunColor;
        params.cloudShadeColor = lighting->cloudShadeColor;
        params.cloudBaseColor = lighting->cloudBaseColor;
        params.cloudDensity = lighting->cloudDensity;
        params.skyHighlight = daynight::skyHighlightStrength(timeOfDay / 24.0f, lighting->highlightSky);
    }
    params.weatherIntensity = weatherIntensity;
    params.skyboxModelId = 0;
    params.useOriginalSkybox = useOriginalSkybox;
    return params;
}

}  // namespace wowee::rendering
