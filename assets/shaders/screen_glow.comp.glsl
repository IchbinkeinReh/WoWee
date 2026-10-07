#version 450

// The glow's blur, as the client's EffectGlow chain runs it (0x008bfe80):
// FFXBox4 takes the frame down to a quarter a side, then FFXGauss4 blurs the
// quarter twice, across and then down, through a second quarter-size target
// (0x008c1c20; 0x008c15f0 sizes them). Each pass is four bilinear taps:
//
//   FFXBox4    taps at (-1.5,-1.5) (0.5,-1.5) (0.5,0.5) (-1.5,0.5) source
//              texels off the D3D pixel corner, a quarter each - the 4x4
//              block under the quarter-size pixel, averaged.
//   FFXGauss4  taps at -2.5, -0.5, 0.5, 2.5 texels along the pass, weighted
//              1/8, 3/8, 3/8, 1/8 - texels -3..3 as 1,1,3,6,3,1,1 sixteenths.
//
// mode 0 is the box, 1 the horizontal Gauss, 2 the vertical.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D uSource;
layout(set = 0, binding = 1, rgba16f) uniform writeonly image2D uTarget;

layout(push_constant) uniform Push {
    int mode;
} pc;

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(uTarget);
    if (any(greaterThanEqual(pixel, size))) return;

    vec2 srcTexel = 1.0 / vec2(textureSize(uSource, 0));
    vec4 sum;
    if (pc.mode == 0) {
        // The quarter pixel's 4x4 source block: taps on the corners between
        // its 2x2 sub-blocks, each the mean of four texels.
        vec2 base = vec2(pixel) * 4.0;
        sum = 0.25 * (textureLod(uSource, (base + vec2(1.0, 1.0)) * srcTexel, 0.0) +
                      textureLod(uSource, (base + vec2(3.0, 1.0)) * srcTexel, 0.0) +
                      textureLod(uSource, (base + vec2(3.0, 3.0)) * srcTexel, 0.0) +
                      textureLod(uSource, (base + vec2(1.0, 3.0)) * srcTexel, 0.0));
    } else {
        vec2 dir = pc.mode == 1 ? vec2(srcTexel.x, 0.0) : vec2(0.0, srcTexel.y);
        vec2 uv = (vec2(pixel) + 0.5) * srcTexel;
        sum = 0.125 * textureLod(uSource, uv - 2.5 * dir, 0.0) +
              0.375 * textureLod(uSource, uv - 0.5 * dir, 0.0) +
              0.375 * textureLod(uSource, uv + 0.5 * dir, 0.0) +
              0.125 * textureLod(uSource, uv + 2.5 * dir, 0.0);
    }
    imageStore(uTarget, pixel, vec4(sum.rgb, 1.0));
}
