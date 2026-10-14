#version 450

#define MAX_BONES 240u  // must match CharacterRenderer::MAX_BONES

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
    mat4 model;
} push;

layout(set = 2, binding = 0) readonly buffer BoneSSBO {
    mat4 bones[];
};

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aBoneWeights;
layout(location = 2) in uvec4 aBoneIndices;
layout(location = 3) in vec3 aNormal;
layout(location = 4) in vec2 aTexCoord;
layout(location = 5) in vec4 aTangent;
layout(location = 6) in vec2 aTexCoord2;

layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 Normal;
layout(location = 2) out vec2 TexCoord;
layout(location = 3) out vec3 Tangent;
layout(location = 4) out vec3 Bitangent;
layout(location = 5) out vec2 TexCoord2;
layout(location = 6) out vec2 EnvCoord;

vec3 safeNormalize(vec3 v, vec3 fallback) {
    float len2 = dot(v, v);
    if (len2 > 1e-8) {
        return v * inversesqrt(len2);
    }
    return fallback;
}

vec3 fallbackTangent(vec3 n) {
    vec3 axis = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    return safeNormalize(cross(axis, n), vec3(1.0, 0.0, 0.0));
}

void main() {
    // Bone slots past the model's own count stay identity, so clamping keeps a
    // stray index harmless instead of reading past the buffer.
    uvec4 bi = min(aBoneIndices, uvec4(MAX_BONES - 1u));
    mat4 skinMat = bones[bi.x] * aBoneWeights.x
                 + bones[bi.y] * aBoneWeights.y
                 + bones[bi.z] * aBoneWeights.z
                 + bones[bi.w] * aBoneWeights.w;

    vec4 skinnedPos = skinMat * vec4(aPos, 1.0);
    vec3 skinnedNorm = mat3(skinMat) * aNormal;
    vec3 skinnedTan = mat3(skinMat) * aTangent.xyz;

    vec4 worldPos = push.model * skinnedPos;
    mat3 modelMat3 = mat3(push.model);
    // The position the rasteriser gets, taken to view space without ever
    // being a world position. A world coordinate out where the Blood Elves
    // live (|x| ~ 9500) holds a float only to 1/1024 of a yard, and each
    // vertex rounds to it on its own: up to half of that, more than the
    // hair, the eyebrows and the scalp pieces stand off the head (0.2 to 1
    // thousandth of a yard), so they and the face traded places from frame
    // to frame. The model's own extent goes through the rotation part of
    // the view alone, and the instance's origin through the whole view - an
    // expression of uniforms, the same value for every vertex of the draw,
    // whose rounding moves the model as a whole and not one vertex against
    // its neighbour. Algebraically the same as view * model * p:
    //   mat3(view) * (mat3(model) * p.xyz) + p.w * (view * (model[3].xyz, 1)).xyz
    // FragPos stays the world position; lighting and fog do not need it
    // finer than that.
    const vec3 modelOriginView = (view * vec4(push.model[3].xyz, 1.0)).xyz;
    const vec3 viewP = mat3(view) * (modelMat3 * skinnedPos.xyz) + skinnedPos.w * modelOriginView;
    FragPos = worldPos.xyz;
    Normal = modelMat3 * skinnedNorm;
    TexCoord = aTexCoord;
    TexCoord2 = aTexCoord2;
    // The sphere map, as m2.vert.glsl: the client's view space looks down +z
    // (0x006bfe60), and Diffuse_Env takes R = -reflect(P, N) there and maps
    // normalize(R + (0, 0, 1)).xy * 0.5 + 0.5.
    vec3 viewN = normalize(mat3(view) * Normal);
    vec3 r = reflect(normalize(viewP), viewN);
    vec3 m = vec3(-r.x, -r.y, r.z + 1.0);
    EnvCoord = m.xy / max(length(m), 1e-5) * 0.5 + 0.5;

    // Gram-Schmidt re-orthogonalize tangent w.r.t. normal
    vec3 N = safeNormalize(Normal, vec3(0.0, 0.0, 1.0));
    vec3 T = safeNormalize(modelMat3 * skinnedTan, fallbackTangent(N));
    T = safeNormalize(T - dot(T, N) * N, fallbackTangent(N));
    vec3 B = safeNormalize(cross(N, T) * aTangent.w, safeNormalize(cross(N, fallbackTangent(N)), vec3(0.0, 1.0, 0.0)));

    Tangent = T;
    Bitangent = B;

    gl_Position = projection * vec4(viewP, 1.0);
}
