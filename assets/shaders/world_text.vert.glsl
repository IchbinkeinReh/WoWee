#version 450

// Text standing in the world (0x007e5640): each corner is its anchor and an
// offset in yards in the plane that faces the camera through it, so the
// whole string lies at the anchor's depth and is tested against the scene's.

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrix;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 viewPos;
    vec4 fogColor;
    vec4 fogParams;
    vec4 shadowParams;
};

layout(location = 0) in vec3 aAnchor;
layout(location = 1) in vec2 aOffset;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main() {
    vec4 p = view * vec4(aAnchor, 1.0);
    p.xy += aOffset;
    gl_Position = projection * p;
    vUV = aUV;
    vColor = aColor;
}
