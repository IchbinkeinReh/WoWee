#version 450

// The horizon in the fog through and through: fog from 0 to 1 yard, in the
// zone's fog colour (0x007d5e70, render states 8, 9, 10, 12), so a tile is a
// silhouette of the fog against the sky.

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

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(fogColor.rgb, 1.0);
}
