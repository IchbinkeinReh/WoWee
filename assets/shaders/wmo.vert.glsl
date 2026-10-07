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

layout(push_constant) uniform Push {
    mat4 model;
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec4 aColor;
layout(location = 4) in vec4 aTangent;
layout(location = 5) in vec2 aTexCoord2;  // the second MOTV
layout(location = 6) in vec4 aColor2;     // the second MOCV

layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 Normal;
layout(location = 2) out vec2 TexCoord;
layout(location = 3) out vec4 VertColor;
layout(location = 4) out vec3 Tangent;
layout(location = 5) out vec3 Bitangent;
// MapObjDiffuse_T1_Refl's second coordinates: reflect(P, N).xy in view space,
// unnormalised (the client's +z-forward view gives the same x and y).
layout(location = 6) out vec2 EnvCoord;
layout(location = 7) out vec2 TexCoord2;
layout(location = 8) out vec4 VertColor2;

void main() {
    // A building's geometry stays where it was authored. The client moves
    // nothing in a WMO procedurally, cloth painted into a wall included.
    vec4 worldPos = push.model * vec4(aPos, 1.0);
    FragPos = worldPos.xyz;

    mat3 normalMatrix = mat3(push.model);
    Normal = normalMatrix * aNormal;
    TexCoord = aTexCoord;
    VertColor = aColor;
    TexCoord2 = aTexCoord2;
    VertColor2 = aColor2;

    // Compute TBN basis vectors for normal mapping
    vec3 T = normalize(normalMatrix * aTangent.xyz);
    vec3 N = normalize(Normal);
    // Gram-Schmidt re-orthogonalize
    T = normalize(T - dot(T, N) * N);
    vec3 B = cross(N, T) * aTangent.w;

    Tangent = T;
    Bitangent = B;

    vec3 viewP = (view * worldPos).xyz;
    vec3 viewN = normalize(mat3(view) * Normal);
    EnvCoord = reflect(normalize(viewP), viewN).xy;

    gl_Position = projection * view * worldPos;
}
