#version 450
// One edge-aware a-trous pass (Dammertz et al. 2010) over the accumulated
// lighting: a 5x5 B3-spline kernel spread `step` pixels apart, taps weighted
// down across depth and normal edges. Run three times with steps 1, 2, 4.
//
// Pixels with a long history are already smooth and are filtered less, so
// settled shadows keep their edges. The three terms are sun, occlusion and
// point light visibility; alpha carries the history length from pass to
// pass, and the last pass writes the distance there instead, which the
// surface shaders check the result against - negated where the surface is a
// unit's (rtGbufUnit), which rtLightAt treats as gone a frame on.

#include "rt_common.glsli"

layout(set = 0, binding = 3) uniform sampler2D uGbufCur;
layout(set = 0, binding = 9) uniform sampler2D uIn;
layout(set = 0, binding = 10, rgba16f) uniform writeonly image2D uOut;

layout(push_constant) uniform Push {
    int step;
    int last;
} pc;

layout(local_size_x = 8, local_size_y = 8) in;

const float KERNEL[3] = float[3](3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0);

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(px, ivec2(pass.extent.xy)))) return;

    vec4 g = texelFetch(uGbufCur, px, 0);
    vec4 c = texelFetch(uIn, px, 0);
    float dist = rtGbufDist(g);
    vec3 n = rtGbufNormal(g);
    if (dist <= 0.0) {
        imageStore(uOut, px, vec4(c.rgb, pc.last != 0 ? 0.0 : c.a));
        return;
    }

    float history = c.a;
    float spread = mix(1.0, 0.35, clamp(history / 24.0, 0.0, 1.0));

    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            ivec2 q = px + ivec2(x, y) * pc.step;
            if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, ivec2(pass.extent.xy)))) continue;
            vec4 qg = texelFetch(uGbufCur, q, 0);
            float qd = rtGbufDist(qg);
            if (qd <= 0.0) continue;
            float k = KERNEL[abs(x)] * KERNEL[abs(y)];
            float wz = exp(-abs(qd - dist) / (0.02 * dist * float(pc.step) + 1e-3));
            float wn = pow(max(dot(rtGbufNormal(qg), n), 0.0), 32.0);
            float w = k * wz * wn;
            if (x != 0 || y != 0) w *= spread;
            sum += texelFetch(uIn, q, 0).rgb * w;
            wsum += w;
        }
    }
    sum /= wsum;
    imageStore(uOut, px, vec4(sum, pc.last != 0 ? (rtGbufUnit(g) ? -dist : dist) : history));
}
