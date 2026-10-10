#version 450
// How much the probes' ratio to the open field is lifted for this view
// (RtGiProbes): one workgroup that reads the volume at 256 of the surfaces on
// screen, as the surfaces will, and writes the lift into a texel of the
// atlas the surfaces read it from (GI_EXPOSURE_TEXEL).
//
// The open field the ratio is measured against is an idealisation: level
// ground under a whole sky. Goldshire is a clearing in a forest, and even its
// open road sees oaks over half its sky, so the road came out at two thirds
// of the flat ambient - while Light.dbc's ambient for Elwynn was set by
// artists looking at that same road, trees and all. So the open field is
// taken to be what the better lit half of the view gets: the mean of the
// upper half of the samples' log ratios is brought up to one, and the rest
// keeps its place below it - the eaves, the alley, the doorway still darker
// than the road. Only ever a lift, and never more than WOWEE_GI_NORMALIZE:
// a view that is all cave stays a cave, only less so. Eased over a second or
// so, so a turn of the camera does not pump the light.

#include "rt_common.glsli"
#include "rt_gi.glsli"

layout(set = 0, binding = 1) uniform sampler2D uGiAtlas;
layout(set = 0, binding = 2, rgba16f) uniform image2D uGiAtlasStore;
layout(set = 0, binding = 5) uniform sampler2D uGbufCur;

layout(local_size_x = 16, local_size_y = 16) in;

shared float sLog[256];
shared float sWeight[256];

void main() {
    uint i = gl_LocalInvocationIndex;
    uint rng = rtSeed(uvec2(gl_LocalInvocationID.xy), uint(pass.cameraPos.w));
    vec2 jitter = vec2(rtRandom(rng), rtRandom(rng));
    ivec2 px = clamp(ivec2((vec2(gl_LocalInvocationID.xy) + jitter) / 16.0 * pass.extent.xy),
                     ivec2(0), ivec2(pass.extent.xy) - 1);

    float lg = 0.0;
    float weight = 0.0;
    vec4 g = texelFetch(uGbufCur, px, 0);
    if (rtGbufDist(g) > 0.0 && pass.giVolume[0].w > 0.0) {
        vec3 n = rtGbufNormal(g);
        vec3 p = rtGbufWorld(g, px);
        vec3 v = normalize(pass.cameraPos.xyz - p);
        vec4 e = giIrradiance(uGiAtlas, pass.giVolume[0], pass.giVolumeMod[0], pass.giVolume[1],
                              pass.giVolumeMod[1], p, n, v);
        if (e.a > 0.5 && !any(isnan(e))) {
            float open = max(dot(giOpenReference(pass.giSky.rgb, pass.giGround.rgb, n), GI_LUMA), 1e-4);
            lg = log(clamp(dot(e.rgb, GI_LUMA) / open, 0.05, 4.0));
            weight = 1.0;
        }
    }
    sLog[i] = lg;
    sWeight[i] = weight;
    barrier();

    if (i != 0u) return;
    float sum = 0.0, count = 0.0;
    for (int k = 0; k < 256; ++k) {
        sum += sLog[k] * sWeight[k];
        count += sWeight[k];
    }
    vec4 prev = imageLoad(uGiAtlasStore, GI_EXPOSURE_TEXEL);
    float prevLift = prev.r > 0.0 && !isnan(prev.r) ? prev.r : 1.0;
    float lift = prevLift;
    if (count >= 16.0) {
        float mean = sum / count;
        float upper = 0.0, upperCount = 0.0;
        for (int k = 0; k < 256; ++k) {
            if (sWeight[k] > 0.0 && sLog[k] >= mean) {
                upper += sLog[k];
                upperCount += 1.0;
            }
        }
        upper /= max(upperCount, 1.0);
        float target = clamp(exp(-upper), 1.0, max(pass.giRayLength.z, 1.0));
        lift = mix(target, prevLift, prev.r > 0.0 ? 0.95 : 0.0);
    }
    imageStore(uGiAtlasStore, GI_EXPOSURE_TEXEL, vec4(lift, 0.0, 0.0, 1.0));
}
