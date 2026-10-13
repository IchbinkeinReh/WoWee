#version 460
#ifdef RT_HARDWARE
#extension GL_EXT_ray_query : require
#endif
// Scene depth -> the tracer's G-buffer, at trace resolution: world normal
// reconstructed from depth, the distance to the camera (0 for sky), and
// which mover the surface is, if any (rtGbufMover).
//
// Built with RT_MSAA when the depth is multisampled and could not be
// resolved; sample 0 stands for the pixel then; and with RT_HARDWARE for the
// hardware tracer, as rt_trace.
//
// A unit drawn see-through - the player's own body as the camera closes in,
// a ghost, one fading in - writes depth all the same, so that its own parts
// sort; but what shows through it is what stands behind, and that is what
// has to be lit. Where the depth is such a unit's, the G-buffer takes the
// first surface past it instead, found by a ray from the camera that passes
// every unit by. The unit itself then finds nothing of itself in the result
// and is lit without it (rtLightSeeThrough, rt_lighting.glsli).

#include "rt_common.glsli"
#include "rt_movers.glsli"
#ifdef RT_HARDWARE
#include "rt_trace_hw.glsli"
#else
#include "rt_trace_sw.glsli"
#endif

#ifdef RT_MSAA
layout(set = 0, binding = 1) uniform sampler2DMS uSceneDepth;
#else
layout(set = 0, binding = 1) uniform sampler2D uSceneDepth;
#endif
layout(set = 0, binding = 2, rgba16f) uniform writeonly image2D uGbuf;

layout(local_size_x = 8, local_size_y = 8) in;

// The movers whose boxes reach into this group's 8x8 texels.
const uint TILE_MOVERS = 128u;
shared uint sMovers[TILE_MOVERS];
shared uint sMoverCount;

float loadDepth(ivec2 p) {
    p = clamp(p, ivec2(0), ivec2(pass.sceneExtent.xy) - 1);
#ifdef RT_MSAA
    return texelFetch(uSceneDepth, p, 0).r;
#else
    return texelFetch(uSceneDepth, p, 0).r;
#endif
}

// Relative to the camera (rtRelFromDepth): the normal is made from the
// differences between neighbours, which world coordinates would round away.
vec3 relAt(ivec2 p, float depth) {
    return rtRelFromDepth((vec2(p) + 0.5) * pass.sceneExtent.zw, depth);
}

// The mover a world position is in, 1 + its index, 0 for none: inside its box
// this frame, the nearest one's where boxes overlap (the list is nearest
// first). See-through ones only when asked for.
int moverAt(vec3 w, uint listed, bool seeThrough) {
    int mover = 0;
    for (uint k = 0u; k < listed; ++k) {
        int i = int(sMovers[k]);
        if ((mover == 0 || i + 1 < mover) && (seeThrough || rtMovers[i].hi.w < 0.5) &&
            all(greaterThanEqual(w, rtMovers[i].lo.xyz)) && all(lessThanEqual(w, rtMovers[i].hi.xyz))) {
            mover = i + 1;
        }
    }
    return mover;
}

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);

    // The tile's movers, the group together: of hundreds on screen a tile
    // sees a few.
    uint lid = gl_LocalInvocationIndex;
    if (lid == 0u) sMoverCount = 0u;
    barrier();
    vec4 tile = vec4(vec2(gl_WorkGroupID.xy * 8u), vec2(gl_WorkGroupID.xy * 8u + 8u));
    int movers = min(rtMoverCount(), RT_MAX_MOVERS);
    for (int i = int(lid); i < movers; i += 64) {
        vec4 r = rtMovers[i].rect;
        if (r.x < tile.z && r.z > tile.x && r.y < tile.w && r.w > tile.y) {
            uint k = atomicAdd(sMoverCount, 1u);
            if (k < TILE_MOVERS) sMovers[k] = uint(i);
        }
    }
    barrier();

    if (any(greaterThanEqual(px, ivec2(pass.extent.xy)))) return;

    // The nearest surface in this texel's block of scene pixels.
    ivec2 base = rtBlockBase(px);
    int sub = 0;
    float d = 1.0;
    for (int i = 0; i < 4; ++i) {
        float di = loadDepth(base + ivec2(i & 1, i >> 1));
        if (di < d) {
            d = di;
            sub = i;
        }
    }
    if (d >= 1.0) {
        imageStore(uGbuf, px, vec4(0.0, 0.0, 0.0, 0.0));
        return;
    }
    ivec2 sp = base + ivec2(sub & 1, sub >> 1);
    vec3 p = relAt(sp, d);

    // Pick, on each axis, the neighbour on the same surface - the one whose
    // depth continues the slope best - so edges do not bend the normal.
    float dl = loadDepth(sp + ivec2(-1, 0)), dr = loadDepth(sp + ivec2(1, 0));
    float dd = loadDepth(sp + ivec2(0, -1)), du = loadDepth(sp + ivec2(0, 1));
    vec3 dx = abs(dr - d) < abs(d - dl) ? relAt(sp + ivec2(1, 0), dr) - p
                                        : p - relAt(sp + ivec2(-1, 0), dl);
    vec3 dy = abs(du - d) < abs(d - dd) ? relAt(sp + ivec2(0, 1), du) - p
                                        : p - relAt(sp + ivec2(0, -1), dd);
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, p) > 0.0) n = -n;

    // Which mover this is, if any. A mover moves on its own: its history is
    // found where it was (rt_temporal), and where it stood when this is read
    // - a frame on - may be bare ground (rtLightAt).
    uint listed = min(sMoverCount, TILE_MOVERS);
    int mover = moverAt(pass.cameraPos.xyz + p, listed, true);

    // A see-through unit's: what shows through it instead (above).
    if (mover > 0 && rtMovers[mover - 1].hi.w > 0.5) {
        vec3 dir = normalize(p);
        uint rng = rtSeed(uvec2(px), uint(pass.cameraPos.w));
        float t;
        vec3 hn;
        vec4 hs;
        rtSkipUnits = true;
        bool hit = pass.misc.x > 0.5 && rtClosest(pass.cameraPos.xyz, dir, pass.params.w, rng, t, hn, hs);
        rtSkipUnits = false;
        if (!hit) {
            imageStore(uGbuf, px, vec4(0.0, 0.0, 0.0, 0.0));
            return;
        }
        p = dir * t;
        n = hn;
        mover = moverAt(pass.cameraPos.xyz + p, listed, false);
    }

    imageStore(uGbuf, px, vec4(rtOctEncode(n), length(p), float(sub) + 4.0 * float(mover)));
}
