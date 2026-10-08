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
    vec4 volumetricParams;  // x = on, y = near, z = 1 / ln(far / near), w = slices
    mat4 rtViewProj;
    vec4 rtCameraPos;
    vec4 rtParams;
    vec4 cameraFogColor;
    vec4 averagedDirectColor;
    vec4 averagedAmbientColor;
    vec4 windowLight;
    vec4 specularColor;  // the light's specular colour; w 1 with the 'specular' option
    // The shadow cascades (shadow_csm.glsli); GPUPerFrameData ends with them.
    mat4 cascadeMatrix[4];
    vec4 cascadeRect[4];
    vec4 cascadeTexel[4];
    vec4 cascadeInfo;
};

#include "rt_lighting.glsli"
#include "texture_filter.glsli"
#include "shadow_csm.glsli"

layout(set = 1, binding = 0) uniform sampler2D uBaseTexture;
layout(set = 1, binding = 1) uniform sampler2D uLayer1Texture;
layout(set = 1, binding = 2) uniform sampler2D uLayer2Texture;
layout(set = 1, binding = 3) uniform sampler2D uLayer3Texture;
layout(set = 1, binding = 4) uniform sampler2D uLayer1Alpha;
layout(set = 1, binding = 5) uniform sampler2D uLayer2Alpha;
layout(set = 1, binding = 6) uniform sampler2D uLayer3Alpha;

// The chunk's baked shadow (MCSH), 1 lit and 0 shadowed; 1 everywhere for a
// chunk without one.
layout(set = 1, binding = 8) uniform sampler2D uBakedShadow;

// Layer 0 of an env chunk: a cube map sampled at the reflection.
layout(set = 1, binding = 9) uniform samplerCube uEnvCube;

layout(set = 1, binding = 7) uniform TerrainParams {
    int layerCount;
    int hasLayer1;
    int hasLayer2;
    int hasLayer3;
    ivec4 layerFlags;  // MCLY flags of layers 0..3: 0x40 scrolls, 0x80 unlit
    int weightedLayers;  // MPHD 0x4: Terrain1's weighted variants
    int envLayer;        // layer 0 from uEnvCube (MCLY 0x400, 0x007b9250)
};

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec2 LayerUV;
layout(location = 4) in vec3 Shading;  // MCCV, 1 neutral

layout(location = 0) out vec4 outColor;

// A layer's alpha, as the client samples it: the 64x64 map stretched over
// the chunk, nothing more (0x007d06b0). A blur near the chunk's edges was
// here to hide seams; the client has none.
float sampleAlpha(sampler2D tex, vec2 uv) {
    return texture(tex, uv).r;
}

// A scrolling layer's offset (MCLY 0x40), as terrainLayerAnimOffset in
// pipeline/terrain_mesh.hpp works it out: the direction (flags & 7) times the
// time, a positive component wrapping at 64 (0x00cd77f8), over the speed's
// divisor (0x00af14f8), the y moving u and the x moving v, both against it.
vec2 layerAnimOffset(int flags) {
    if ((flags & 0x40) == 0) return vec2(0.0);
    const vec2 kDir[8] = vec2[8](vec2(-1, 0), vec2(-1, 1), vec2(0, 1), vec2(1, 1),
                                 vec2(1, 0), vec2(1, -1), vec2(0, -1), vec2(-1, -1));
    const float kDivisor[8] = float[8](64.0, 48.0, 32.0, 16.0, 8.0, 4.0, 2.0, 1.0);
    vec2 dir = kDir[flags & 7];
    vec2 acc = dir * fogParams.z;
    if (dir.x > 0.0) acc.x = mod(acc.x, 64.0);
    if (dir.y > 0.0) acc.y = mod(acc.y, 64.0);
    return -acc.yx / kDivisor[(flags >> 3) & 7];
}

