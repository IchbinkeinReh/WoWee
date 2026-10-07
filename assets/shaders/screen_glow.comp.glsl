#version 450

// The glow's blur, over a quarter-size copy of the finished frame. The client
// blurs its own down-sampled copy with FFXGauss4 and FFXBox4 before PassGlow
// adds it back (Wow.exe 3.3.5a, ffxGlow); a 9x9 Gaussian on the quarter copy
// stands in for those two. See screen_effects.hpp.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D uFrame;
layout(set = 0, binding = 1, rgba16f) uniform writeonly image2D uGlow;

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(uGlow);
    if (any(greaterThanEqual(pixel, size))) return;

    vec2 texel = 1.0 / vec2(size);
    vec2 uv = (vec2(pixel) + 0.5) * texel;
    const float w[5] = float[](0.2270270, 0.1945946, 0.1216216, 0.0540540, 0.0162162);
    vec3 sum = vec3(0.0);
    for (int y = -4; y <= 4; ++y) {
        for (int x = -4; x <= 4; ++x) {
            sum += textureLod(uFrame, uv + vec2(x, y) * texel, 0.0).rgb * (w[abs(x)] * w[abs(y)]);
        }
    }
    imageStore(uGlow, pixel, vec4(sum, 1.0));
}
