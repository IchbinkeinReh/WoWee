#version 450

// The circle on the ground while a spell waits for a place (0x004f8a40),
// drawn over the ground's triangles through blob_shadow.vert. White:
// Spell-Shadow-Acceptable or -Unacceptable as it is, its alpha by "ShadowAdd"
// (0x007e2c60), the height fade over the box's four yards - in over the
// lowest sixth and out over the highest. Blend mode 2 lays it on by its
// alpha, with that mode's alpha ref of 1 (0x00ad8b7c).
// spell_target_circle::shade in rendering/spell_target_circle.hpp is the same.

layout(set = 1, binding = 0) uniform sampler2D uCircle;

layout(location = 0) in vec2 vCircleUV;
layout(location = 1) in float vHeight;
layout(location = 0) out vec4 outColor;

float fadeTexel(float i) {
    float s = (i / 63.0) * 12.0;
    float f = s < 2.0 ? s * 0.5 : (s < 10.0 ? 1.0 : max((12.0 - s) * 0.5, 0.0));
    return floor(f * 255.0 + 0.5) / 255.0;
}

// The 64-texel row, linearly filtered and clamped at its edges.
float fadeAt(float u) {
    float x = u * 64.0 - 0.5;
    float i0 = floor(x);
    float t = x - i0;
    float a = fadeTexel(clamp(i0, 0.0, 63.0));
    float b = fadeTexel(clamp(i0 + 1.0, 0.0, 63.0));
    return mix(a, b, t);
}

void main() {
    vec4 texel = texture(uCircle, vCircleUV);
    float alpha = texel.a * fadeAt(vHeight);
    if (alpha < 1.0 / 255.0) discard;
    outColor = vec4(texel.rgb, alpha);
}
