#version 450

// What the ground is multiplied by under a unit (Mod blend, 0x007e4480).
// Texture 0, ShadowBlob.blp, with colour op Fade: the texture over white by
// the model's alpha. Texture 1, "ShadowMod" (0x007e3820), added: grey 1 - fade
// over its 64 texels, fading in over the box's lowest sixth and out over its
// highest. blob_shadow::modulation in rendering/blob_shadow.hpp is the same.

layout(set = 1, binding = 0) uniform sampler2D uBlob;

layout(push_constant) uniform Push {
    vec4 uRow;
    vec4 vRow;
    vec4 hRow;
    vec4 params;
} push;

layout(location = 0) in vec2 vBlobUV;
layout(location = 1) in float vHeight;
layout(location = 0) out vec4 outColor;

float modTexel(float i) {
    float s = (i / 63.0) * 12.0;
    float f = s < 2.0 ? s * 0.5 : (s < 10.0 ? 1.0 : max((12.0 - s) * 0.5, 0.0));
    return floor((1.0 - f) * 255.0 + 0.5) / 255.0;
}

// The 64-texel row, linearly filtered and clamped at its edges.
float modAt(float u) {
    float x = u * 64.0 - 0.5;
    float i0 = floor(x);
    float t = x - i0;
    float a = modTexel(clamp(i0, 0.0, 63.0));
    float b = modTexel(clamp(i0 + 1.0, 0.0, 63.0));
    return mix(a, b, t);
}

void main() {
    vec3 blob = texture(uBlob, vBlobUV).rgb;
    vec3 faded = mix(vec3(1.0), blob, clamp(push.params.x, 0.0, 1.0));
    outColor = vec4(clamp(faded + vec3(modAt(vHeight)), 0.0, 1.0), 1.0);
}
