#version 450
// Blend this frame's rays into the tiles of the probes that cast them
// (RtGiProbes): one workgroup per probe on the list, one invocation per texel
// of its 10x10 visibility tile, the first 8x8 of which also do its irradiance
// tile.
//
// An irradiance texel is the cosine-weighted mean of the light the rays
// brought back around its direction - normalised by the weights, so a probe
// under an open sky of radiance S holds S whichever way the texel faces, the
// same units as the zone's flat ambient. A visibility texel is the mean and
// mean square of the distance they travelled, in a much narrower lobe.
//
// Border texels compute what the interior texel they copy computes, so a
// tile is finished in one pass with no copy after it. Every invocation reads
// only its own texel's history and writes only that texel, the reads all
// before the barrier and the writes after.
//
// The history is blended toward with the hysteresis, but never more than the
// probe's age allows: a probe that has just come into the grid takes its
// first estimate as it is and averages the next ones evenly until the
// hysteresis takes over. The age rides in the irradiance tile's alpha.

#include "rt_common.glsli"
#include "rt_gi.glsli"

layout(set = 0, binding = 2, rgba16f) uniform image2D uGiAtlasStore;
layout(std430, set = 0, binding = 3) readonly buffer GiList { uint giList[]; };
layout(std430, set = 0, binding = 4) readonly buffer GiRays { uvec2 giRays[]; };

layout(local_size_x = 10, local_size_y = 10) in;

shared vec4 sRay[GI_RAYS];  // rgb radiance, a distance (clamped)
shared vec3 sDir[GI_RAYS];
shared uint sBuried;

const float MAX_AGE = 255.0;
// The visibility lobe: cos^10 is about 20 degrees to half height, wide enough
// that 64 rays leave few texels with nothing in reach.
const float VIS_SHARPNESS = 10.0;

// The interior texel a tile texel copies (rt_gi_volume.hpp's borderSource).
ivec2 borderSource(ivec2 t, int texels) {
    bool loX = t.x < 0, hiX = t.x >= texels, loY = t.y < 0, hiY = t.y >= texels;
    if ((loX || hiX) && (loY || hiY)) return ivec2(loX ? texels - 1 : 0, loY ? texels - 1 : 0);
    if (loX || hiX) return ivec2(loX ? 0 : texels - 1, texels - 1 - t.y);
    if (loY || hiY) return ivec2(texels - 1 - t.x, loY ? 0 : texels - 1);
    return t;
}

vec3 texelDirection(ivec2 t, int texels) {
    return giOctDecode((vec2(t) + 0.5) / float(texels) * 2.0 - 1.0);
}

float maxOf(vec3 v) { return max(v.x, max(v.y, v.z)); }

