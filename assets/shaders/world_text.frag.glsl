#version 450

// The interface font's coverage in the string's colour, laid on by its alpha
// with a world batch's alpha reference of 1 (0x006c53a0). No fog: the batch
// turns it off.

layout(set = 1, binding = 0) uniform sampler2D uFont;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 c = vColor * texture(uFont, vUV);
    if (c.a < 1.0 / 255.0) discard;
    outColor = c;
}
