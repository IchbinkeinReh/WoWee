#version 450

// Where the frame is clipped to white, coarsely: one texel per cell of
// `cell` x `cell` pixels of the frame's copy, keeping the share of them that
// are white in all three channels - the sky round a low sun, burnt out in the
// 8-bit frame. The eye adaptation's composite blurs this further and keeps
// the exposure off there (eye_adaptation.frag.glsl, eye_adaptation.hpp).
// A pale yellow cloud or a sunlit field, bright in one or two channels but
// not white in all three, is not clipped and counts for nothing.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D uFrame;
layout(set = 0, binding = 1, rgba16f) uniform writeonly image2D uMask;

layout(push_constant) uniform Push {
    ivec2 frameSize;
    int cell;
} pc;

// eye_adaptation::kClipLow / kClipHigh.
const float kClipLow = 0.96;
const float kClipHigh = 0.995;

void main() {
    ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(texel, imageSize(uMask)))) return;
    ivec2 origin = texel * pc.cell;
    ivec2 end = min(origin + ivec2(pc.cell), pc.frameSize);
    float sum = 0.0;
    float count = 0.0;
    for (int y = origin.y; y < end.y; ++y) {
        for (int x = origin.x; x < end.x; ++x) {
            vec3 c = texelFetch(uFrame, ivec2(x, y), 0).rgb;
            sum += smoothstep(kClipLow, kClipHigh, min(c.r, min(c.g, c.b)));
            count += 1.0;
        }
    }
    imageStore(uMask, texel, vec4(count > 0.0 ? sum / count : 0.0));
}
