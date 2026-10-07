#pragma once

/// The pieces of the 3.3.5a client's DayNight code (Wow.exe build 12340) that
/// do not need a DBC: the curves that place the light, the sun and the moon,
/// the weight of one Light.dbc volume, the order the volumes are applied in,
/// and the storm blend. Each function names the client routine it follows.
///
/// Directions come back in render space (core::coords::canonicalToRender), as
/// unit vectors. The client's own vectors are in world space.

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "core/coordinates.hpp"

namespace wowee::rendering::daynight {

/// One key of a client time curve: a day fraction and the value there.
struct CurveKey {
    float t;
    float v;
};

/// A client time curve at day fraction `t` (0x007ed3b0).
///
/// `t` is clamped to [0,1]. The key after `t` is the first whose time is not
/// below it; the curve wraps from the last key to the first across midnight,
/// and two keys closer than 0.001 apart hold the earlier key's value.
template <std::size_t N>
inline float sampleCurve(const CurveKey (&keys)[N], float t) {
    static_assert(N > 0);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    std::size_t next = 0;
    while (next < N && t > keys[next].t) ++next;
    std::size_t prev;
    if (next == N) {
        next = 0;
        prev = N - 1;
    } else {
        prev = next == 0 ? N - 1 : next - 1;
    }
    float span = keys[next].t - keys[prev].t;
    if (std::abs(span) < 0.001f) return keys[prev].v;
    if (span < 0.0f) span += 1.0f;
    float into = t - keys[prev].t;
    if (into < 0.0f) into += 1.0f;
    return keys[prev].v + (keys[next].v - keys[prev].v) * (into / span);
}

/// cos(pi * u) as the client computes it: a floor split (0x005fe800) and the
/// cubic 1 - 6f^2 + 4f^3 on the fraction, negated on odd periods. Off from the
/// true cosine by up to 0.02, which is what the client draws with.
inline float clientCosPi(float u) {
    const float whole = std::floor(u);
    const float f = u - whole;
    const float c = 1.0f - (6.0f - f * 4.0f) * f * f;
    const long long period = static_cast<long long>(whole);
    return (period & 1) ? -c : c;
}

/// The unit vector at polar angle `phi` from straight up and azimuth `theta`,
/// (cos theta sin phi, sin theta sin phi, cos phi) in world space, through
/// clientCosPi as 0x007eea90 and 0x007eecc0 compute it. Render space out.
inline glm::vec3 polarDirection(float phi, float theta) {
    constexpr float kInvPi = 0.31830987f;
    const float sinPhi = clientCosPi(phi * kInvPi - 0.5f);
    const float cosPhi = clientCosPi(phi * kInvPi);
    const float sinTheta = clientCosPi(theta * kInvPi - 0.5f);
    const float cosTheta = clientCosPi(theta * kInvPi);
    const glm::vec3 world(cosTheta * sinPhi, sinTheta * sinPhi, cosPhi);
    return glm::normalize(core::coords::canonicalToRender(world));
}

// ---------------------------------------------------------------------------
// The curves. Built in place by the functions that use them, read here off
// Wow.exe's initialisers. Times are day fractions, angles radians.

/// 0x007eea90's polar angle of the directional light (0xd390e4): 127 degrees
/// from straight up at midnight and noon, 110 at 06:00 and 18:00.
inline constexpr CurveKey kLightPhi[] = {
    {0.00f, 2.2165682f}, {0.25f, 1.9198622f}, {0.50f, 2.2165682f}, {0.75f, 1.9198622f}};
/// 0x007eea90's azimuth (0xd390c4): 5pi/4 at every key.
inline constexpr float kLightTheta = 3.9269907f;

/// 0x007eecc0's sun polar angle (0xd391e0): 100 degrees (ten below the
/// horizon) at 05:30 and 21:30, five from straight up 11:55 to 12:05.
inline constexpr CurveKey kSunPhi[] = {
    {0.22916667f, 1.7453293f}, {0.49652779f, 0.08726646f}, {0.5f, 0.08726646f},
    {0.50347221f, 0.08726646f}, {0.89583331f, 1.7453293f}};
/// 0x007eecc0's sun azimuth (0xd391c8): pi/4 at every key.
inline constexpr float kSunTheta = 0.78539819f;

/// 0x007eecc0's moon polar angle (0xd391a0): 35 degrees from straight up
/// 23:55 to 00:05, 100 (below the horizon) at 04:00 and 22:00.
inline constexpr CurveKey kMoonPhi[] = {
    {0.0f, 0.61086524f}, {0.0034722222f, 0.61086524f}, {0.16666667f, 1.7453293f},
    {0.91666669f, 1.7453293f}, {0.99652779f, 0.61086524f}};
/// 0x007eecc0's moon azimuth (0xd39188): pi/4 at every key.
inline constexpr float kMoonTheta = 0.78539819f;

/// 0x007eecc0's Blue Child azimuth (0xd39148): 135 degrees, 150 at 04:00,
/// 165 at 22:00. Its polar angle is the White Lady's curve (0xd39160 holds
/// the same keys as 0xd391a0).
inline constexpr CurveKey kBlueChildTheta[] = {
    {0.0f, 2.3561945f}, {0.16666667f, 2.6179938f}, {0.91666669f, 2.8797932f}};
/// The Blue Child runs on a clock 1.7 times slower than the day
/// (0xd38e84), counted from a day number the client leaves at 0 (0xd38b08).
inline constexpr float kBlueChildPeriodDays = 1.7f;

/// The sun sprite's size (0xd39128 x 0xd38e40 = 1): twice as large at 06:00
/// and 21:00, normal from 06:45 to 20:15.
inline constexpr CurveKey kSunSize[] = {
    {0.25f, 2.0f}, {0.28125f, 1.0f}, {0.84375f, 1.0f}, {0.875f, 2.0f}};
/// The moons' size curve (0xd39108): 1 around midnight, 1.5 from 04:00 to
/// 22:00. The White Lady's is scaled by 1.75 (0xd38e60), the Blue Child's by
/// 1 (0xd38e80).
inline constexpr CurveKey kMoonSize[] = {
    {0.041666668f, 1.0f}, {0.16666667f, 1.5f}, {0.91666669f, 1.5f}, {0.99930555f, 1.0f}};
inline constexpr float kWhiteLadyScale = 1.75f;
inline constexpr float kBlueChildScale = 1.0f;
/// How far from the eye the client puts the sun and moons (0x007eecc0), the
/// unit their sprite sizes are in.
inline constexpr float kCelestialDistance = 12.0f;

/// The sun and moon glare (0x007ee150, 0x007ee230, applied by 0x007ef6e0):
/// how fast it rises to and falls from what it should be, per second; its
/// size and alpha, lerped by how squarely the camera faces the body - from
/// a facing of 0.7 (no more than about 45 degrees off) to 1; and the hours
/// it shows.
struct GlareDef {
    float riseRate;
    float fallRate;
    float sizeBase;
    float sizeNear;     ///< at a facing of 0.7 (sun), or the moon's own size
    float sizeFacing;   ///< at a facing of 1
    float alphaNear;
    float alphaFacing;
    CurveKey time[4];
};
inline constexpr float kGlareFacingThreshold = 0.7f;
inline constexpr GlareDef kSunGlare = {
    4.0f, 1.5151515f, 1.0f, 3.0f, 20.0f, 0.5f, 1.0f,
    {{0.27083334f, 0.0f}, {0.3125f, 1.0f}, {0.8125f, 1.0f}, {0.875f, 0.0f}}};
inline constexpr GlareDef kMoonGlare = {
    3.0303030f, 1.5151515f, 2.0f, 1.0f, 1.0f, 0.1f, 1.0f,
    {{0.083333336f, 1.0f}, {0.13541667f, 0.0f}, {0.94791669f, 0.0f}, {0.99930555f, 1.0f}}};

/// How far into its size and alpha ranges a glare is for a camera facing
/// `facing` (the dot of its forward with the way to the body): 0 at 0.7 or
/// less, 1 looking straight at it (0x007ef6e0).
inline float glareFacing(float facing) {
    const float f = facing < kGlareFacingThreshold ? kGlareFacingThreshold : facing;
    return (f - kGlareFacingThreshold) / (1.0f - kGlareFacingThreshold);
}

/// How much the sun's glare takes off the world's light: 0.35 x the facing,
/// held to no less than 0.7, to the tenth power, x how much glare shows
/// (0x007ef6e0 keeps pow(facing, 10) x glare at 0xd38f4c; 0x007816f0 scales
/// the ambient and direct light, 0xd38ca8 and 0xd38cac, by one less 0.35 of
/// it). Looking into a full glare the world is 35% darker.
inline float sunGlareWorldDim(float facing, float glare) {
    const float f = facing < kGlareFacingThreshold ? kGlareFacingThreshold : facing;
    return 0.35f * std::pow(f, 10.0f) * glare;
}

/// A sun or moon quad's alpha at `h` client units above the eye, for a body
/// whose centre is `centre` above it, `size` across and coloured at alpha
/// `a` (0x007edee0): cut at the horizon; the vertices within 0.4 units of it
/// take 2.5 x their height, the rest keep `a`, and a quad that straddles the
/// 0.4 line gets a row of vertices on it at 1. Gouraud shading between them.
/// Negative where the quad is cut.
inline float celestialQuadAlpha(float h, float centre, float size, float a) {
    if (h < 0.0f) return -1.0f;
    const float top = centre + size * 0.5f;
    float bottom = centre - size * 0.5f;
    if (top < 0.0f) return -1.0f;
    if (bottom < 0.0f) bottom = 0.0f;
    const auto low = [](float y) { const float v = y * 2.5f; return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    const bool topLow = top - 0.4f < 0.001f;
    const bool bottomLow = bottom - 0.4f < 0.001f;
    if (!topLow && bottomLow) {
        // Split on the 0.4 line: 2.5 h below it, 1 on it, `a` at the top.
        if (h <= 0.4f) return low(h);
        const float f = (h - 0.4f) / (top - 0.4f);
        return 1.0f + (a - 1.0f) * (f > 1.0f ? 1.0f : f);
    }
    const float span = top - bottom;
    const float f = span > 0.0f ? (h - bottom) / span : 0.0f;
    const float aBottom = bottomLow ? low(bottom) : a;
    const float aTop = topLow ? low(top) : a;
    return aBottom + (aTop - aBottom) * (f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f));
}

/// The way the directional light travels at day fraction `t` (0x007eea90).
/// Always out of the north-west and downward: 37 degrees above the horizon at
/// midnight and noon, 20 at 06:00 and 18:00. The moon's light at night comes
/// down the same path.
inline glm::vec3 directionalLightDir(float t) {
    return polarDirection(sampleCurve(kLightPhi, t), kLightTheta);
}

/// From the eye toward the sun at day fraction `t` (0x007eecc0). Below the
/// horizon between about 20:50 and 06:10.
inline glm::vec3 sunDirection(float t) {
    return polarDirection(sampleCurve(kSunPhi, t), kSunTheta);
}

/// From the eye toward the moon at day fraction `t` (0x007eecc0). Above the
/// horizon between about 22:15 and 03:20.
inline glm::vec3 moonDirection(float t) {
    return polarDirection(sampleCurve(kMoonPhi, t), kMoonTheta);
}

/// The Blue Child's clock at day fraction `t`: t / 1.7, the day number being 0
/// (0x007eecc0).
inline float blueChildTime(float t) {
    const float u = t / kBlueChildPeriodDays;
    return u - std::floor(u);
}

/// From the eye toward the Blue Child at day fraction `t` (0x007eecc0). Up
/// from midnight to about 05:45, low in the north.
inline glm::vec3 blueChildDirection(float t) {
    const float u = blueChildTime(t);
    return polarDirection(sampleCurve(kMoonPhi, u), sampleCurve(kBlueChildTheta, u));
}

// ---------------------------------------------------------------------------
// Light.dbc volumes.

/// How much of one light shows at `dist` yards from it (0x007ee5d0): all of
/// it inside the inner radius, falling linearly to none at the outer.
inline float lightFalloffWeight(float dist, float innerRadius, float outerRadius) {
    if (dist <= innerRadius) return 1.0f;
    const float w = 1.0f - (dist - innerRadius) / (outerRadius - innerRadius);
    return w > 0.0f ? w : 0.0f;  // also a NaN from outer == inner
}

/// The client skips lights whose outer radius is under 3 yards
/// (0x007f1360, with DAT_00af4a68 set).
inline constexpr float kMinLightOuterRadius = 3.0f;

/// Whether light A is applied before light B (0x007ed0a0, the order of the
/// heap 0x007f1360 drains). Each light is lerped over what came before it, so
/// the one applied last shows most: the farther goes first and the nearest
/// last. Two lights within a third of a yard of each other are ordered by
/// inner radius instead, the larger first.
inline bool lightAppliedBefore(float distSqA, float innerA, const glm::vec3& posA,
                               float distSqB, float innerB, const glm::vec3& posB) {
    const glm::vec3 d = posA - posB;
    if (glm::dot(d, d) > (1.0f / 3.0f) * (1.0f / 3.0f)) return distSqA > distSqB;
    return innerA > innerB;
}

/// How far the storm light sets replace the normal ones: four times the
/// weather's intensity, at most all of it, for any kind of weather
/// (0x007f3920 writes it to 0xd38b88 from the weather at 0xd38b4c).
inline float stormBlend(float weatherIntensity) {
    const float s = weatherIntensity * 4.0f;
    return s > 1.0f ? 1.0f : (s > 0.0f ? s : 0.0f);
}

/// The fog distances the client draws with (0x007f16f0 and 0x007ecd80): the
/// end no farther than the far clip and no nearer than 10 yards, the start that
/// fraction of the end, the fraction kept to [-1, 1] (0x007ebff0).
struct FogRange {
    float start;
    float end;
};
inline FogRange clientFogRange(float fogEndYards, float startScalar, float farClip) {
    float end = fogEndYards < 10.0f ? 10.0f : fogEndYards;
    if (farClip > 0.0f && end > farClip) end = farClip;
    const float scalar = startScalar < -1.0f ? -1.0f : (startScalar > 1.0f ? 1.0f : startScalar);
    return {end * scalar, end};
}

/// Whether a map takes the client's later fog: map 530 and up, Outland and
/// Northrend among them, with vertex shaders (0x007816f0 sets 0xd38acc from
/// the map id against 0x212). Every map before it keeps the linear fog.
inline bool mapUsesFogExponent(uint32_t mapId) {
    return mapId >= 530u;
}

/// The far clip's bounds (0x00780770): no nearer than 183 yards; no farther
/// than 1583 on the newer maps, 791 on the old ones.
inline constexpr float kFarClipMin = 183.33333f;
inline constexpr float kFarClipOldWorld = 791.66669f;
inline constexpr float kFarClipNewWorld = 1583.3334f;

/// How far a map may be drawn (0x00780770): 1583 yards from map 530 on -
/// but not Hellfire Ramparts (543) or Utgarde Pinnacle (575) - on a machine
/// with more than 1 GB of memory (0x0086b4c0), else 791. The maps before 530,
/// and those two, get 791 unless farClipOverride is 1 or more (it is 0 by
/// default, 0x0078e400).
inline float clientFarClipLimit(uint32_t mapId, bool farClipOverride = false,
                                bool moreThan1GB = true) {
    if (mapId < 530u || mapId == 543u || mapId == 575u)
        return farClipOverride ? kFarClipNewWorld : kFarClipOldWorld;
    return moreThan1GB ? kFarClipNewWorld : kFarClipOldWorld;
}

/// The far clip the client draws a map with for a view-distance setting (the
/// farclip cvar, 350 by default): the setting held between 183 yards and the
/// map's limit (0x00780770, applied by 0x00780800 and on entering a map by
/// 0x00781430). The camera takes it (0x00607b00) and DNInfo's far clip with
/// it (0x004f8410), which every light's fog end is held to (0x007f16f0) and
/// which the later fog ends at (0x007ecd80).
inline float clientFarClip(float setting, uint32_t mapId, bool farClipOverride = false,
                           bool moreThan1GB = true) {
    const float limit = clientFarClipLimit(mapId, farClipOverride, moreThan1GB);
    if (!(setting >= kFarClipMin)) return kFarClipMin;
    return setting > limit ? limit : setting;
}

/// What 0x007f3230 draws with when it has no light at all (DAT_00d39008 0):
/// fog out to 1e10 yards - so to the far clip (0x007f16f0) - starting half way,
/// drawn with an exponent of 4 on any map.
inline constexpr float kNoLightFogEnd = 1.0e10f;
inline constexpr float kNoLightFogStartScalar = 0.5f;
inline constexpr float kNoLightFogExponent = 4.0f;

/// The power that fog is drawn with for a light whose authored fog runs from
/// `start` to `end` yards (0x007ecd00): 1.5 for a range of 500 yards or more
/// (700 or the far clip, whichever is nearer, less 200), rising to 7 as the
/// range closes to nothing. The denser the fog the zone asked for, the faster
/// it thickens once its end has moved out to the far clip.
inline float clientFogExponent(float start, float end, float farClip) {
    const float reach = (farClip < 700.0f ? farClip : 700.0f) - 200.0f;
    const float range = end - start;
    if (range <= reach) return (1.0f - range / reach) * 5.5f + 1.5f;
    return 1.5f;
}

/// One light's fog as the client keeps it for blending (0x007ebff0, then
/// 0x007ecd80): the end no nearer than 10 yards, the start fraction kept to
/// [-1, 1], the exponent 1 - plain linear fog. On a map with the later fog an
/// end past 1000/36 yards moves out to the far clip and the authored range
/// becomes the exponent instead, and the start fraction is no less than zero.
struct LightFog {
    float end;
    float startScalar;
    float exponent;
};
inline LightFog clientLightFog(float endYards, float startScalar, float farClip, bool fogExponent) {
    LightFog f{endYards < 10.0f ? 10.0f : endYards,
               startScalar < -1.0f ? -1.0f : (startScalar > 1.0f ? 1.0f : startScalar), 1.0f};
    if (!fogExponent) return f;
    if (f.end > 27.777779f && farClip > 0.0f) {
        f.exponent = clientFogExponent(f.startScalar * f.end, f.end, farClip);
        f.end = farClip;
    }
    if (f.startScalar < 0.0f) f.startScalar = 0.0f;
    return f;
}

/// How much of a surface shows through the fog at `dist`, as the shaders
/// work it out: linear from the start to the end, raised to the exponent the
/// light blend gave (0x00873210 hands it to them beside the range).
inline float clientFogVisibility(float dist, float start, float end, float exponent) {
    const float span = end - start;
    float v = span > 0.0f ? (end - dist) / span : (dist < end ? 1.0f : 0.0f);
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return std::pow(v, exponent < 1.0f ? 1.0f : exponent);
}

/// How much of a colour is left `depth` yards under a liquid's surface, by
/// its LiquidType row (0x007f3230's tail): the fraction of MaxDarkenDepth the
/// camera is down, at most all of it, times the row's intensity for that
/// colour, taken off one. The client scales the HSV value by this
/// (0x007ed790), which for an RGB colour is the same as scaling it.
inline float liquidDarkenScale(float depth, float maxDarkenDepth, float intensity) {
    if (!(maxDarkenDepth > 0.0f)) return 1.0f;
    const float d = depth < 0.0f ? 0.0f : (depth > maxDarkenDepth ? maxDarkenDepth : depth);
    const float s = 1.0f - d / maxDarkenDepth * intensity;
    return s > 0.0f ? s : 0.0f;
}

/// How much of one WMO fog sphere shows at `dist` from its centre
/// (0x007a1150): all of it within the small radius, falling linearly to none
/// at the large one.
inline float wmoFogSphereWeight(float dist, float smallRadius, float largeRadius) {
    float d = dist < 0.0f ? 0.0f : (dist > largeRadius ? largeRadius : dist);
    if (d < smallRadius) return 1.0f;
    const float span = largeRadius - smallRadius;
    if (!(span > 0.0f)) return 0.0f;
    return 1.0f - (d - smallRadius) / span;
}

/// How far the interior fog has taken over from the zone's, by the camera's
/// distance from the way out: all of it 25 yards in (0x007f16f0, 0.04 a yard).
inline float wmoFogBlend(float distanceInside) {
    const float b = distanceInside * 0.04f;
    return b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b);
}

