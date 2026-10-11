#version 460
#ifdef RT_HARDWARE
#extension GL_EXT_ray_query : require
#endif
// One sample per trace pixel of each enabled term:
//   sun visibility - a ray toward a point on the sun's disc
//   ambient occlusion - a short cosine-weighted ray
//   point light visibility - a ray toward one of the frame's point lights
// Noisy by design; rt_temporal and rt_filter turn it into something usable.
// The diffuse light that bounces is the probe volume's (rt_gi_trace), which
// the surfaces read for themselves.
//
// The point lights (rt_point_lights.hpp) are taken whole and shadowed by one
// of them. Their unshadowed light at the pixel - every light's colour by its
// falloff and N.L - is exact and goes out as it is (uPointOut). Their shadow
// is one ray, to a light picked in proportion to what it gives here, whose
// visibility is an unbiased estimate of the fraction of that light which
// arrives: the sum of each light's share times its own visibility. That
// fraction is what the temporal and spatial filters smooth, in the third
// channel beside the sun's and the occlusion's, and the surfaces multiply the
// two back together (rt_lighting.glsli). A flickering torch then flickers at
// once, without waiting for its shadow to converge, and a dozen lights cost
// one ray a pixel, not a dozen.
//
// A building's own lights (MOLT) are baked into its interior's vertex
// colours already. Their part of the unshadowed light goes out beside it, in
// uPointOut's alpha (the brightest channel's worth), so those surfaces can
// take the rest of the light and only the shadows of these
// (rt_lighting.glsli).

#include "rt_common.glsli"
#ifdef RT_HARDWARE
#include "rt_trace_hw.glsli"
#else
#include "rt_trace_sw.glsli"
#endif

layout(set = 0, binding = 3) uniform sampler2D uGbufCur;
layout(set = 0, binding = 4, rgba16f) uniform writeonly image2D uRaw;
layout(set = 0, binding = 11, rgba16f) uniform writeonly image2D uPointOut;  // a = the baked lights' part

// Must match rt_lights::PointLightGPU.
struct RtPointLight {
    vec4 positionEnd;  // xyz, w = where its light ends (yards), negative for a baked one
    vec4 colorStart;   // rgb, w = where its falloff starts
};
layout(std430, set = 0, binding = 12) readonly buffer RtPointLights { RtPointLight rtPointLights[]; };

// rt_lights::attenuation: all within the start, none past the end, a
// smoothed ramp between.
float rtLightFalloff(float d, float start, float end) {
    if (d >= end) return 0.0;
    start = clamp(start, 0.0, end);
    if (d <= start) return 1.0;
    float t = (end - d) / (end - start);
    return t * t * (3.0 - 2.0 * t);
}

// A light is a small sphere, so its shadows have a soft edge.
const float RT_LIGHT_RADIUS = 0.15;
// A light sits inside its lamp - a lantern's glass, a brazier's bowl - so a
// shadow ray stops this short of it rather than finding the lamp itself.
const float RT_LIGHT_STANDOFF = 0.45;

layout(local_size_x = 8, local_size_y = 8) in;

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(px, ivec2(pass.extent.xy)))) return;

    vec4 g = texelFetch(uGbufCur, px, 0);
    float dist = rtGbufDist(g);
    if (dist <= 0.0) {
        imageStore(uRaw, px, vec4(1.0, 1.0, 1.0, 0.0));
        imageStore(uPointOut, px, vec4(0.0));
        return;
    }
    vec3 n = rtGbufNormal(g);
    vec3 p = rtGbufWorld(g, px);
    vec3 origin = p + n * rtSurfaceOffset(dist);
    uint rng = rtSeed(uvec2(px), uint(pass.cameraPos.w));
    uint mode = uint(pass.params.x + 0.5);

    // Whether a unit stopped one of this pixel's rays: a unit moves, and
    // its shadow must not stay behind in the history (rt_temporal).
    bool unitHit = false;

    float sun = 0.0;
    vec3 L = normalize(pass.sunDir.xyz);
    if (pass.sunColor.w > 0.5 && dot(n, L) > 0.0) {
        vec3 sd = rtConeSample(L, pass.sunDir.w, rng);
        // Within the shadow map's reach the units' sun shadows are the map's:
        // drawn this frame in the pose on screen. Traced, they came a frame
        // late and settled over several more, and a running body trailed a
        // ghost of its staff and legs over the ground and itself.
        vec3 fromMap = p - pass.unitMap.xyz;
        rtSkipUnits = dot(fromMap, fromMap) < pass.unitMap.w * pass.unitMap.w;
        bool byUnit;
        sun = rtOccludedBy(origin, sd, pass.params.w, rng, byUnit) ? 0.0 : 1.0;
        rtSkipUnits = false;
        unitHit = unitHit || byUnit;
    }

    float ao = 1.0;
    if (mode >= RT_MODE_AO) {
        vec3 ad = rtCosineHemisphere(n, rng);
        float t;
        vec3 hn;
        vec4 hs;
        if (rtClosest(origin, ad, pass.params.y, rng, t, hn, hs)) ao = t / pass.params.y;
    }

    // The point lights: their sum, and one of them picked by weighted
    // reservoir sampling in the same pass.
    vec3 pointSum = vec3(0.0);
    float bakedSum = 0.0;
    float weightSum = 0.0;
    int picked = -1;
    uint lightCount = uint(pass.params.z + 0.5);
    for (uint i = 0u; i < lightCount; ++i) {
        RtPointLight light = rtPointLights[i];
        vec3 toLight = light.positionEnd.xyz - p;
        float d2 = dot(toLight, toLight);
        float end = abs(light.positionEnd.w);
        if (d2 >= end * end) continue;
        float d = sqrt(d2);
        float ndl = d > 1e-4 ? dot(n, toLight / d) : 1.0;
        if (ndl <= 0.0) continue;
        vec3 c = light.colorStart.rgb * (rtLightFalloff(d, light.colorStart.w, end) * ndl);
        float w = max(c.r, max(c.g, c.b));
        if (w <= 0.0) continue;
        pointSum += c;
        if (light.positionEnd.w < 0.0) bakedSum += w;
        weightSum += w;
        if (rtRandom(rng) * weightSum < w) picked = int(i);
    }
    float pointVis = 1.0;
    if (picked >= 0) {
        RtPointLight light = rtPointLights[picked];
        vec3 jitter = (vec3(rtRandom(rng), rtRandom(rng), rtRandom(rng)) * 2.0 - 1.0) * RT_LIGHT_RADIUS;
        vec3 toLight = light.positionEnd.xyz + jitter - origin;
        float d = length(toLight);
        float reach = d - min(RT_LIGHT_STANDOFF, 0.5 * d);
        if (d > 1e-3 && reach > 0.0) {
            bool byUnit;
            pointVis = rtOccludedBy(origin, toLight / d, reach, rng, byUnit) ? 0.0 : 1.0;
            unitHit = unitHit || byUnit;
        }
    }

    imageStore(uRaw, px, vec4(sun, ao, pointVis, unitHit ? 1.0 : 0.0));
    imageStore(uPointOut, px, vec4(pointSum, bakedSum));
}