// A layer's colour, its scroll applied.
vec4 layerTexel(sampler2D tex, int flags) {
    // The scroll does not change across a pixel, so the coordinate's own
    // derivatives are the layer's.
    return textureFilteredGrad(tex, TexCoord + layerAnimOffset(flags), dFdx(TexCoord), dFdy(TexCoord),
                               textureFilterMode(viewPos.w));
}

// The air between the camera and this point, out of the fog volume: rgb is
// the light it scatters toward the camera, a how much of the point shows
// through it. See VolumetricFog.
vec4 fogVolumeAt(vec3 worldPos) {
    vec4 clip = projection * view * vec4(worldPos, 1.0);
    float depth = max(clip.w, 1e-4);
    vec2 uv = clip.xy / depth * 0.5 + 0.5;
    float slice = log(max(depth, volumetricParams.y) / volumetricParams.y) * volumetricParams.z;
    // Each slice holds the air up to its far edge, so a point is read half a
    // slice back from where it stands.
    return textureLod(uFogVolume, vec3(uv, slice - 0.5 / volumetricParams.w), 0.0);
}

// The zone's distance fog, then the air in front of it. The distance fog is
// the far haze the sky is painted to meet, so it goes on first; the volume is
// everything between the camera and that, sunlit shafts included.
// `distanceFog`: the zone's fog colour here (0x007f16f0's 0xd38b8c).
vec3 applyFog(vec3 color, vec3 worldPos, float dist, vec3 distanceFog) {
    // Raised to fogColor.w: 1 before map 530, the later fog's exponent after (0x00873210).
    float fogFactor = pow(clamp((fogParams.y - dist) / (fogParams.y - fogParams.x), 0.0, 1.0), max(fogColor.w, 1.0));
    color = mix(distanceFog, color, fogFactor);
    if (volumetricParams.x > 0.5) {
        vec4 air = fogVolumeAt(worldPos);
        color = color * air.a + air.rgb;
    }
    return color;
}

