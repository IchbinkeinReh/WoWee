#version 450

// A weather mist sheet's corner (0x00786e10 builds them on the CPU, four a
// sheet facing the camera, each with its own alpha).

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

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in float aAlpha;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main() {
    vUV = aUV;
    // The fog's colour (the light's +0x8c) with the corner's alpha.
    vColor = vec4(fogColor.rgb, aAlpha);
    gl_Position = projection * view * vec4(aPos, 1.0);
}
