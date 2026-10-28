#version 460
#ifdef RT_HARDWARE
#extension GL_EXT_ray_query : require
#endif
// The rays of this frame's probes (RtGiProbes): one workgroup per probe on the
// list, one ray per invocation, over a spherical Fibonacci set turned at
// random each frame so successive updates sample new directions.
//
// What a ray brings back is the light leaving what it hit toward the probe:
// the surface's colour times the sun on it (with its own shadow ray) and
// times the irradiance the volume held there last - which is the light of
// every bounce before, so each update adds one more. A ray that escapes sees
// the sky above the horizon and the open ground's light below it, the two the
// zone's flat ambient stands for (giOpenReference). The distance it travelled
// goes along for the visibility tiles - negative for a ray that met the
// ground from beneath or a solid from inside, as only a probe buried in them
// casts: the update counts those and switches the probe off.

#include "rt_common.glsli"
#ifdef RT_HARDWARE
#include "rt_trace_hw.glsli"
#else
#include "rt_trace_sw.glsli"
#endif
#include "rt_gi.glsli"

layout(set = 0, binding = 1) uniform sampler2D uGiAtlas;
layout(std430, set = 0, binding = 3) readonly buffer GiList { uint giList[]; };
layout(std430, set = 0, binding = 4) writeonly buffer GiRays { uvec2 giRays[]; };

layout(local_size_x = 64) in;

// The half-width of a direction's share of the sphere, as a cone's tangent:
// 64 directions take 4 pi / 64 steradians each, a cone of some 14 degrees.
const float RT_GI_CELL_TAN = 0.26;

// The light already arriving at a hit point, from what the atlas holds. Where
// the volume does not reach, the open field's: no better guess, and the one
// the surfaces themselves fall back on.
vec3 bounceAt(vec3 p, vec3 n, vec3 v) {
    vec3 open = giOpenReference(pass.giSky.rgb, pass.giGround.rgb, n);
    if (pass.giPrevVolume[0].w <= 0.0) return open;
    vec4 e = giIrradiance(uGiAtlas, pass.giPrevVolume[0], pass.giPrevVolumeMod[0],
                          pass.giPrevVolume[1], pass.giPrevVolumeMod[1], p, n, v);
    return mix(open, e.rgb, e.a);
}

void main() {
    uint entry = giList[gl_WorkGroupID.x];
    int cascade = int((entry >> 20) & 0xFu);
    uint slot = entry & 0xFFFFFu;
    ivec3 s = ivec3(int(slot) % GI_PROBES.x, (int(slot) / GI_PROBES.x) % GI_PROBES.y,
                    int(slot) / (GI_PROBES.x * GI_PROBES.y));
    vec4 vol = pass.giVolume[cascade];
    ivec3 local = (s - ivec3(pass.giVolumeMod[cascade].xyz) + GI_PROBES) % GI_PROBES;
    vec3 origin = giProbeWorld(vol, local);

    uint r = gl_LocalInvocationIndex;
    vec3 dir = normalize(mat3(pass.giRotation) * giFibonacci(r));
    uint rng = rtSeed(uvec2(slot, r + uint(cascade) * 64u), uint(pass.cameraPos.w));
    float tMax = cascade == 0 ? pass.giRayLength.x : pass.giRayLength.y;

    // The direction's own ray first, which alone says how far the probe
    // sees that way (the visibility); any more the settings ask for (pass.rays.w)
    // go to other points of the same direction's share of the sphere and
    // only add to its light.
    vec3 radianceSum = vec3(0.0);
    float dist = tMax;
    uint giRaysEach = rtRayCount(pass.rays.w);
    for (uint k = 0u; k < giRaysEach; ++k) {
    vec3 rayDir = k == 0u ? dir : rtConeSample(dir, RT_GI_CELL_TAN, rng);
    vec3 radiance;
    float rayDist = tMax;
    float t;
    vec3 hn;
    vec4 hs;
    bool below;
    bool hit = rtClosestEx(origin, rayDir, tMax, rng, t, hn, hs, below);
    if (hit && below) {
        radiance = vec3(0.0);
        rayDist = -max(t, 1e-3);
    } else if (hit) {
        rayDist = t;
        // A sliver of a triangle has no normal to speak of; facing the ray
        // back is as good as any, and better than a NaN that the bounces
        // would carry into every probe of the volume.
        if (any(isnan(hn)) || dot(hn, hn) < 0.5) hn = -rayDir;
        vec3 hp = origin + rayDir * t;
        vec3 L = normalize(pass.sunDir.xyz);
        vec3 light = bounceAt(hp, hn, -rayDir);
        float ndl = dot(hn, L);
        if (pass.sunColor.w > 0.5 && ndl > 0.0 &&
            !rtOccluded(hp + hn * rtSurfaceOffset(t), L, pass.params.w, rng)) {
            light += pass.sunColor.rgb * ndl;
        }
        radiance = hs.rgb * light;
    } else {
        radiance = rayDir.z >= 0.0 ? pass.giSky.rgb : pass.giGround.rgb;
    }
    if (k == 0u) dist = rayDist;
    if (any(isnan(radiance)) || any(isinf(radiance))) radiance = vec3(0.0);
    radianceSum += min(radiance, vec3(16.0));
    }
    vec3 radiance = radianceSum / float(giRaysEach);

    if (any(isnan(radiance)) || any(isinf(radiance))) radiance = vec3(0.0);
    radiance = min(radiance, vec3(16.0));  // half floats, and no fireflies
    giRays[gl_WorkGroupID.x * uint(GI_RAYS) + r] =
        uvec2(packHalf2x16(radiance.rg), packHalf2x16(vec2(radiance.b, dist)));
}
