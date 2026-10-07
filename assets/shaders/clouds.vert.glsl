#version 450

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

// The client's cloud dome (Wow.exe 3.3.5a 0x007f20e0): a vertex is the point
// on the sky sphere, already relative to the eye, with its place on the
// cloud texture and its row's alpha.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aUvAlpha;  // xy = cloud texture uv, z = vertex alpha

layout(location = 0) out vec3 vWorldDir;
layout(location = 1) out vec2 vUV;
layout(location = 2) out float vAlpha;

void main() {
    vWorldDir = aPos;
    vUV = aUvAlpha.xy;
    vAlpha = aUvAlpha.z;
    mat4 rotView = mat4(mat3(view));
    vec4 pos = projection * rotView * vec4(aPos, 1.0);
    gl_Position = pos.xyww;
}
