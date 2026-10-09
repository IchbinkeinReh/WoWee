#version 450

// The circle under the target (0x00725980, 0x00744eb0), drawn over the
// ground's triangles through blob_shadow.vert. Every stage modulates:
// Textures\UnitSelectTexture.blp by the circle's colour, and the alpha by
// "ShadowAdd" (0x007e36e0), the height fade over the box's 64 texels -
// white, its alpha in over the lowest sixth and out over the highest. Blend
// mode 3 adds the result on by its alpha, with the alpha ref 1 of that mode
// (0x00ad8b7c). selection_circle::shade in rendering/selection_circle.hpp is
// the same.

layout(set = 1, binding = 0) uniform sampler2D uCircle;

layout(push_constant) uniform Push {
    vec4 uRow;
    vec4 vRow;
    vec4 hRow;
    vec4 color;  // the selection colour (0x00521bf0)
} push;

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
    float alpha = texel.a * push.color.a * fadeAt(vHeight);
    if (alpha < 1.0 / 255.0) discard;
    outColor = vec4(texel.rgb * push.color.rgb, alpha);
}
