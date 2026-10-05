#version 450

// The M2 caster side of the shadow map, instanced: every copy of a model is
// drawn by one call, each reading its own model matrix from the instance
// buffer. shadow.vert.glsl is the same shader taking one combined matrix per
// draw, which made a forest a draw call per tree per batch; it still draws
// terrain and buildings, and M2 when this one cannot.
//
// Like shadow.vert.glsl, it moves nothing: a model is where its own bones
// put it.
layout(push_constant) uniform Push {
    mat4 lightSpace;        // the light's view-projection; the model is per instance
    vec4 sway;              // unused; kept so the layout shared with ShadowPush holds
    ivec4 flags;            // x useTexture, y alphaTest, z unused, w first instance
    vec4 wind;              // unused, as sway
} push;

layout(std430, set = 1, binding = 0) readonly buffer ShadowInstances {
    mat4 instanceModel[];
};

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aBoneWeights;
layout(location = 3) in vec4 aBoneIndicesF;

layout(location = 0) out vec2 TexCoord;

void main() {
    mat4 model = instanceModel[push.flags.w + gl_InstanceIndex];
    vec4 pos = vec4(aPos, 1.0);

    TexCoord = aTexCoord;
    // Combined first, as the CPU combined them, so the depths match the
    // non-instanced path's rather than rounding the other way round.
    gl_Position = (push.lightSpace * model) * pos;
}
