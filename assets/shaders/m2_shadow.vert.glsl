#version 450

// The M2 caster side of the shadow map, instanced: every copy of a model is
// drawn by one call, each reading its own model matrix from the instance
// buffer. shadow.vert.glsl is the same shader taking one combined matrix per
// draw, which made a forest a draw call per tree per batch; it still draws
// terrain and buildings, and M2 when this one cannot.
//
// The wind is shadow.vert.glsl's, line for line, and so m2.vert.glsl's - see
// the note there. The per-tree phase it took from sway.xy comes from the
// instance's own origin here, which is what the CPU used to put there.
layout(push_constant) uniform Push {
    mat4 lightSpace;        // the light's view-projection; the model is per instance
    vec4 sway;              // z reference height, w amplitude (xy unused)
    ivec4 flags;            // x useTexture, y alphaTest, z foliageSway, w first instance
    vec4 wind;              // x windTime
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

    if (push.flags.z != 0) {
        vec2 worldRef = model[3].xy;
        float heightFactor = clamp(pos.z / max(push.sway.z, 0.01), 0.0, 1.0);
        heightFactor *= heightFactor;   // quadratic - the base stays planted
        float amp = push.sway.w * heightFactor;

        // Layer 1: Trunk sway - slow, large amplitude
        float trunkPhase = push.wind.x * 0.8 + dot(worldRef, vec2(0.1, 0.13));
        float trunkSwayX = sin(trunkPhase) * 0.35 * amp;
        float trunkSwayY = cos(trunkPhase * 0.7) * 0.25 * amp;

        // Layer 2: Branch sway - medium frequency, per-branch phase
        float branchPhase = push.wind.x * 1.7 + dot(worldRef, vec2(0.37, 0.71));
        float branchSwayX = sin(branchPhase + pos.y * 0.4) * 0.15 * amp;
        float branchSwayY = cos(branchPhase * 1.1 + pos.x * 0.3) * 0.12 * amp;

        // Layer 3: Leaf flutter - fast, small amplitude, per-vertex
        float leafPhase = push.wind.x * 4.5 + dot(aPos, vec3(1.7, 2.3, 0.9));
        float leafFlutterX = sin(leafPhase) * 0.06 * amp;
        float leafFlutterY = cos(leafPhase * 1.3) * 0.05 * amp;

        pos.x += trunkSwayX + branchSwayX + leafFlutterX;
        pos.y += trunkSwayY + branchSwayY + leafFlutterY;
    }

    TexCoord = aTexCoord;
    // Combined first, as the CPU combined them, so the depths match the
    // non-instanced path's rather than rounding the other way round.
    gl_Position = (push.lightSpace * model) * pos;
}
