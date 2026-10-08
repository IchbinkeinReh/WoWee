#version 450

// One colour for the whole line: the pole's ambient light, opaque.

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

layout(push_constant) uniform Line {
    vec4 colour;
} line;

layout(location = 0) in float vFogFactor;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(mix(fogColor.rgb, line.colour.rgb, vFogFactor), line.colour.a);
}
