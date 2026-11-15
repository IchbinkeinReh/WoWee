#version 450

// The eye's exposure over the finished frame, ahead of the interface, read
// from a copy of it. Not a plain multiply: the frame is 8-bit, the sky round
// a low sun already clipped to white, and white times an exposure under one
// was a flat grey disc. The dimmest channel - how white the pixel is - is
// taken back through a shoulder to the light it stands for, exposed there,
// and brought back down by the same shoulder, the colour scaled with it:
// exactly value x exposure under the knee, a clipped white still white.
// eye_adaptation.hpp (exposeColour) is the same, and
// what tests/test_eye_adaptation.cpp checks. See eye_adaptation_pass.hpp.

layout(set = 0, binding = 0) uniform sampler2D uFrame;

layout(push_constant) uniform Push {
    float exposure;
} pc;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

const float kKnee = 0.7;
const float kWhite = 8.0;
const float kA = (1.0 - kKnee) / (kWhite - 1.0);

float toScene(float v) {
    if (v <= kKnee) return max(v, 0.0);
    float s = min((v - kKnee) / (1.0 - kKnee), 1.0);
    float x = s * kA / (1.0 + kA - s);
    return kKnee + x * (kWhite - kKnee);
}

float toScreen(float h) {
    if (h <= kKnee) return max(h, 0.0);
    float x = (h - kKnee) / (kWhite - kKnee);
    float s = min(x * (1.0 + kA) / (x + kA), 1.0);
    return kKnee + s * (1.0 - kKnee);
}

void main() {
    vec3 c = texelFetch(uFrame, ivec2(gl_FragCoord.xy), 0).rgb;
    // The whiteness: the clipped sky round the sun is white in all three
    // channels; a sunlit field or a noon sky, bright in one or two, keeps
    // the plain multiply.
    float w = min(c.r, min(c.g, c.b));
    float scale = w > 1e-4 ? toScreen(pc.exposure * toScene(w)) / w : pc.exposure;
    outColor = vec4(clamp(c * scale, 0.0, 1.0), 1.0);
}