/// Which of a WMO fog record's two fogs the camera gets (0x007f16f0): the
/// air fog out of liquid. In liquid, the liquid fog - unless the LiquidType
/// has flag 0x20 and the MFOG record lacks 0x100, or the LiquidType has
/// 0x100 and the record lacks 0x10 - in which case the WMO adds no fog of its
/// own and the zone's (underwater) fog stays.
enum class WmoFogChoice { Air, Liquid, None };
inline WmoFogChoice wmoFogChoice(bool cameraInLiquid, uint32_t liquidTypeFlags, uint32_t mfogFlags) {
    if (!cameraInLiquid) return WmoFogChoice::Air;
    if ((liquidTypeFlags & 0x20u) && !(mfogFlags & 0x100u)) return WmoFogChoice::None;
    if ((liquidTypeFlags & 0x100u) && !(mfogFlags & 0x10u)) return WmoFogChoice::None;
    return WmoFogChoice::Liquid;
}

/// Whether the WMO's liquid fog replaces the zone's fog outright rather than
/// blending in over the first 25 yards: LiquidType flag 0x40 (0x007f16f0
/// copies it over the base fog, 0x007f1885).
inline bool wmoLiquidFogIsWhole(WmoFogChoice choice, uint32_t liquidTypeFlags) {
    return choice == WmoFogChoice::Liquid && (liquidTypeFlags & 0x40u) != 0;
}

