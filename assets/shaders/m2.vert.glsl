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
    int texCoordSet;         // per stage: 0 UV0, 1 UV1, 2 env map; stage 0 in bits 0-1, stage 1 in 2-3
    int isFoliage;           // -1 sky, 0 everything else
    int instanceDataOffset;  // Base index into InstanceSSBO for this draw group
    // Ground cover by chunk: where this draw's entries start in ClutterList,
    // or -1 for an ordinary draw from InstanceSSBO.
    int clutterList;
    // The view distance squared, and the model's box centre in its own space:
    // a chunk's tuft whose centre is past it is not drawn, as the cull did
    // for one instance at a time.
    float clutterDistSq;
    float centerX;
    float centerY;
    float centerZ;
    // The batch's texture matrices for a chunk's draw - the same for every
    // tuft, as only a model whose texture tracks hold still is drawn this way:
    // the two stages' linear rows, then their translations (xy, zw).
    vec4 clutterUvLinear;
    vec4 clutterUvLinear2;
    vec4 clutterUvOffsets;
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
    // the zone's light. 2: in a WMO group of the camera's interior pass, so
    // fogged in the camera's colour. 4: a game object outside, lit by the
    // two colours below as the zone's light reaches it. Also in what was
    // padding.
    int flags;
    // The texture matrix's linear part, rows (m00, m01) and (m10, m11);
    // uvOffset is its translation.
    vec4 uvLinear;
    // The batch's animated colour and alpha for this instance; alpha below
    // zero means the material's static values apply.
    vec4 colorMul;
    vec4 interiorAmbient;
    vec4 interiorDirect;
    // The second texture stage's matrix: linear rows, then the translation.
    vec4 uvLinear2;
    vec4 uvOffset2;
};
layout(set = 3, binding = 0) readonly buffer InstanceSSBO {
    InstanceData instanceData[];
};
// This frame's chunked ground cover: x the entry in ClutterStatic, y the
// base of its bones this frame.
layout(set = 3, binding = 1) readonly buffer ClutterListSSBO {
    ivec2 clutterList[];
};
// Every placed tuft's entry, written once when its chunk is placed.
layout(set = 3, binding = 2) readonly buffer ClutterStaticSSBO {
    InstanceData clutterData[];
};
// Outside the ray traced lighting, the client's M2 point lights
// (m2_vertex_lights.hpp): the set each instance entry draws with, 0 for
// none, then the sets - per light its position (w 1 when set) and diffuse.
layout(set = 3, binding = 3) readonly buffer VertexLightSSBO {
    int vertexLightSetOf[131072];
    vec4 vertexLightSets[];
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
layout(location = 3) out vec2 TexCoord2;
layout(location = 5) out float vFadeAlpha;
layout(location = 6) flat out int vSkyMode;
layout(location = 7) flat out float vHighlight;
layout(location = 8) flat out vec4 vColorMul;
layout(location = 9) flat out int vInteriorLit;
layout(location = 10) flat out vec3 vInteriorAmbient;
layout(location = 11) flat out vec4 vInteriorDirect;
layout(location = 12) flat out float vDetailLit;  // ground cover's baked shadow (flags & 8)
// The client's point lights summed at this vertex (m2_vertex_lights.hpp).
layout(location = 13) out vec3 vPointLight;

void main() {
    // Fetch per-instance data from SSBO
    InstanceData inst;
    int bBase;
    if (push.clutterList >= 0) {
        ivec2 entry = clutterList[push.clutterList + gl_InstanceIndex];
        inst = clutterData[entry.x];
        bBase = entry.y;
        inst.uvLinear = push.clutterUvLinear;
        inst.uvOffset = push.clutterUvOffsets.xy;
        inst.uvLinear2 = push.clutterUvLinear2;
        inst.uvOffset2 = vec4(push.clutterUvOffsets.zw, 0.0, 0.0);
        vec3 centre = (inst.model * vec4(push.centerX, push.centerY, push.centerZ, 1.0)).xyz;
        vec3 toCam = centre - viewPos.xyz;
        if (dot(toCam, toCam) > push.clutterDistSq) {
            // Every vertex of the instance at one point outside the clip
            // volume: nothing of it is drawn.
            gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
            return;
        }
    } else {
        inst = instanceData[push.instanceDataOffset + gl_InstanceIndex];
        bBase = inst.boneBase;
    }
    mat4 model = inst.model;
    vec2 uvOff = inst.uvOffset;
    float fade = inst.fadeAlpha;
    int uBones = inst.useBones;

    vec4 pos = vec4(aPos, 1.0);
    vec4 norm = vec4(aNormal, 0.0);

    if (uBones != 0) {
        // Clamp to the range this instance actually owns. A model whose bone
        // count exceeds the ceiling would otherwise index into the next
        // instance's matrices and fling its vertices across the world.
        int bCount = max(inst.boneCount, 1);
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

    // Each stage's coordinates (0x00836600 names the vertex program by them):
    // a UV set through that stage's own matrix, or the sphere map. The client's
    // view space looks down +z (its lookAt, 0x006bfe60), and Diffuse_Env takes
    // R = -reflect(P, N) there - (-r.x, -r.y, r.z) in this view - and maps
    // normalize(R + (0, 0, 1)).xy * 0.5 + 0.5.
    vec3 viewP = (view * worldPos).xyz;
    vec3 viewN = normalize(mat3(view) * Normal);
    vec3 r = reflect(normalize(viewP), viewN);
    vec3 m = vec3(-r.x, -r.y, r.z + 1.0);
    vec2 envUV = m.xy / max(length(m), 1e-5) * 0.5 + 0.5;
    int src0 = push.texCoordSet & 3;
    int src1 = (push.texCoordSet >> 2) & 3;
    vec2 uv0 = src0 == 1 ? aTexCoord2 : aTexCoord;
    vec2 uv1 = src1 == 1 ? aTexCoord2 : aTexCoord;
    vec4 uvLin = inst.uvLinear;
    TexCoord = src0 == 2 ? envUV : vec2(dot(uvLin.xy, uv0), dot(uvLin.zw, uv0)) + uvOff;
    vec4 uvLin2 = inst.uvLinear2;
    // 3: Diffuse_Env_T2's second stage, the first UV set through the identity
    // the env stage left in its matrix slot (0x0081f450, 0x00873550).
    TexCoord2 = src1 == 2 ? envUV
              : src1 == 3 ? aTexCoord
              : vec2(dot(uvLin2.xy, uv1), dot(uvLin2.zw, uv1)) + inst.uvOffset2.xy;

    vFadeAlpha = fade;
    vColorMul = inst.colorMul;
    vSkyMode = push.isFoliage < 0 ? 1 : 0;
    vHighlight = inst.highlight;
    vInteriorLit = inst.flags;
    vInteriorAmbient = inst.interiorAmbient.rgb;
    vInteriorDirect = inst.interiorDirect;
    vDetailLit = inst.interiorAmbient.a;

    // Lights 1-3 of the model's four nearest (0x008353d0), by vertex as the
    // vertex programs light them: diffuse x max(N.L, 0) / (0.7 d + 0.03 d^2)
    // (0x00834a40's attenuation, Diffuse_T1's point-light variant).
    vPointLight = vec3(0.0);
    if (push.clutterList < 0) {
        int set = vertexLightSetOf[push.instanceDataOffset + gl_InstanceIndex];
        if (set > 0) {
            vec3 n = normalize(Normal);
            for (int i = 0; i < 3; ++i) {
                vec4 lp = vertexLightSets[set * 6 + i * 2];
                if (lp.w < 0.5) continue;
                vec3 toL = lp.xyz - worldPos.xyz;
                float d = length(toL);
                if (d < 1e-5) continue;
                float atten = 1.0 / (0.7 * d + 0.03 * d * d);
                vPointLight += vertexLightSets[set * 6 + i * 2 + 1].rgb * (max(dot(n, toL / d), 0.0) * atten);
            }
        }
    }

    gl_Position = projection * view * worldPos;
    // A sky model sits on the far plane whatever its radius, so the depth
    // test rejects it wherever terrain has already been drawn. The sky is
    // drawn after the ground now, and this is what makes that free.
    // (Depth is reversed: the far plane is 0.)
    if (push.isFoliage < 0) gl_Position.z = 0.0;
}