void main() {
    // The fog's distance is the view depth: the Terrain vertex program takes it
    // from the model-view matrix's z row (c2) into ((end - z) / (end - start))^exp
    // (c12), not the distance to the eye.
    float fragDist = -(view * vec4(FragPos, 1.0)).z;

    // The layers' weights, as the Terrain1 pixel programs blend them: one over
    // another by its alpha (lerp after lerp), or on a map with MPHD flag 0x4
    // the weighted variants' base x (1 - saturate(a1 + a2 + a3)) plus each
    // layer by its own alpha (0x0079e5c0 picks the variant by 0x00cf08d0).
    float a1 = hasLayer1 != 0 ? sampleAlpha(uLayer1Alpha, LayerUV) : 0.0;
    float a2 = hasLayer2 != 0 ? sampleAlpha(uLayer2Alpha, LayerUV) : 0.0;
    float a3 = hasLayer3 != 0 ? sampleAlpha(uLayer3Alpha, LayerUV) : 0.0;
    vec4 w;
    if (weightedLayers != 0) {
        w = vec4(1.0 - clamp(a1 + a2 + a3, 0.0, 1.0), a1, a2, a3);
    } else {
        w = vec4((1.0 - a1) * (1.0 - a2) * (1.0 - a3), a1 * (1.0 - a2) * (1.0 - a3),
                 a2 * (1.0 - a3), a3);
    }

    // A layer is lit - 2 x the vertex colour times its texel - or with MCLY
    // 0x80 its texel as it is (the per-layer variants' c1, 0x007d0760). The
    // blend is linear, so the lit and the unlit layers are summed apart. Where
    // a layer has no weight its fetch is skipped; a layer covers part of a
    // chunk, so whole regions of the screen take that branch together.
    vec3 litColor = vec3(0.0);
    vec3 unlitColor = vec3(0.0);
    vec3 norm = normalize(Normal);
    // The texels' alpha, blended alike: the highlight's mask.
    float specMask = 0.0;
    {
        // An env chunk's layer 0 is a cube at the reflection: the Terrain env
        // vertex programs reflect the view-space position about the normal,
        // back to the world by c8-c10, swizzled xzy (the client's z up).
        vec4 t0;
        if (envLayer != 0) {
            vec3 r = reflect(FragPos - viewPos.xyz, norm);
            t0 = texture(uEnvCube, vec3(r.y, r.x, r.z).xzy);
        } else {
            t0 = layerTexel(uBaseTexture, layerFlags.x);
        }
        vec3 t = t0.rgb * w.x;
        specMask += t0.a * w.x;
        if ((layerFlags.x & 0x80) != 0) unlitColor += t; else litColor += t;
    }
    if (w.y > 0.002) {
        vec4 t = layerTexel(uLayer1Texture, layerFlags.y) * w.y;
        specMask += t.a;
        if ((layerFlags.y & 0x80) != 0) unlitColor += t.rgb; else litColor += t.rgb;
    }
    if (w.z > 0.002) {
        vec4 t = layerTexel(uLayer2Texture, layerFlags.z) * w.z;
        specMask += t.a;
        if ((layerFlags.z & 0x80) != 0) unlitColor += t.rgb; else litColor += t.rgb;
    }
    if (w.w > 0.002) {
        vec4 t = layerTexel(uLayer3Texture, layerFlags.w) * w.w;
        specMask += t.a;
        if ((layerFlags.w & 0x80) != 0) unlitColor += t.rgb; else litColor += t.rgb;
    }

    // The vertex colour, which the pixel program doubles: the Terrain vertex
    // program's clamp(ambient + clamp(N.L) x direct) (c25, c24, c26), clamped
    // to one before the chunk's vertex shading (MCCV) scales it.
    RtLight rt = rtLightAt(FragPos);
    float ndl = clamp(dot(norm, normalize(-lightDir.xyz)), 0.0, 1.0);
    vec3 light = clamp(rtAmbient(rt, ambientColor.rgb) + ndl * lightColor.rgb, 0.0, 1.0) * Shading;

    // The shadow: the baked one (MCSH, the alpha texture's fourth channel),
    // or with the sun's shadow map on, the map alone. Whatever is lit, lit
    // and unlit layers alike, is scaled by 0.7 + 0.3 x the light let through
    // (Terrain2/Terrain3).
    //
    // The client takes the lesser of the two, but its map is a coarse one
    // and off by default. Beside this one the baked mask - laid down offline
    // for one sun, trees and buildings included - is a second set of
    // shadows that matches neither the sun overhead nor anything standing
    // there, soft streaks across open ground with no caster in sight.
    float lit = texture(uBakedShadow, LayerUV).r;
    if (shadowParams.x > 0.5) {
        vec3 ldir = normalize(-lightDir.xyz);
        float nl = dot(norm, ldir);
        float bias = max(0.0005 * (1.0 - abs(nl)), 0.00005);
        float shadow = csmShadow(uShadowMap, FragPos, norm, nl, bias);
        shadow = mix(1.0, shadow, shadowParams.y);
        lit = rtShadow(rt, shadow);
    }

    vec3 result = (litColor * light + unlitColor) * (0.7 + 0.3 * lit);

    // The 'specular' option (off by default): the specular vertex programs'
    // secondary colour, pow(max(N.H, 0), 20) x the light's specular (c27),
    // H halfway between the eye and the light, added by the texels' alpha
    // and the shadow (Terrain1: secondary x tex.a x shadow).
    if (specularColor.w > 0.5) {
        vec3 h = normalize(normalize(viewPos.xyz - FragPos) + normalize(-lightDir.xyz));
        float s = pow(max(dot(norm, h), 0.0), 20.0);
        result += s * specularColor.rgb * specMask * lit;
    }

    result = applyFog(result, FragPos, fragDist, fogColor.rgb);

    outColor = vec4(result, 1.0);
}