void main() {
    uint entry = giList[gl_WorkGroupID.x];
    int cascade = int((entry >> 20) & 0xFu);
    uint slot = entry & 0xFFFFFu;
    bool reset = (entry & 0x80000000u) != 0u;
    ivec3 s = ivec3(int(slot) % GI_PROBES.x, (int(slot) / GI_PROBES.x) % GI_PROBES.y,
                    int(slot) / (GI_PROBES.x * GI_PROBES.y));
    // Past the cell diagonal and a half, a distance says nothing a shading
    // point could test against, and would only inflate the variance.
    float maxDist = pass.giVolume[cascade].w * 2.6;

    uint i = gl_LocalInvocationIndex;
    if (i == 0u) sBuried = 0u;
    barrier();
    if (i < uint(GI_RAYS)) {
        uvec2 packed = giRays[gl_WorkGroupID.x * uint(GI_RAYS) + i];
        vec2 rg = unpackHalf2x16(packed.x);
        vec2 bd = unpackHalf2x16(packed.y);
        if (bd.y < 0.0) atomicAdd(sBuried, 1u);
        vec4 ray = vec4(rg, bd.x, min(abs(bd.y), maxDist));
        sRay[i] = any(isnan(ray)) || any(isinf(ray)) ? vec4(0.0, 0.0, 0.0, maxDist) : ray;
        sDir[i] = normalize(mat3(pass.giRotation) * giFibonacci(i));
    }

    ivec2 t = ivec2(gl_LocalInvocationID.xy);
    bool doIrr = t.x < GI_IRR_TILE && t.y < GI_IRR_TILE;
    ivec2 irrTexel = giIrradianceTileOrigin(cascade, s) + t;
    ivec2 visTexel = giVisibilityTileOrigin(cascade, s) + t;
    ivec2 irrFirst = giIrradianceTileOrigin(cascade, s) + 1;
    vec4 prevIrr = doIrr ? imageLoad(uGiAtlasStore, irrTexel) : vec4(0.0);
    vec4 prevVis = imageLoad(uGiAtlasStore, visTexel);
    float age = reset ? 0.0 : imageLoad(uGiAtlasStore, irrFirst).a;
    barrier();

    // Whatever a probe held that is not a number is forgotten rather than
    // blended: through the bounces it would spread to every probe that sees
    // this one, and from them to the whole volume.
    bool bad = isnan(age) || isinf(age) || any(isnan(prevIrr)) || any(isinf(prevIrr));
    if (bad) {
        age = 0.0;
        prevIrr = vec4(0.0);
    }
    if (any(isnan(prevVis.rg)) || any(isinf(prevVis.rg))) prevVis = vec4(maxDist, maxDist * maxDist, 0.0, 0.0);

    float hysteresis = min(pass.giSky.w, age / (age + 1.0));
    float newAge = min(age + 1.0, MAX_AGE);

    vec3 visDir = texelDirection(borderSource(t - 1, GI_VIS_TEXELS), GI_VIS_TEXELS);
    vec2 moments = vec2(0.0);
    float visWeight = 0.0;
    vec3 irrDir = texelDirection(borderSource(t - 1, GI_IRR_TEXELS), GI_IRR_TEXELS);
    vec3 irr = vec3(0.0);
    float irrWeight = 0.0;
    for (int r = 0; r < GI_RAYS; ++r) {
        vec4 ray = sRay[r];
        vec3 d = sDir[r];
        float cv = max(dot(visDir, d), 0.0);
        float wv = pow(cv, VIS_SHARPNESS);
        moments += vec2(ray.a, ray.a * ray.a) * wv;
        visWeight += wv;
        float wi = max(dot(irrDir, d), 0.0);
        irr += ray.rgb * wi;
        irrWeight += wi;
    }

    // A texel no ray came near keeps what it had - or, new, says the probe
    // sees far, which tests nothing.
    vec2 visOut = reset ? vec2(maxDist, maxDist * maxDist) : prevVis.rg;
    if (visWeight > 1e-4) {
        moments /= visWeight;
        visOut = mix(moments, prevVis.rg, hysteresis);
    }
    // Under the ground or inside a wall: a quarter of the rays meeting
    // something from behind (rtClosestEx) is more than a probe outside ever
    // casts. Such a probe sees only dark undersides, and the visibility tiles
    // are too coarse to shut it out of a point just above the ground - its
    // rays skim the underside at every distance - so it would darken every
    // field it lies under, by about a quarter. The readers skip it
    // (giSampleCascade). DDGI's probe classification, Majercik et al. 2021.
    float buried = sBuried * 4u > uint(GI_RAYS) ? 1.0 : 0.0;
    imageStore(uGiAtlasStore, visTexel, vec4(visOut, buried, 0.0));

    if (doIrr) {
        vec3 result = irr / max(irrWeight, 1e-4);
        float h = hysteresis;
        // A change far past the noise - a door of light opening, the sun
        // coming out - is followed faster than the noise is averaged.
        if (maxOf(abs(result - prevIrr.rgb)) > 0.4) h *= 0.5;
        imageStore(uGiAtlasStore, irrTexel, vec4(mix(result, prevIrr.rgb, h), newAge));
    }
}
