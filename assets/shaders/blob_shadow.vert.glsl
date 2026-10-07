#version 450

// A unit's blob shadow, laid on the ground's own triangles: the client's
// color_proj_proj / diffuse_proj_proj vertex programs - the position through
// the view, texture 0 and texture 1 each through a matrix of the world
// position (0x007e2d60), no fog (0x007e4480 turns it off).

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

layout(push_constant) uniform Push {
    vec4 uRow;    // the blob's u
    vec4 vRow;    // the blob's v
    vec4 hRow;    // the height fade's coordinate
    vec4 params;  // x: the model's alpha
} push;

layout(location = 0) in vec3 aPos;
layout(location = 0) out vec2 vBlobUV;
layout(location = 1) out float vHeight;

void main() {
    vec4 p = vec4(aPos, 1.0);
    vBlobUV = vec2(dot(push.uRow, p), dot(push.vRow, p));
    vHeight = dot(push.hRow, p);
    gl_Position = projection * view * p;
}