// ---------------------------------------------------------------------------
// The sky dome.

/// The polar angles of the client's sky dome rows, as fractions of pi
/// (0xa41a90, read by 0x007f2470): zenith, the four bands, the horizon ring,
/// the nadir. 0x007f0530 colours them ch2, ch3, ch4, ch5, ch6, ch7, ch7.
inline constexpr float kSkyDomeRows[] = {0.0f, 0.17f, 0.20f, 0.23f, 0.24f, 0.25f, 1.0f};

/// The dawn and dusk sky glow's clock (0xaf4b7c, read by 0x007f0530): none
/// at 03:00, all of it at 06:30, none by 07:00; none at 20:30, all at 21:30,
/// none again by midnight.
inline constexpr CurveKey kSkyHighlightTime[] = {
    {0.125f, 0.0f}, {0.27083334f, 1.0f}, {0.29166666f, 0.0f},
    {0.85416669f, 0.0f}, {0.89583331f, 1.0f}, {0.99930555f, 0.0f}};
/// How the glow runs round the dome (0xaf4bac), keyed by the turn fraction
/// 0x007f0530 gives each of the dome's 24 columns.
inline constexpr CurveKey kSkyHighlightAzimuth[] = {
    {0.125f, 1.0f}, {0.375f, 0.0f}, {0.5f, -0.5f}, {0.625f, -0.7f}, {0.75f, -0.5f}, {0.875f, 0.0f}};
