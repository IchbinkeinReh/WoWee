#version 450

// What the sun shafts stream out of: the quarter-size copy of the frame, kept
// only where the scene's depth says there is sky. See sun_shafts.hpp.
//
// Everything the world draws writes depth - the terrain, the buildings, the
// trees, the WDL horizon behind them - and the sky, its clouds, the sun and
// the moon write none, so a texel with nothing in front of the far plane is
// sky. One quarter-size texel covers four by four pixels of the frame; it
// keeps the share of them that are sky, so a ridge against the sun gets a
// soft edge and not a stair of quarter-size blocks. Geometry keeps nothing:
// a sunlit wall, as white as the sky beside it, casts no rays.
//
// Built with SHAFTS_MSAA for a multisampled depth that was not resolved; its
// first sample stands for the pixel.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D uFrame;
#ifdef SHAFTS_MSAA
layout(set = 0, binding = 1) uniform sampler2DMS uDepth;
#else
layout(set = 0, binding = 1) uniform sampler2D uDepth;
#endif
layout(set = 0, binding = 2, rgba16f) uniform writeonly image2D uSource;

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(uSource);
    if (any(greaterThanEqual(pixel, size))) return;

#ifdef SHAFTS_MSAA
    ivec2 depthSize = textureSize(uDepth);
#else
    ivec2 depthSize = textureSize(uDepth, 0);
#endif
    // The depth can be smaller than the frame (an upscaler renders the scene
    // smaller): the texel's footprint is found by place on screen, not by
    // pixel.
    float sky = 0.0;
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            vec2 uv = (vec2(pixel) + (vec2(i, j) + 0.5) * 0.25) / vec2(size);
            ivec2 p = min(ivec2(uv * vec2(depthSize)), depthSize - 1);
            // Reversed depth: the far plane, where the sky is, is 0.
            // (For a multisampled depth the last argument is the sample.)
            sky += texelFetch(uDepth, p, 0).r <= 0.0 ? 1.0 : 0.0;
        }
    }
    sky *= 1.0 / 16.0;

    vec3 c = texelFetch(uFrame, pixel, 0).rgb;
    imageStore(uSource, pixel, vec4(c * sky, sky));
}
