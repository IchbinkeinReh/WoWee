#version 450

// Evens out the march's fixed sample pattern (volumetric_light.comp.glsl): a
// 5x5 blur over the march's image, each neighbour weighed by how near its
// depth lies to this texel's, so a lamp's glow is not smeared across the
// edge of a wall in front of it. Within this frame only. See
// volumetric_light.hpp.
//
// The depth is the reversed one, which goes as one over the distance: the
// difference of two over the larger is how far apart they are in proportion,
// whatever the distance.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 1, binding = 3, r32f) uniform readonly image2D uLowDepth;
layout(set = 1, binding = 4, rgba16f) uniform readonly image2D uMarch;
layout(set = 1, binding = 5, rgba16f) uniform writeonly image2D uBlurred;

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(uBlurred);
    if (any(greaterThanEqual(pixel, size))) return;

    float d0 = imageLoad(uLowDepth, pixel).r;
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            ivec2 q = clamp(pixel + ivec2(i, j), ivec2(0), size - 1);
            float d = imageLoad(uLowDepth, q).r;
            float apart = abs(d - d0) / max(max(d, d0), 1e-7);
            float w = exp(-0.5 * float(i * i + j * j) / 2.25) * exp(-apart * 12.0);
            sum += imageLoad(uMarch, q).rgb * w;
            weight += w;
        }
    }
    imageStore(uBlurred, pixel, vec4(sum / max(weight, 1e-6), 1.0));
}
