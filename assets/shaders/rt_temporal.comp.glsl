#version 450
// Accumulate this frame's samples into last frame's, reprojected.
//
// A pixel's history is found by projecting its world position with last
// frame's matrices and is kept only when last frame saw the same surface
// there - same distance within a few percent, similar normal. The blend
// weight is 1 / history length, capped, so a newly revealed pixel converges
// in a handful of frames and a settled one averages dozens.
//
// Three terms ride along - sun, occlusion and point light visibility - and
// the history length is the fourth channel.
//
// What moves on its own - a unit, what it carries, a door, a lift - is a
// mover (rt_movers.glsli): its texels look for their history where its motion
// since last frame puts them, and only among last frame's movers' texels, so
// a body never takes the ground's history nor the ground the body's. Its pose
// does not follow that rigid motion - an arm swings, a lid opens - so the
// mover's own history is kept short.
//
// A mover's shadow is another matter: it is dragged over ground whose history
// still holds the shadow where it was, and the test passes there - same
// ground, same distance. So the history is also held to what this frame's
// samples around the pixel allow (a variance clamp over the same surface),
// and a history clamped hard is shortened with it, so a shadow that moved off
// a patch of ground leaves in a few frames instead of fading out over two
// dozen behind whatever cast it.

#include "rt_common.glsli"
#include "rt_movers.glsli"

layout(set = 0, binding = 3) uniform sampler2D uGbufCur;
layout(set = 0, binding = 5) uniform sampler2D uGbufPrev;
layout(set = 0, binding = 6) uniform sampler2D uRaw;
layout(set = 0, binding = 7) uniform sampler2D uHistPrev;
layout(set = 0, binding = 8, rgba16f) uniform writeonly image2D uHist;

layout(local_size_x = 8, local_size_y = 8) in;

const float MAX_HISTORY = 24.0;
const float UNIT_HISTORY = 2.0;

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(px, ivec2(pass.extent.xy)))) return;

    vec4 g = texelFetch(uGbufCur, px, 0);
    vec4 raw = texelFetch(uRaw, px, 0);
    float dist = rtGbufDist(g);
    vec3 n = rtGbufNormal(g);
    if (dist <= 0.0) {
        imageStore(uHist, px, vec4(raw.rgb, 1.0));
        return;
    }

    vec3 p = rtGbufWorld(g, px);
    int mover = rtGbufMover(g);
    // Where this surface was when last frame's history was made.
    vec3 pp = p;
    vec3 pn = n;
    if (mover > 0) {
        pp = rtMoverPrev(mover - 1, p);
        pn = normalize(rtMoverPrevDir(mover - 1, n));
    }

    float len = 0.0;
    vec4 hist = vec4(0.0);
    vec4 clip = pass.prevViewProj * vec4(pp, 1.0);
    if (pass.misc.y > 0.5 && clip.w > 0.0) {
        vec2 puv = clip.xy / clip.w * 0.5 + 0.5;
        if (all(greaterThanEqual(puv, vec2(0.0))) && all(lessThan(puv, vec2(1.0)))) {
            // Bilinear by hand, dropping the taps that were another surface.
            vec2 f = puv * pass.extent.xy - 0.5;
            ivec2 base = ivec2(floor(f));
            vec2 w = fract(f);
            float expected = length(pp - pass.prevCameraPos.xyz);
            float wsum = 0.0;
            for (int i = 0; i < 4; ++i) {
                ivec2 o = ivec2(i & 1, i >> 1);
                ivec2 q = clamp(base + o, ivec2(0), ivec2(pass.extent.xy) - 1);
                vec4 pg = texelFetch(uGbufPrev, q, 0);
                float pd = rtGbufDist(pg);
                bool same = pd > 0.0 && abs(pd - expected) < 0.05 * expected + 0.1 &&
                            dot(rtGbufNormal(pg), pn) > 0.8 && (rtGbufMover(pg) > 0) == (mover > 0);
                float bw = (o.x == 1 ? w.x : 1.0 - w.x) * (o.y == 1 ? w.y : 1.0 - w.y);
                if (same && bw > 0.0) {
                    hist += texelFetch(uHistPrev, q, 0) * bw;
                    wsum += bw;
                }
            }
            if (wsum > 0.05) {
                hist /= wsum;
                len = hist.a;
            }
        }
    }

    if (len > 0.0) {
        // This frame's samples over a 5x5 block (every other texel), on this
        // surface: their mean and spread. One sample a pixel is noisy - a
        // half-lit edge is a mix of 0s and 1s - so the window is the mean
        // plus one and a half standard deviations and a fifth on either side.
        vec3 m1 = vec3(0.0), m2 = vec3(0.0);
        float count = 0.0;
        for (int y = -2; y <= 2; y += 2) {
            for (int x = -2; x <= 2; x += 2) {
                ivec2 q = clamp(px + ivec2(x, y), ivec2(0), ivec2(pass.extent.xy) - 1);
                vec4 qg = texelFetch(uGbufCur, q, 0);
                float qd = rtGbufDist(qg);
                bool same = (x == 0 && y == 0) ||
                            (qd > 0.0 && abs(qd - dist) < 0.05 * dist + 0.1 && dot(rtGbufNormal(qg), n) > 0.8 &&
                             rtGbufMover(qg) == mover);
                if (!same) continue;
                vec3 v = texelFetch(uRaw, q, 0).rgb;
                m1 += v;
                m2 += v * v;
                count += 1.0;
            }
        }
        vec3 mean = m1 / count;
        vec3 sigma = sqrt(max(m2 / count - mean * mean, vec3(0.0)));
        vec3 window = 1.5 * sigma + 0.2;
        vec3 held = clamp(hist.rgb, mean - window, mean + window);
        vec3 moved = abs(held - hist.rgb);
        len *= clamp(1.0 - 3.0 * max(moved.r, max(moved.g, moved.b)), 0.1, 1.0);
        hist.rgb = held;
    }

    // A unit's shadow is here this frame: units move, and a long history
    // would keep where it was for many frames after - a ghost of a running
    // body's legs trailing it. Kept short while it is here, the history lets
    // go of it within a few frames once it has moved on.
    if (raw.a > 0.5) len = min(len, UNIT_HISTORY);
    if (mover > 0) len = min(len, rtMovers[mover - 1].lo.w);
    len = min(len + 1.0, MAX_HISTORY);
    float a = 1.0 / len;
    vec3 lightOut = mix(hist.rgb, raw.rgb, a);
    imageStore(uHist, px, vec4(lightOut, len));
}