/// How much of the glow is up at day fraction `t`, for a light whose
/// LightParams.HighlightSky (DNInfo[0x15]) is `highlightSky` (0x007f0530:
/// the time curve times 0xd38c28).
inline float skyHighlightStrength(float t, float highlightSky) {
    return sampleCurve(kSkyHighlightTime, t) * highlightSky;
}
/// Where the glow's azimuth curve starts, from the camera's forward in world
/// space: atan2(y, x) of it as a turn fraction, plus a quarter (0x007f3920
/// keeps the angle in 0xd38b3c; 0x007f0530 adds 0.25). Looking straight up or
/// down the client takes atan2(z, x) instead. Column j of the dome, at world
/// azimuth pi/2 - 2 pi j / 24, is keyed at this less j/24.
inline float skyHighlightPhase(const glm::vec3& worldForward) {
    constexpr float kTwoPi = 6.2831855f;
    const float x = worldForward.x;
    const float y = worldForward.y;
    float a = (x * x + y * y <= 0.0001f) ? std::atan2(worldForward.z, x) : std::atan2(y, x);
    if (a < 0.0f) a += kTwoPi;
    float u = a * 0.15915494f + 0.25f;
    if (u > 1.0f) u -= 1.0f;
    return u;
}
/// One of the dome's banded rows (ch3..ch6) at a column whose azimuth curve
/// reads `az`, with the glow at strength `h` (0x007f0530): the row's colour
/// pulled toward ch3 by h, then toward the base colour as az rises to 1, or on
/// toward ch2 as it goes below 0.
inline glm::vec3 skyHighlightRow(const glm::vec3& row, const glm::vec3& skyMiddle,
                                 const glm::vec3& skyTop, float h, float az) {
    const glm::vec3 toward = row + (skyMiddle - row) * h;
    if (az >= 0.0f) return row + (toward - row) * ((1.0f - az) * h);
    const glm::vec3 lifted = toward + (skyTop - toward) * (0.7f * h);
    return toward + (lifted - toward) * (-az * h);
}

