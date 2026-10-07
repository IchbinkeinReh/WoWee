#version 450

// M2 ribbon emitter fragment shader: the texture times the vertex colour,
// alpha tested at the material's reference and fogged unless the material
// is unfogged (0x00980b70, 0x00873390, 0x00873ee0). The blend is the
// pipeline's.

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
    vec4 playerPos;
    vec4 playerWake;
    vec4 volumetricParams;
};

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform RibbonMaterial {
    float alphaRef;
    int lit;
    int fogged;
} mat;

layout(location = 0) in vec3 vColor;
layout(location = 1) in float vAlpha;
layout(location = 2) in vec2 vUV;
layout(location = 3) in float vFogFactor;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(uTexture, vUV);
    float a = tex.a * vAlpha;
    if (a < mat.alphaRef) discard;
    vec3 rgb = tex.rgb * vColor;
    // The world's fog, as for any fogged draw.
    if (mat.fogged != 0) rgb = mix(fogColor.rgb, rgb, vFogFactor);
    outColor = vec4(rgb, a);
}
