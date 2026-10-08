#version 450

// A fishing line's points (0x006f8f50), in the world's fog.

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

layout(location = 0) out float vFogFactor;

void main() {
    vec4 viewPos4 = view * vec4(aPos, 1.0);
    gl_Position = projection * viewPos4;
    float dist = -viewPos4.z;
    vFogFactor = pow(clamp((fogParams.y - dist) / max(fogParams.y - fogParams.x, 0.001), 0.0, 1.0),
                     max(fogColor.w, 1.0));
}
