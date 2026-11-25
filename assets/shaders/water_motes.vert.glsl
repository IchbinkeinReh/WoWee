#version 450

// A corner of what floats in the water (0x0079ca70).

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

layout(location = 0) out vec2 vUV;
layout(location = 1) out float vDepth;

void main() {
    vUV = aUV;
    vec4 inView = view * vec4(aPos, 1.0);
    vDepth = -inView.z;
    gl_Position = projection * inView;
}