// ---------------------------------------------------------------------------
// The stars.

/// The stars' clock (0xaf4c20, read by 0x007ee0d0): all of them to 03:00,
/// gone by 04:30, back from 22:30 to all of them at midnight.
inline constexpr CurveKey kStarsTime[] = {
    {0.125f, 1.0f}, {0.1875f, 0.0f}, {0.9375f, 0.0f}, {1.0f, 1.0f}};
/// The stars model's alpha byte at day fraction `t`: the curve times 254,
/// plus one, truncated (0x007ee0d0). 0x009abd50 draws it only above 1.
inline int starsAlphaByte(float t) {
    return static_cast<int>(sampleCurve(kStarsTime, t) * 254.0f + 1.0f);
}
/// The stars model's opacity, 0 when the client would not draw it.
inline float starsAlpha(float t) {
    const int a = starsAlphaByte(t);
    return a > 1 ? static_cast<float>(a) / 255.0f : 0.0f;
}
/// The client's stars: a sky model of their own, drawn at the eye under the
/// procedural sky (0x009abb00 loads it, 0x009abd50 draws it). The name is
/// the client's (0xaa9844); the .mdl is read as the .m2 beside it.
inline constexpr const char* kStarsModelPath = "Environments\\Stars\\stars.mdl";

/// Where a view direction meets the dome, as its polar angle over pi. The dome
/// is a unit sphere centred cos(45 deg) below the eye (0x007f2470), which is
/// what puts the bands low on the sky. skybox.frag.glsl's domePolar is the
/// same computation; this one is for the tests.
inline float skyDomePolarFraction(const glm::vec3& unitDir) {
    constexpr float c = 0.70710678f;
    const float t = -c * unitDir.z +
                    std::sqrt(std::max(c * c * unitDir.z * unitDir.z - c * c + 1.0f, 0.0f));
    const float cosPhi = std::clamp(t * unitDir.z + c, -1.0f, 1.0f);
    return std::acos(cosPhi) * 0.31830989f;
}

