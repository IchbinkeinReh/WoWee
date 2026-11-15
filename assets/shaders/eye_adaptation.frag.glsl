#version 450

// The eye's exposure over the finished frame, ahead of the interface, read
// from a copy of it. Not a plain multiply everywhere: the frame is 8-bit, and
// the sky round a low sun is already clipped to white in it - white times an
// exposure under one was a flat grey disc. Where the frame is clipped white
// (eye_highlight_mask.comp.glsl, one texel per cell of pixels), blurred
// wide, the exposure is let go toward one, so the burnt-out sky stays white
// and the glare round it falls off softly into the exposed sky. The colour
// is only ever scaled, so a pale cloud keeps its hue; away from any clipped
// white - a noon field, an inn - it is exactly the old multiply.
// eye_adaptation.hpp (glareWeight, glareScale) is the same, and what
// tests/test_eye_adaptation.cpp checks. See eye_adaptation_pass.hpp.

layout(set = 0, binding = 0) uniform sampler2D uFrame;
layout(set = 0, binding = 1) uniform sampler2D uMask;

layout(push_constant) uniform Push {
    float exposure;
    float cell;
} pc;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

// eye_adaptation::kGlareGain, kGlareSpread, kCoreGain, kCoreSpread,
// kIsolatedLow, kIsolatedHigh.
const float kGlareGain = 2.5;
const float kGlareSpread = 2.0;
const float kCoreGain = 3.0;
const float kCoreSpread = 0.5;
const float kIsolatedLow = 0.02;
const float kIsolatedHigh = 0.15;

// The share of clipped white round `at` (mask uv): a 5 x 5 Gaussian over
// the mask, its taps `spread` mask texels apart, each filtered.
float clippedShare(vec2 at, vec2 maskSize, float spread) {
    vec2 stride = spread / maskSize;
    float sum = 0.0;
    float weights = 0.0;
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            float w = exp(-float(dx * dx + dy * dy) / 4.5);
            sum += w * textureLod(uMask, at + vec2(dx, dy) * stride, 0.0).r;
            weights += w;
        }
    }
    return sum / weights;
}

float ramp(float x) {
    float t = clamp(x, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec3 c = texelFetch(uFrame, pixel, 0).rgb;
    float scale = pc.exposure;
    if (pc.exposure < 1.0) {
        vec2 maskSize = vec2(textureSize(uMask, 0));
        vec2 at = gl_FragCoord.xy / pc.cell / maskSize;
        // The wide glare round a burnt-out sky.
        float wide = clippedShare(at, maskSize, kGlareSpread);
        float glare = ramp(wide * kGlareGain);
        // A small clipped core on its own - the sun's disc low over a ridge -
        // too small to count in the wide blur: a tighter one, only where
        // the wide one sees next to nothing, so a large burnt-out sky keeps
        // its soft edge.
        float core = ramp(clippedShare(at, maskSize, kCoreSpread) * kCoreGain);
        glare = max(glare, core * (1.0 - smoothstep(kIsolatedLow, kIsolatedHigh, wide)));
        scale = mix(pc.exposure, 1.0, glare);
    }
    outColor = vec4(clamp(c * scale, 0.0, 1.0), 1.0);
}
