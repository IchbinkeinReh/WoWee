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

// One of the client's sky sprites (Wow.exe 3.3.5a 0x007edbe0, drawn by
// 0x009ac660): a quad facing the camera, push.dirSize.w units across, twelve
// units from the eye toward the body.
layout(push_constant) uniform Push {
    vec4 dirSize;  // xyz = toward the body, w = size
    vec4 color;
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec2 TexCoord;
layout(location = 1) out float vHeight;  // above the eye, in the client's units

void main() {
    TexCoord = aTexCoord;
    mat3 rot = mat3(view);
    vec3 viewPos = rot * (push.dirSize.xyz * 12.0) + vec3(aPos.xy * push.dirSize.w, 0.0);
    vHeight = (transpose(rot) * viewPos).z;
    vec4 clip = projection * vec4(viewPos, 1.0);
    // On the far plane, as the clouds are, so the depth test the sky is now
    // drawn under rejects it wherever the world has already been drawn.
    gl_Position = vec4(clip.xy, clip.w, clip.w);
}
