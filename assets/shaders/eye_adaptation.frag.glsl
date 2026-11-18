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
// eye_adaptation.hpp (glareWeight, glareScale, exposeFrame) is the same, and
// what tests/test_eye_adaptation.cpp checks. See eye_adaptation_pass.hpp.

layout(set = 0, binding = 0) uniform sampler2D uFrame;
// r: the clipped share blurred wide, g: the clipped share itself, one texel
// per cell (eye_highlight_mask.comp.glsl, modes 0 and 1).
layout(set = 0, binding = 1) uniform sampler2D uMask;

layout(push_constant) uniform Push {
    float exposure;
    float cell;
} pc;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

// eye_adaptation::kGlareGain, kCoreGain, kIsolatedLow, kIsolatedHigh.
const float kGlareGain = 2.5;
const float kCoreGain = 3.0;
const float kIsolatedLow = 0.02;
const float kIsolatedHigh = 0.15;

// The mask at `at` (mask uv) through a cubic B-spline over its texels, in
// four filtered taps. Read bilinearly, as it was, its slope broke at every
// texel's centre - one cell of pixels apart, 30 at 1001 high - and the
// exposure let go by it, ramped and gained, drew those breaks as a lattice
// of light and dark lines over the glare round the sun. The B-spline is
// smooth through its second derivative there: the glare falls off with no
// grid.
vec2 maskSmooth(vec2 at, vec2 maskSize) {
    vec2 st = at * maskSize - 0.5;
    vec2 i = floor(st);
    vec2 f = st - i;
    vec2 f2 = f * f;
    vec2 f3 = f2 * f;
    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    vec2 w3 = f3 / 6.0;
    vec2 g0 = w0 + w1;
    vec2 g1 = w2 + w3;
    vec2 h0 = (i - 1.0 + w1 / g0 + 0.5) / maskSize;
    vec2 h1 = (i + 1.0 + w3 / g1 + 0.5) / maskSize;
    return g0.y * (g0.x * textureLod(uMask, vec2(h0.x, h0.y), 0.0).rg +
                   g1.x * textureLod(uMask, vec2(h1.x, h0.y), 0.0).rg) +
           g1.y * (g0.x * textureLod(uMask, vec2(h0.x, h1.y), 0.0).rg +
                   g1.x * textureLod(uMask, vec2(h1.x, h1.y), 0.0).rg);
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
        vec2 share = maskSmooth(at, maskSize);
        // The wide glare round a burnt-out sky.
        float wide = share.x;
        float glare = ramp(wide * kGlareGain);
        // A small clipped core on its own - the sun's disc low over a ridge -
        // too small to count in the wide blur: the mask itself, only where
        // the wide one sees next to nothing, so a large burnt-out sky keeps
        // its soft edge.
        float core = ramp(share.y * kCoreGain);
        glare = max(glare, core * (1.0 - smoothstep(kIsolatedLow, kIsolatedHigh, wide)));
        scale = mix(pc.exposure, 1.0, glare);
    }
    outColor = vec4(clamp(c * scale, 0.0, 1.0), 1.0);
}
