#version 450

// A ripple on the liquid, added to it (render state 6 set to 3, 0x0079d5e0).

layout(set = 1, binding = 0) uniform sampler2D rippleTexture;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 tint;
} push;

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 t = texture(rippleTexture, texCoord);
    outColor = vec4(t.rgb, t.a * push.tint.a);
}
