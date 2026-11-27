#pragma once

/// The arithmetic of the volumetric light (VolumetricLight), apart from Vulkan
/// so a test can state it: what each quality spends, how a lamp's light fades
/// through the air round it, where a view ray crosses a lamp's reach, how
/// thick the air is, and how the sun's and the moon's shares hand over at the
/// horizon. volumetric_light.comp.glsl holds the same formulas; each says
/// which of these it mirrors.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::rendering::volumetric_light {

/// The setting's choices, in the order the settings file stores them.
enum class Quality : int { Off = 0, Low = 1, High = 2 };

inline Quality qualityFromIndex(int index) {
    return index <= 0 ? Quality::Off : (index == 1 ? Quality::Low : Quality::High);
}

/// What one frame may spend, fixed per quality, so the cost has a ceiling
/// whatever the scene holds: the march runs at a fraction of the screen's
/// size, the sun's (or moon's) light in a fixed number of steps along each
/// ray, every lamp the ray passes through in a fixed number more, and at most
/// so many lamps. Each lit step of the sky body's light asks a second point
/// beside it (shaftRadius), so its steps cost up to two lookups each; the
/// steps move from frame to frame and the history (kHistoryWeight) gathers
/// about five frames of them (VL4: 16/8 steps where VL had 24/12).
struct Budget {
    unsigned divisor = 0;     ///< the march's image is the screen's size over this
    unsigned sunSteps = 0;    ///< samples of the sky body's light along a ray
    unsigned lightSteps = 0;  ///< samples inside each lamp's reach a ray crosses
    unsigned maxLights = 0;   ///< lamps per frame, the most important first
};

inline Budget budgetFor(Quality q) {
    switch (q) {
        case Quality::Low: return {.divisor = 4, .sunSteps = 8, .lightSteps = 4, .maxLights = 16};
        case Quality::High: return {.divisor = 2, .sunSteps = 16, .lightSteps = 8, .maxLights = 32};
        case Quality::Off: break;
    }
    return {};
}

/// How much light the air scatters toward the eye per yard, for the sky
/// body and for the lamps, before the air's thickness (airFactor). Clear air
/// over a hundred yards scatters next to nothing: what the eye sees of it is
/// a faint glow toward the sun and the shafts where something shades part
/// of it - not a veil over the scene, which the zone's own fog already is
/// (T434: 0.001 a yard with an even floor under the lobe was a milky veil
/// by day and grey fog by night). Low scatters less than High, not only
/// coarser: the two must look apart (T434: Low was High at a lower size).
///
/// The sky body's scatter counts only the lit air beside shade (shaftRadius):
/// open air adds nothing whatever the figure, so it can be what makes a
/// shaft seen. U23: VL2's 2e-4 under its "half the ray in shade" weight and
/// the hidden disc's lobe of 0.4 came to about a hundredth of the sun's
/// light toward a sun behind leaves - invisible even at 400 %.
struct Scatter {
    float body = 0.0f;
    float lamp = 0.0f;
};

inline constexpr float kBodyScatter = 1.5e-3f;  // per yard, High, lit air beside shade only (VL4)
inline constexpr float kLampScatter = 0.006f;   // per yard, High
inline constexpr float kLowShare = 0.6f;        // Low's share of High's

inline Scatter scatterFor(Quality q) {
    switch (q) {
        case Quality::Low: return {.body = kBodyScatter * kLowShare, .lamp = kLampScatter * kLowShare};
        case Quality::High: return {.body = kBodyScatter, .lamp = kLampScatter};
        case Quality::Off: break;
    }
    return {};
}

/// A term's scatter under its Debug Glow slider (dbgvlsun, dbgvlmoon,
/// dbgvllights): `share` is the slider's percent over 100. 1 leaves it
/// exactly as it is; below 0 is taken as 0.
inline float tunedScatter(float scatter, float share) { return scatter * (share > 0.0f ? share : 0.0f); }

/// The moon's light in the air beside the sun's for the same colour. Her
/// light colour (the night's directional light) is already a fraction of the
/// sun's; VL2's half on top of that left nothing (U23: no moon shafts at
/// 400 %). Her cone over the open night sky (T434) is gone with the rule that
/// only lit air beside shade counts, not with a smaller share.
inline constexpr float kMoonScatterShare = 1.0f;

/// A lamp's glow by day: the eye, held to the daylight, sees the air round a
/// lamp little lit beside the sunlit air about it. `sunShare` is
/// bodyShares' sun (1 by full day). T436: a street lamp's glow was a wall of
/// light at one in the afternoon.
inline constexpr float kLampDaylightShare = 0.35f;

inline float lampDaylight(float sunShare) {
    return 1.0f - (1.0f - kLampDaylightShare) * std::clamp(sunShare, 0.0f, 1.0f);
}

