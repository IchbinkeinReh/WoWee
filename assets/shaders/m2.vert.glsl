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
    vec4 playerPos;   // xyz = player world position, w = horizontal speed
    vec4 playerWake;  // xyz = trailing player position (springback reference)
};

// Per-draw push constants (batch-level data only)
layout(push_constant) uniform Push {
    int texCoordSet;         // UV set index (0 or 1)
    int isFoliage;           // -1 sky, 0 everything else
    int instanceDataOffset;  // Base index into InstanceSSBO for this draw group
    float swayRefHeight;     // Unused; kept so the push layout does not move
    float swayAmp;           // Unused; kept so the push layout does not move
    float plantHeight;       // Unused; kept so the layout holds
} push;

layout(set = 2, binding = 0) readonly buffer BoneSSBO {
    mat4 bones[];
};

// Per-instance data read via gl_InstanceIndex (GPU instancing)
struct InstanceData {
    mat4 model;
    vec2 uvOffset;
    float fadeAlpha;
    int useBones;
    int boneBase;
    int boneCount;
    // 0 for an ordinary instance, 1 while the player is pressing on it. Sits
    // in what was padding.
    float highlight;
    // 1: a WMO interior doodad, lit by the two colours below rather than
    // the zone's light. Also in what was padding.
    int flags;
    // The texture matrix's linear part, rows (m00, m01) and (m10, m11);
    // uvOffset is its translation.
    vec4 uvLinear;
    // The batch's animated colour and alpha for this instance; alpha below
    // zero means the material's static values apply.
    vec4 colorMul;
    vec4 interiorAmbient;
    vec4 interiorDirect;
};
layout(set = 3, binding = 0) readonly buffer InstanceSSBO {
    InstanceData instanceData[];
};

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec4 aBoneWeights;
layout(location = 4) in vec4 aBoneIndicesF;
layout(location = 5) in vec2 aTexCoord2;

layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 Normal;
layout(location = 2) out vec2 TexCoord;
layout(location = 5) out float vFadeAlpha;
layout(location = 6) flat out int vSkyMode;
layout(location = 7) flat out float vHighlight;
layout(location = 8) flat out vec4 vColorMul;
layout(location = 9) flat out int vInteriorLit;
layout(location = 10) flat out vec3 vInteriorAmbient;
layout(location = 11) flat out vec3 vInteriorDirect;

void main() {
    // Fetch per-instance data from SSBO
    int instIdx = push.instanceDataOffset + gl_InstanceIndex;
    mat4 model = instanceData[instIdx].model;
    vec2 uvOff = instanceData[instIdx].uvOffset;
    float fade = instanceData[instIdx].fadeAlpha;
    int uBones = instanceData[instIdx].useBones;
    int bBase  = instanceData[instIdx].boneBase;

    vec4 pos = vec4(aPos, 1.0);
    vec4 norm = vec4(aNormal, 0.0);

    if (uBones != 0) {
        // Clamp to the range this instance actually owns. A model whose bone
        // count exceeds the ceiling would otherwise index into the next
        // instance's matrices and fling its vertices across the world.
        int bCount = max(instanceData[instIdx].boneCount, 1);
        ivec4 bi = clamp(ivec4(aBoneIndicesF), ivec4(0), ivec4(bCount - 1));
        mat4 skinMat = bones[bBase + bi.x] * aBoneWeights.x
                     + bones[bBase + bi.y] * aBoneWeights.y
                     + bones[bBase + bi.z] * aBoneWeights.z
                     + bones[bBase + bi.w] * aBoneWeights.w;
        pos = skinMat * pos;
        norm = skinMat * norm;
    }

    // Nothing here moves the geometry on its own. The client animates a
    // doodad only through its own bones and texture tracks - a tree that
    // sways, a banner that ripples or a boat that rocks does so because its
    // artist keyed it - so the only displacement is the skinning above.
    vec4 worldPos = model * pos;

    FragPos = worldPos.xyz;
    Normal = mat3(model) * norm.xyz;

    vec2 baseUV = push.texCoordSet == 1 ? aTexCoord2 : aTexCoord;
    vec4 uvLin = instanceData[instIdx].uvLinear;
    TexCoord = vec2(dot(uvLin.xy, baseUV), dot(uvLin.zw, baseUV)) + uvOff;

    vFadeAlpha = fade;
    vColorMul = instanceData[instIdx].colorMul;
    vSkyMode = push.isFoliage < 0 ? 1 : 0;
    vHighlight = instanceData[instIdx].highlight;
    vInteriorLit = instanceData[instIdx].flags & 1;
    vInteriorAmbient = instanceData[instIdx].interiorAmbient.rgb;
    vInteriorDirect = instanceData[instIdx].interiorDirect.rgb;

    gl_Position = projection * view * worldPos;
    // A sky model sits on the far plane whatever its radius, so the depth
    // test rejects it wherever terrain has already been drawn. The sky is
    // drawn after the ground now, and this is what makes that free.
    if (push.isFoliage < 0) gl_Position.z = gl_Position.w;
}