// ---------------------------------------------------------------------------
// The clouds.

/// The cloud dome's rows (0x007f20e0, table 0xa41ad4): polar angles over pi,
/// zenith to the horizon ring, on the same sphere as the sky dome. Row i is
/// i/11 of the way out from the cloud texture's centre to its edge.
inline constexpr float kCloudDomeRows[12] = {0.0f,  0.025f, 0.05f, 0.075f, 0.1f,   0.125f,
                                             0.15f, 0.175f, 0.205f, 0.23f, 0.245f, 0.25f};
/// Each row's vertex alpha (0xa41b04): solid down to 8.8 degrees above the
/// horizon, half at 3.7, none at the horizon.
inline constexpr unsigned char kCloudDomeAlpha[12] = {255, 255, 255, 255, 255, 255,
                                                      255, 255, 255, 128, 0,   0};

/// How far from the cloud texture's centre (0 to 0.5 at the horizon ring) a
/// point of the dome at polar fraction `p` lies: piecewise linear between the
/// rows, as the mesh's UVs are. Past the horizon it runs on along the last
/// row's slope, so a body below the horizon still lights the clouds from the
/// right side (0x007ef920 places the sun and moon on the texture this way).
inline float cloudTextureRadius(float p) {
    constexpr int n = 12;
    if (p <= 0.0f) return 0.0f;
    for (int i = 1; i < n; ++i) {
        if (p <= kCloudDomeRows[i]) {
            const float a = kCloudDomeRows[i - 1];
            const float b = kCloudDomeRows[i];
            const float f = (p - a) / (b - a);
            return 0.5f * (static_cast<float>(i - 1) + f) / static_cast<float>(n - 1);
        }
    }
    const float slope = 0.5f / static_cast<float>(n - 1) / (kCloudDomeRows[n - 1] - kCloudDomeRows[n - 2]);
    return 0.5f + (p - kCloudDomeRows[n - 1]) * slope;
}