/// Whether a light of the trace's list glows in the air. Not a building's own
/// light (MOLT): it stands for the light a room is painted with, not for a
/// flame one can see - its glow was the same beige over the whole of a room
/// (T434, the Crossroads inn without the ray traced lighting). Its lamps,
/// fires and glowing surfaces give their own lights and glow.
template <class SourceT>
inline bool glowsInAir(SourceT source, SourceT building) {
    return source != building;
}

/// How much of the sky body's light a point of air takes from the shadow
/// map toward the edge of its outer cascade, by max(|x|, |y|) across that
/// tile: all of it inside nine tenths, none past its edge, smooth between.
/// Past the map nothing says the point is lit, so it is not (the user's
/// rule); the cut was a hard-edged cone round the moon (T434).
inline float coverageFade(float edge) {
    // Exactly nothing at and past the edge: (1 - 0.8) / 0.2 rounds a hair
    // under 1 in float, and the smoothstep left a tiny light there
    // (macOS CI, clang/libc++).
    if (!(edge < 1.0f)) return 0.0f;
    const float t = std::clamp((edge - 0.8f) / 0.2f, 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

/// The most lamps any quality takes: the size of the shader's array.
inline constexpr unsigned kMaxLights = 32;

/// How far along the view the sun's and the moon's light is gathered, yards.
/// The near air, where a shaft is a real share of what the eye looks
/// through; the shadow map's own reach ends the rest.
inline constexpr float kSunReach = 120.0f;

/// The radius of a lamp's glow core, yards: an eighth of its reach, half a
/// yard to a yard and a half - about the flame and its glass with the air
/// right round them, not a ball of light the size of the room (V572/T436:
/// a fifth of the reach, up to three yards, glowed as a wall of light).
inline float glowCore(float end) {
    return std::clamp(0.125f * end, 0.5f, 1.5f);
}

/// How much of a lamp's light the air `distance` yards from it holds, for a
/// lamp that reaches `end` yards: bright at its core, a soft tail, nothing
/// at its reach. The inverse square from the core (glowCore), times 2 so it
/// crosses 1 at the core, windowed to nothing at the end -
/// rt_lights::realisticAttenuation's shape with a core the size of the glow
/// rather than of the flame. Smooth everywhere, its slope nothing at the end:
/// no ring where the reach stops.
/// volumetric_light.comp.glsl's vlGlowFalloff must agree.
inline float glowFalloff(float distance, float end) {
    if (!(end > 0.0f) || !(distance < end)) return 0.0f;
    const float core = glowCore(end);
    const float q = distance / core;
    const float x = distance / end;
    const float w = 1.0f - x * x * x * x;
    return std::min(2.0f * w * w / (1.0f + q * q), 2.0f);
}

/// Where along a ray the march puts a lamp's samples: as densely as the glow
/// is bright (equi-angular sampling, Kulla and Fajardo 2012, about the core
/// rather than the lamp's point). `along` is where the ray comes nearest the
/// lamp, `across` how near, `t0`..`t1` the stretch inside its reach and `u`
/// 0..1 the sample's place in it. Returns where the sample lies and what it
/// stands for (one over the density): glowFalloff times that, summed over
/// the samples and divided by their number, is the glow along the stretch -
/// without the window, exactly, whatever the number of samples, so the glow
/// has no grain or bands for the blur to hide, and its edge is the lamp's
/// own soft one.
struct GlowSample {
    float t = 0.0f;
    float weight = 0.0f;
};

inline GlowSample glowSample(float along, float across, float t0, float t1, float core, float u) {
    const float h = std::sqrt(across * across + core * core);
    const float a0 = std::atan((t0 - along) / h);
    const float a1 = std::atan((t1 - along) / h);
    const float a = a0 + (a1 - a0) * std::clamp(u, 0.0f, 1.0f);
    const float x = h * std::tan(a);
    GlowSample s;
    s.t = along + x;
    s.weight = (a1 - a0) * (h * h + x * x) / h;
    return s;
}

/// Where the ray origin + t * dir (dir of unit length) runs inside the sphere
/// round `centre` of `radius`, clipped to 0..maxT: the stretch of air a lamp
/// lights on the way to what the pixel shows. None when it misses, or when
/// that stretch lies behind the eye or past the surface.
inline std::optional<std::pair<float, float>> segmentThroughSphere(const glm::vec3& origin, const glm::vec3& dir,
                                                                   float maxT, const glm::vec3& centre,
                                                                   float radius) {
    const glm::vec3 oc = origin - centre;
    const float b = glm::dot(oc, dir);
    const float c = glm::dot(oc, oc) - radius * radius;
    const float disc = b * b - c;
    if (!(disc > 0.0f)) return std::nullopt;
    const float s = std::sqrt(disc);
    const float t0 = std::max(-b - s, 0.0f);
    const float t1 = std::min(-b + s, maxT);
    if (!(t1 > t0)) return std::nullopt;
    return std::make_pair(t0, t1);
}

/// Henyey-Greenstein, scaled so scattering evenly in every direction is 1 -
/// the mist's phaseHG (volumetric_fog_inject.comp.glsl), the same function.
inline float phaseHG(float cosTheta, float g) {
    const float g2 = g * g;
    return (1.0f - g2) / std::pow(std::max(1.0f + g2 - 2.0f * g * cosTheta, 1e-4f), 1.5f);
}

/// Where the sky body's light in the air is seen: a lit point of air counts
/// only while shade lies beside it - a point this far from it, across the
/// light, is in shade (the march asks one such point a lit step, in a
/// direction that turns from step to step and frame to frame). Open
/// daylight, the open sky and the air round an unhidden sun or moon have no
/// shade beside them and add nothing (T434, the tester's aim: off and on
/// alike in the open); a gap in a crown, a window, a doorway, a ridge in
/// front of a low sun have the shade beside the lit air, which is what the
/// eye sees as a shaft. U23 replaced VL2's per-ray rule (all of a ray's
/// light only while half of it lay in shade), which threw away the lit air
/// of nearly every ray toward a sun behind leaves.
///
/// The span grows with the distance, so a far shaft is not a hairline: a
/// tenth of the way along the ray, two to eight yards.
inline constexpr float kShaftRadiusShare = 0.1f;
inline constexpr float kShaftRadiusMin = 2.0f;
inline constexpr float kShaftRadiusMax = 8.0f;

inline float shaftRadius(float t) {
    return std::clamp(kShaftRadiusShare * t, kShaftRadiusMin, kShaftRadiusMax);
}

/// Whether the point beside a lit one can say anything: it must be air the
/// eye can see, in front of what the screen shows there (`pointDepth` and
/// `sceneDepth` along the view, yards; `sky` where the screen shows sky).
/// A point inside the ground, a wall or a crown is in their shade without
/// standing for any shade in the air - near the ground at a low sun every
/// lit point would find one below it and the whole ground would wear a veil.
/// Off the screen or behind the eye nothing is known: it does not count.
inline constexpr float kInAirMargin = 0.1f;

inline bool besideInAir(bool onScreen, bool sky, float pointDepth, float sceneDepth) {
    if (!onScreen || !(pointDepth > 0.0f)) return false;
    if (sky) return true;
    return pointDepth < sceneDepth - kInAirMargin;
}

/// One lit step's share of the sky body's light: lit (0..1, the shadow map
/// fades at its edge) times how shaded the point beside it is - only when
/// that point is air in sight (besideInAir). A shaded step adds nothing.
inline float shaftStep(float lit, bool besideValid, float besideShade) {
    if (!(lit > 0.0f) || !besideValid) return 0.0f;
    return lit * std::clamp(besideShade, 0.0f, 1.0f);
}

/// The forward lobe toward the sun (moon), smoothly held under `cap` times
/// even: cap * p / (cap + p). The hard min() it replaces left a flat top
/// with a crease round it - straight at the moon a bright ring (T434).
/// Forward-peaked, as off dust and haze: toward the sun about twelve times
/// even, sideways a sixth of even, with the back to it a sixteenth (VL4:
/// g 0.8, cap 16, where VL2 had 0.7 and 8 - the shafts stand out toward the
/// sun and hardly anywhere else).
inline constexpr float kBodyLobeG = 0.8f;
inline constexpr float kBodyLobeCap = 16.0f;

inline float bodyPhase(float cosTheta) {
    const float p = phaseHG(cosTheta, kBodyLobeG);
    return kBodyLobeCap * p / (kBodyLobeCap + p);
}

/// The thickest the air gets: fog, dawn and rain together once multiplied to
/// eight times clear air, a milky wall.
inline constexpr float kMaxAirFactor = 2.5f;

/// How much thicker than clear air the air is: the zone's own fog (read off
/// how close its authored fog ends: Duskwood's 525 yards gives more, an open
/// plain's far end less - the mist's factor, Renderer::volumetricFogExtinction),
/// the morning mist round half past six, and rain or snow. 1 is a clear day
/// on a plain whose fog ends at 900 yards.
inline float airFactor(float authoredFogEnd, float hours, float rain, float snow) {
    float f = 1.0f;
    if (authoredFogEnd > 1.0f) f *= std::clamp(900.0f / authoredFogEnd, 0.6f, 2.0f);
    const float fromDawn = std::abs(hours - 6.5f);
    const float t = std::clamp(fromDawn / 2.5f, 0.0f, 1.0f);
    const float dawn = 1.0f - t * t * (3.0f - 2.0f * t);
    f *= 1.0f + 0.8f * dawn;
    f *= 1.0f + 1.2f * std::clamp(rain, 0.0f, 1.0f);
    f *= 1.0f + 0.8f * std::clamp(snow, 0.0f, 1.0f);
    return std::min(f, kMaxAirFactor);
}

/// The sun's and the moon's shares of the sky's light in the air, from the
/// sines of their drawn elevations: the sun's while it is up, fading out over
/// its last few degrees; the moon's only once the sun has gone and while she
/// is up herself. Neither below the horizon - no light comes from a sky body
/// that is not there to be seen.
struct BodyShares {
    float sun = 0.0f;
    float moon = 0.0f;
};

inline BodyShares bodyShares(float sunHeight, float moonHeight) {
    const auto up = [](float h) {
        const float t = std::clamp(h / 0.08f, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    BodyShares s;
    s.sun = up(sunHeight);
    s.moon = (1.0f - s.sun) * up(moonHeight);
    return s;
}

/// The history (U23, user 2026-11-27: the light may be smoothed over the
/// last ~5 frames): each frame's light is blended into what the frames
/// before left at this weight - 0.2, so the last five frames carry two
/// thirds of it - after the old light is found again where the same air was
/// last frame (reprojected through the scene's depth) and held to the range
/// of this frame's light round it, so a moving lamp or a turned head leaves
/// no trail.
inline constexpr float kHistoryWeight = 0.2f;

/// The camera moved this far in one frame: a teleport, a cut, a load - the
/// old light is of another place and is dropped. Flying at full speed at
/// ten frames a second is a few yards.
inline constexpr float kHistoryCutDistance = 15.0f;

/// Whether this frame may build on the last one's light: it drew the light
/// too, into targets of the same size and quality, and the camera did not
/// jump. Anything else starts afresh from this frame's light alone.
inline bool historyUsable(bool lastFrameDrew, bool sameTargets, float cameraMoved) {
    return lastFrameDrew && sameTargets && cameraMoved <= kHistoryCutDistance;
}

/// This frame's light blended into the history: the new light alone when
/// there is none to build on.
inline glm::vec3 historyBlend(const glm::vec3& history, const glm::vec3& current, bool usable) {
    if (!usable) return current;
    return history + (current - history) * kHistoryWeight;
}

/// The distance the history keeps beside its light for the sky, which has no
/// surface: any two such agree, and none agrees with a surface (a half float
/// holds it).
inline constexpr float kHistorySkyDistance = 60000.0f;

/// Whether the light kept for a texel was the same air's: the distance from
/// last frame's eye to what its texel showed (`kept`), against the distance
/// from last frame's eye to what this texel shows now (`expected`). Five
/// percent and a tenth of a yard either way, or something moved across it -
/// a unit walked by, a door swung - and the old light is not used there.
inline bool historyDepthAgrees(float expected, float kept) {
    if (!(expected > 0.0f) || !(kept > 0.0f)) return false;
    return std::abs(expected - kept) <= 0.05f * std::max(expected, kept) + 0.1f;
}

/// The debug view "VL only" (dbgvlonly): the volumetric light alone over
/// black, ten times as bright so its faint light shows, brought under white
/// by the composite's own shoulder (compositeShoulder).
inline constexpr float kDebugViewGain = 10.0f;

/// volumetric_light_composite.frag.glsl's shoulder: under the knee as it
/// is, above it brought smoothly under white without changing the hue.
inline glm::vec3 compositeShoulder(const glm::vec3& light) {
    constexpr float knee = 0.4f;
    const glm::vec3 l = glm::max(light, glm::vec3(0.0f));
    const float peak = std::max(std::max(l.r, l.g), l.b);
    if (peak <= knee) return l;
    const float over = (peak - knee) / (1.0f - knee);
    const float shaped = knee + (1.0f - knee) * over / (1.0f + over);
    return l * (shaped / peak);
}

inline glm::vec3 debugViewColor(const glm::vec3& light) {
    return compositeShoulder(light * kDebugViewGain);
}

/// The lamps the march takes, out of a list most important first (as
/// rt_lights::selectLights leaves it): the first `max` whose reach comes
/// within `range` yards of the eye. Returns their indices, in that order.
template <class Light>
std::vector<size_t> pickLights(const std::vector<Light>& lights, const glm::vec3& eye, float range, size_t max) {
    std::vector<size_t> out;
    for (size_t i = 0; i < lights.size() && out.size() < max; ++i) {
        const Light& l = lights[i];
        if (!(l.attenuationEnd > 0.0f)) continue;
        if (glm::length(l.position - eye) - l.attenuationEnd > range) continue;
        out.push_back(i);
    }
    return out;
}

}  // namespace wowee::rendering::volumetric_light
