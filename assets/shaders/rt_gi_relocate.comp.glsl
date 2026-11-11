#version 460
#ifdef RT_HARDWARE
#extension GL_EXT_ray_query : require
#endif
// Where this frame's probes trace from (RtGiProbes): one workgroup per probe
// on the list, one ray per invocation, recorded just before rt_gi_trace.
//
// A probe on its grid point can stand a hand's width from a wall, or inside
// one. Its rays then start on the surface - through the cracks between a
// building's pieces they see the sky - or behind it, and the light it holds is
// what the room's corner does not get (the Tirisfal tower room's band,
// user162). DDGI moves such a probe off its point (Majercik et al. 2021,
// probe relocation), but by a few steps a frame, which leaves the place it
// ends up depending on the frames before. Here the move is worked out afresh
// each time the probe is updated, from its grid point and the scene alone:
// the fixed spherical Fibonacci set, unturned, out to half a cell.
//
// - Most of its rays meeting a solid from inside (a quarter or more, as the
//   update counts a buried probe): through the nearest such face and half a
//   yard past it, if that is within a quarter of a cell.
// - Otherwise, a surface nearer than half a yard: away from it - from all of
//   them that near, weighted by how near - until it is half a yard off, at
//   most a quarter of a cell, and only as far as the probe's own rays saw
//   the way free.
// Nothing else changes: the probe's light still blends over its updates.

#include "rt_common.glsli"
#ifdef RT_HARDWARE
#include "rt_trace_hw.glsli"
#else
#include "rt_trace_sw.glsli"
#endif
#include "rt_gi.glsli"

layout(set = 0, binding = 2, rgba16f) uniform image2D uGiAtlasStore;
layout(std430, set = 0, binding = 3) readonly buffer GiList { uint giList[]; };

layout(local_size_x = 64) in;

// How near a surface a probe may stand, yards.
const float GI_CLEAR = 0.5;

shared vec4 sHit[GI_RAYS];  // xyz direction, w distance (negative: met from inside, 1e9: nothing)

void main() {
    uint entry = giList[gl_WorkGroupID.x];
    int cascade = int((entry >> 20) & 0xFu);
    uint slot = entry & 0xFFFFFu;
    ivec3 s = ivec3(int(slot) % GI_PROBES.x, (int(slot) / GI_PROBES.x) % GI_PROBES.y,
                    int(slot) / (GI_PROBES.x * GI_PROBES.y));
    vec4 vol = pass.giVolume[cascade];
    ivec3 local = (s - ivec3(pass.giVolumeMod[cascade].xyz) + GI_PROBES) % GI_PROBES;
    vec3 origin = giProbeWorld(vol, local);
    float spacing = vol.w;
    float maxMove = 0.25 * spacing;
    float reach = 0.5 * spacing;

    uint r = gl_LocalInvocationIndex;
    vec3 dir = giFibonacci(r);
    // Seeded by the probe's place in the world, not its slot or the frame.
    ivec3 cell = ivec3(vol.xyz) + local;
    uint rng = rtSeed(uvec2(uint(cell.x) * 73856093u ^ uint(cell.y) * 19349663u ^ uint(cell.z) * 83492791u, r),
                      uint(cascade));
    // Leaf cards are a loose volume of leaves, not a wall to stand off.
    rtCutoutsOpen = true;
    float t;
    vec3 hn;
    vec4 hs;
    bool below;
    float dist = 1e9;
    if (rtClosestEx(origin, dir, reach, rng, t, hn, hs, below)) dist = below ? -max(t, 1e-3) : t;
    rtCutoutsOpen = false;
    sHit[r] = vec4(dir, dist);
    barrier();
    if (r != 0u) return;

    uint backs = 0u;
    float backT = 1e9;
    vec3 backDir = vec3(0.0);
    float frontT = 1e9;
    vec3 away = vec3(0.0);
    for (int i = 0; i < GI_RAYS; ++i) {
        vec4 h = sHit[i];
        if (h.w < 0.0) {
            ++backs;
            if (-h.w < backT) { backT = -h.w; backDir = h.xyz; }
        } else if (h.w < 1e8) {
            frontT = min(frontT, h.w);
            if (h.w < GI_CLEAR) away -= h.xyz * (GI_CLEAR - h.w);
        }
    }

    vec3 offset = vec3(0.0);
    if (float(backs) >= 0.25 * float(GI_RAYS)) {
        // Inside: out through the nearest face it is behind.
        float move = backT + GI_CLEAR;
        if (move <= maxMove) offset = backDir * move;
    } else if (frontT < GI_CLEAR && dot(away, away) > 1e-8) {
        vec3 a = normalize(away);
        // How far the way is free along a: the nearest front hit among the
        // rays that lean that way, less the clearance it needs there.
        float free = reach;
        for (int i = 0; i < GI_RAYS; ++i) {
            vec4 h = sHit[i];
            float c = dot(h.xyz, a);
            if (c > 0.7 && h.w >= 0.0 && h.w < 1e8) free = min(free, h.w * c - GI_CLEAR);
        }
        float move = min(min(GI_CLEAR - frontT, maxMove), max(free, 0.0));
        offset = a * move;
    }
    imageStore(uGiAtlasStore, giOffsetTexel(cascade, s), vec4(offset, 1.0));
}
