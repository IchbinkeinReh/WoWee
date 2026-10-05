#version 450

// The caster side of the shadow map, for terrain, buildings and doodads.
//
// Casters are drawn exactly where the main passes draw them. Nothing in the
// client moves a model except its own bones and texture tracks, so there is
// no displacement here to keep in step with anything.
layout(push_constant) uniform Push {
    mat4 lightSpaceModel;   // light-space * model, multiplied on the CPU
    vec4 sway;              // unused; kept so the layout shared with ShadowPush holds
    ivec4 flags;            // x useTexture, y alphaTest, z unused
    vec4 wind;              // unused, as sway
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aBoneWeights;
layout(location = 3) in vec4 aBoneIndicesF;

layout(location = 0) out vec2 TexCoord;

void main() {
    vec4 pos = vec4(aPos, 1.0);

    TexCoord = aTexCoord;
    gl_Position = push.lightSpaceModel * pos;
}
