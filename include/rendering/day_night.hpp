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

// ---------------------------------------------------------------------------
// The sky dome.

/// The polar angles of the client's sky dome rows, as fractions of pi
/// (0xa41a90, read by 0x007f2470): zenith, the four bands, the horizon ring,
/// the nadir. 0x007f0530 colours them ch2, ch3, ch4, ch5, ch6, ch7, ch7.
inline constexpr float kSkyDomeRows[] = {0.0f, 0.17f, 0.20f, 0.23f, 0.24f, 0.25f, 1.0f};

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
