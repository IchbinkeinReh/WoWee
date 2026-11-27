#version 450

// The scene's depth brought down to the volumetric light's own size, as the
// nearest of the pixels each of its texels covers. See volumetric_light.hpp.
//
// The nearest, so a ray stops at the first thing in front of it: a lamp's
// glow is not carried over the edge of a pillar standing before it. Kept as
// the reversed depth it is (the far plane, where the sky is, is 0).
//
// Built with VL_MSAA for a multisampled depth that was not resolved; its
// first sample stands for the pixel.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#ifdef VL_MSAA
layout(set = 1, binding = 2) uniform sampler2DMS uDepth;
#else
layout(set = 1, binding = 2) uniform sampler2D uDepth;
#endif
layout(set = 1, binding = 3, r32f) uniform writeonly image2D uLowDepth;

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(uLowDepth);
    if (any(greaterThanEqual(pixel, size))) return;

#ifdef VL_MSAA
    ivec2 depthSize = textureSize(uDepth);
#else
    ivec2 depthSize = textureSize(uDepth, 0);
#endif
    // The depth can be smaller than the frame (an upscaler renders the scene
    // smaller): the footprint is found by place on screen, not by pixel.
    float nearest = 0.0;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            vec2 uv = (vec2(pixel) + (vec2(i, j) + 0.5) * 0.5) / vec2(size);
            ivec2 p = clamp(ivec2(uv * vec2(depthSize)), ivec2(0), depthSize - 1);
            // Reversed: larger is nearer. (For a multisampled depth the last
            // argument is the sample.)
            nearest = max(nearest, texelFetch(uDepth, p, 0).r);
        }
    }
    imageStore(uLowDepth, pixel, vec4(nearest));
}
