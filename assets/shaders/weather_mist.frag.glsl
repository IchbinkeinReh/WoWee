#version 450

// SnowMist01 or WeatherMistGrainy01 by the corner's colour, alpha blended
// with that mode's 1/255 alpha reference (0x00ad8b7c).

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 c = texture(uTexture, vUV) * vColor;
    if (c.a < 1.0 / 255.0) discard;
    outColor = c;
}
