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
// Everything here is this frame's: the G-buffer the texel stands for was
// drawn this frame, and the history is found where the camera's motion since
// last frame puts it - the standard motion vector of a surface that stands
// still. What moves on its own - a unit, a door, a lift - finds there what
// stood there a frame ago, which the test above rejects where it is another
// surface and the clamp below holds to this frame's samples where it is not.
//
// A moving caster's shadow is dragged over ground whose history still holds
// it where it was, and the test passes there - same ground, same distance. So
// the history is also held to what this frame's samples around the pixel
// allow (a variance clamp over the same surface), and a history clamped hard
// is shortened with it, so a shadow that moved off a patch of ground leaves in
// a few frames instead of fading out over two dozen behind whatever cast it.

#include "rt_common.glsli"

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
    bool valid = rtGbufValid(g);

    float len = 0.0;
    vec4 hist = vec4(0.0);
    vec4 clip = pass.prevViewProj * vec4(p, 1.0);
    if (pass.misc.y > 0.5 && clip.w > 0.0) {
        vec2 puv = clip.xy / clip.w * 0.5 + 0.5;
        if (all(greaterThanEqual(puv, vec2(0.0))) && all(lessThan(puv, vec2(1.0)))) {
            // Last frame's texels of this surface among the 3x3 around the
            // reprojected point, each by how near the pixel it was traced
            // from lies to it (rtHistoryWeight): a still camera finds mostly
            // its own texel's history, and where an edge moved a pixel and the
            // nearest texels are another surface now, one of this surface a
            // texel on still carries it.
            vec2 reprojected = puv * pass.sceneExtent.xy;
            float bs = float(rtBlockSize());
            ivec2 center = ivec2(floor(reprojected / bs));
            float expected = length(p - pass.prevCameraPos.xyz);
            float wsum = 0.0;
            for (int y = -1; y <= 1; ++y) {
                for (int x = -1; x <= 1; ++x) {
                    ivec2 q = center + ivec2(x, y);
                    if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, ivec2(pass.extent.xy)))) continue;
                    vec4 pg = texelFetch(uGbufPrev, q, 0);
                    float pd = rtGbufDist(pg);
                    bool same = pd > 0.0 && abs(pd - expected) < 0.05 * expected + 0.1 &&
                                dot(rtGbufNormal(pg), n) > 0.8 && rtGbufValid(pg) == valid;
                    if (!same) continue;
                    float hw = rtHistoryWeight(vec2(rtGbufPixel(pg, q)) + 0.5, reprojected, bs);
                    hist += texelFetch(uHistPrev, q, 0) * hw;
                    wsum += hw;
                }
            }
            if (wsum > 0.0) {
                hist /= wsum;
                len = hist.a;
            }
        }
    }

    if (len > 0.0) {
        // This frame's samples over a 5x5 block (every other texel, and the
        // four next to it), on this surface: their mean and spread. One
        // sample a pixel is noisy - a half-lit edge is a mix of 0s and 1s -
        // so the window is the mean plus one and a half standard deviations
        // and a fifth on either side, and wider where there are few samples
        // of this surface: at an edge, on a pole, the spread of one or two
        // says nothing, and the history was held to one frame's noise
        // (rtClampWindow).
        vec3 m1 = vec3(0.0), m2 = vec3(0.0);
        float count = 0.0;
        const ivec2 taps[13] = ivec2[13](
            ivec2(0, 0), ivec2(-2, -2), ivec2(0, -2), ivec2(2, -2), ivec2(-2, 0), ivec2(2, 0),
            ivec2(-2, 2), ivec2(0, 2), ivec2(2, 2), ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));
        for (int i = 0; i < 13; ++i) {
            ivec2 q = px + taps[i];
            if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, ivec2(pass.extent.xy)))) continue;
            vec4 qg = texelFetch(uGbufCur, q, 0);
            float qd = rtGbufDist(qg);
            bool same = i == 0 ||
                        (qd > 0.0 && abs(qd - dist) < 0.05 * dist + 0.1 && dot(rtGbufNormal(qg), n) > 0.8 &&
                         rtGbufValid(qg) == valid);
            if (!same) continue;
            vec3 v = texelFetch(uRaw, q, 0).rgb;
            m1 += v;
            m2 += v * v;
            count += 1.0;
        }
        vec3 mean = m1 / count;
        vec3 sigma = sqrt(max(m2 / count - mean * mean, vec3(0.0)));
        vec3 window = rtClampWindow(sigma, count);
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
    len = min(len + 1.0, MAX_HISTORY);
    float a = 1.0 / len;
    vec3 lightOut = mix(hist.rgb, raw.rgb, a);
    imageStore(uHist, px, vec4(lightOut, len));
}
