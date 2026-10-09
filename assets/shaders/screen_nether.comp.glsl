#version 450

// ffxNetherWorld's blur (0x007e9b10, FFXNetherBlur.bls vertex and pixel
// programs): each pixel the mean of four taps, at 0, 1, 2 and 3 steps along
// a direction; run twice, the frame into a quarter-size target and that into
// another. The step is 8 of the frame's texels, in uv, on both axes (the
// program's c1.x).
//
// The direction comes from a 6x6 grid laid over the target, row 0 at the
// top: at each point the angle 3 x value + angle (the camera's, and the
// phase's), turned into (cos, sin) by the vertex program and interpolated
// across the grid's quads.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D uSource;
layout(set = 0, binding = 1, rgba16f) uniform writeonly image2D uTarget;

layout(push_constant) uniform Push {
    vec2 step;     // uv per step
    float angle;   // program.local[0].x
    float pad;
    uint packed[18];  // the 36 grid values, two halves a word
} pc;

float gridValue(int i) {
    vec2 two = unpackHalf2x16(pc.packed[i >> 1]);
    return (i & 1) == 0 ? two.x : two.y;
}

vec2 gridDir(int col, int row) {
    float a = 3.0 * gridValue(row * 6 + col) + pc.angle;
    return vec2(cos(a), sin(a));
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(uTarget);
    if (any(greaterThanEqual(pixel, size))) return;

    vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
    vec2 g = clamp(uv * 5.0, vec2(0.0), vec2(5.0));
    ivec2 c0 = min(ivec2(floor(g)), ivec2(4));
    vec2 f = g - vec2(c0);
    vec2 d = mix(mix(gridDir(c0.x, c0.y), gridDir(c0.x + 1, c0.y), f.x),
                 mix(gridDir(c0.x, c0.y + 1), gridDir(c0.x + 1, c0.y + 1), f.x), f.y);
    vec2 o = d * pc.step;

    vec4 sum = textureLod(uSource, uv, 0.0) + textureLod(uSource, uv + o, 0.0) +
               textureLod(uSource, uv + 2.0 * o, 0.0) + textureLod(uSource, uv + 3.0 * o, 0.0);
    imageStore(uTarget, pixel, vec4(sum.rgb * 0.25, 1.0));
}
