#version 450

// ffxSpecial's fog, one frame of it (0x007e92a0 and 0x007e9080 on the
// 256x128 target 0x007ea5f0 makes): a row of the seed noise is laid three
// rows up from the bottom, in the row's colour, then every texel takes the
// mean of the four below it - (-1, +1), (0, +1), (+1, +1), (0, +2) - less
// the decay from its alpha (FFXPropagateFog.bls). The fog creeps up the
// target a row a frame and fades as it goes; FFXFogCombine wraps the target
// round the screen, its bottom at the corners and its top at the middle.
//
// Read from last frame's target, written to the other.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, rgba8) uniform readonly image2D uPrev;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D uOut;
layout(set = 0, binding = 2) uniform sampler2D uNoise;

layout(push_constant) uniform Push {
    vec4 colour;    // the seed's material colour (param 0)
    float decay;    // param 1 / 255
    int noiseRow;   // the noise row laid this frame
    int seedRow;    // the target row it is laid on
    float pad;
} pc;

vec4 source(ivec2 p, ivec2 size) {
    p = clamp(p, ivec2(0), size - 1);
    if (p.y == pc.seedRow) {
        float a = texelFetch(uNoise, ivec2(p.x % textureSize(uNoise, 0).x, pc.noiseRow), 0).a;
        return vec4(pc.colour.rgb, a);
    }
    return imageLoad(uPrev, p);
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(uOut);
    if (any(greaterThanEqual(pixel, size))) return;

    vec4 sum = source(pixel + ivec2(-1, 1), size) + source(pixel + ivec2(0, 1), size) +
               source(pixel + ivec2(1, 1), size) + source(pixel + ivec2(0, 2), size);
    vec4 r = sum * 0.25;
    r.a -= pc.decay;
    imageStore(uOut, pixel, clamp(r, 0.0, 1.0));
}
