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
};

#include "rt_lighting.glsli"

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

layout(set = 1, binding = 7) uniform TerrainParams {
    int layerCount;
    int hasLayer1;
    int hasLayer2;
    int hasLayer3;
    ivec4 layerFlags;  // MCLY flags of layers 0..3: 0x40 scrolls, 0x80 unlit
};

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec2 LayerUV;
layout(location = 4) in vec3 Shading;  // MCCV, 1 neutral

layout(location = 0) out vec4 outColor;

// One texel of the shadow map, handed in by the renderer. The map is 512,
// 1024, 2048 or 4096 a side by the quality setting; this used to be a
// constant for 4096, so at 512 the filter taps all landed inside one texel
// and the bias shrank eightfold. The fallback covers a per-frame block that
// never filled the slot in, such as the character preview's.
float shadowTexel() {
    return shadowParams.z > 0.0 ? shadowParams.z : 1.0 / 4096.0;
}

float sampleShadowPCF(sampler2DShadow smap, vec3 coords) {
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            shadow += texture(smap, vec3(coords.xy + vec2(x, y) * shadowTexel(), coords.z));
        }
    }
    return shadow / 9.0;
}

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
    return texture(tex, TexCoord + layerAnimOffset(flags));
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
    float fragDist = length(viewPos.xyz - FragPos);

    // WoW terrain: layers are blended sequentially, each on top of the previous result.
    // Alpha=1 means the layer fully covers everything below; alpha=0 means invisible.
    // A layer with MCLY 0x80 is drawn with the lighting off (0x007d0760), so
    // the lit and the unlit layers are kept apart: each mix() is linear, and
    // the two add up to the one blend.
    vec4 baseColor = layerTexel(uBaseTexture, layerFlags.x);
    bool unlit0 = (layerFlags.x & 0x80) != 0;
    vec4 finalColor = unlit0 ? vec4(0.0) : baseColor;
    vec3 unlitColor = unlit0 ? baseColor.rgb : vec3(0.0);
    if (hasLayer1 != 0) {
        float a1 = sampleAlpha(uLayer1Alpha, LayerUV);
        // Where the layer is not painted, mix() returns what it was given and
        // the fetch that fed it was work for nothing. A layer covers part of a
        // chunk, so whole regions of the screen take this branch together.
        if (a1 > 0.002) {
            vec4 t = layerTexel(uLayer1Texture, layerFlags.y);
            bool u = (layerFlags.y & 0x80) != 0;
            finalColor = mix(finalColor, u ? vec4(0.0) : t, a1);
            unlitColor = mix(unlitColor, u ? t.rgb : vec3(0.0), a1);
        }
    }
    if (hasLayer2 != 0) {
        float a2 = sampleAlpha(uLayer2Alpha, LayerUV);
        if (a2 > 0.002) {
            vec4 t = layerTexel(uLayer2Texture, layerFlags.z);
            bool u = (layerFlags.z & 0x80) != 0;
            finalColor = mix(finalColor, u ? vec4(0.0) : t, a2);
            unlitColor = mix(unlitColor, u ? t.rgb : vec3(0.0), a2);
        }
    }
    if (hasLayer3 != 0) {
        float a3 = sampleAlpha(uLayer3Alpha, LayerUV);
        if (a3 > 0.002) {
            vec4 t = layerTexel(uLayer3Texture, layerFlags.w);
            bool u = (layerFlags.w & 0x80) != 0;
            finalColor = mix(finalColor, u ? vec4(0.0) : t, a3);
            unlitColor = mix(unlitColor, u ? t.rgb : vec3(0.0), a3);
        }
    }

    // The chunk's vertex shading (MCCV) tints the textures.
    finalColor.rgb *= Shading;
    unlitColor *= Shading;

    // The vertex normal as it is. A bump derived from the texture's own
    // brightness was perturbed into it here; the client has nothing like it.
    vec3 norm = normalize(Normal);

    vec3 lightDir2 = normalize(-lightDir.xyz);
    vec3 ambient = ambientColor.rgb * finalColor.rgb;
    // Lambert, as every other surface has it. This took abs() of the angle
    // and floored it at 0.2, so a slope turned from the sun was lit as one
    // turned toward it, and hills had no shape at low sun. The ambient term
    // is what keeps the shaded side from black, as it does for the models
    // standing on it.
    float diff = max(dot(norm, lightDir2), 0.0);
    vec3 diffuse = diff * lightColor.rgb * finalColor.rgb;

    float shadow = 1.0;
    if (shadowParams.x > 0.5) {
        vec3 ldir = normalize(-lightDir.xyz);
        float normalOffset = shadowTexel() * 2.0 * (1.0 - abs(dot(norm, ldir)));
        vec3 biasedPos = FragPos + norm * normalOffset;
        vec4 lsPos = lightSpaceMatrix * vec4(biasedPos, 1.0);
        vec3 proj = lsPos.xyz / lsPos.w;
        proj.xy = proj.xy * 0.5 + 0.5;
        if (proj.x >= 0.0 && proj.x <= 1.0 && proj.y >= 0.0 && proj.y <= 1.0 && proj.z >= 0.0 && proj.z <= 1.0) {
            float bias = max(0.0005 * (1.0 - abs(dot(norm, ldir))), 0.00005);
            shadow = sampleShadowPCF(uShadowMap, vec3(proj.xy, proj.z - bias));
            shadow = mix(1.0, shadow, shadowParams.y);
        }
    }

    RtLight rt = rtLightAt(FragPos);
    shadow = rtShadow(rt, shadow);
    ambient = rtAmbient(rt, ambientColor.rgb) * finalColor.rgb;

    vec3 result = ambient + shadow * diffuse;

    // The baked terrain shadow (MCSH), drawn as the client draws it when its
    // own dynamic shadows are off: toward the shadow colour, ambient / 3, by
    // ch8's red (0x007ee750; ambientColor.w carries it). Not over the shadow
    // map, which already darkens the same ground.
    if (shadowParams.x < 0.5) {
        float baked = 1.0 - texture(uBakedShadow, LayerUV).r;
        if (baked > 0.0) {
            result = mix(result, ambientColor.rgb / 3.0 * finalColor.rgb,
                         baked * clamp(ambientColor.w, 0.0, 1.0));
        }
    }

    // The unlit layers, as they are: no light, no shadow.
    result += unlitColor;

    result = applyFog(result, FragPos, fragDist, fogColor.rgb);

    outColor = vec4(result, 1.0);
}
