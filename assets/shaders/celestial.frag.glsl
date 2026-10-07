#version 450

// The sun, the moons and their glare: the texture in the push colour. Cut at
// the horizon and faded in over the 0.4 units above it, as 0x007edee0 clips
// the quad and sets its vertex alpha to 2.5 x the height.
layout(push_constant) uniform Push {
    vec4 dirSize;
    vec4 color;
} push;

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(location = 0) in vec2 TexCoord;
layout(location = 1) in float vHeight;

layout(location = 0) out vec4 outColor;

void main() {
    if (vHeight < 0.0) discard;
    vec4 tex = texture(uTexture, TexCoord);
    float alpha = tex.a * push.color.a * clamp(vHeight * 2.5, 0.0, 1.0);
    if (alpha < 0.002) discard;
    outColor = vec4(tex.rgb * push.color.rgb, alpha);
}