/// The cloud texture's opacity, 0..1, for a noise byte `k` over the
/// coverage threshold (0x007efd00 indexes the table 0x007edb50 builds:
/// 255 - 255 x 0.96^(k x 153/256), the 153 being 255 less the 0.6 cover it
/// was built for). At or under the threshold, none.
inline float cloudCoverageAlpha(float k) {
    if (k < 0.0f) return 0.0f;
    return 1.0f - std::pow(0.96f, k * (153.0f / 256.0f));
}

/// The coverage threshold for cloud cover `density` (float band 3): the
/// noise byte a texel has to clear, (1 - density) x 255. The client stores
/// it in a byte; cover past 0..1 is held there rather than wrapped.
inline float cloudCoverageThreshold(float density) {
    const float d = density < 0.0f ? 0.0f : (density > 1.0f ? 1.0f : density);
    return std::round((1.0f - d) * 255.0f);
}

/// Whether the clouds are lit by the sun at day fraction `t` rather than the
/// moon: from 04:50 to 22:10 (0x007efae0).
inline bool cloudsLitBySun(float t) { return t >= 0.2013889f && t < 0.9236111f; }

/// How strongly the sun or moon lights the clouds in weather `storm` (0..1):
/// 1 - 0.75 storm, the time curve at 0xaf4be0 being 1 at every key
/// (0x007efae0).
inline float cloudGlow(float storm) { return 1.0f - 0.75f * storm; }

/// The height, in texels, of the light over the cloud texture: 64, flatter
/// lighting in weather (0x007efae0: 64 + 192 storm).
inline float cloudLightHeight(float storm) { return 64.0f + 192.0f * storm; }

}  // namespace wowee::rendering::daynight
