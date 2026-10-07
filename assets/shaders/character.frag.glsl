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
    vec4 cameraFogColor;  // the camera's fog, its interior's blended in
};

#include "rt_lighting.glsli"

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(set = 1, binding = 1) uniform CharMaterial {
    float opacity;
    int alphaTest;
    int unused0;       // was the black colour key: the client never keys by colour
    int unlit;
    float emissiveBoost;
    // Keep these as scalar floats to match the C++ UBO packing. A std140 vec3
    // would insert padding here and shift the following material flags.
    float emissiveTintR;
    float emissiveTintG;
    float emissiveTintB;
    float specularIntensity;
    int enableNormalMap;
    int enablePOM;
    float pomScale;
    int pomMaxSamples;
    float heightMapVariance;
    float normalMapStrength;
    int hairMaterial;
    // 0 normal; 1 Mod and 2 Mod2x, faded toward their neutral 1.0 and 0.5 by
    // alpha; 3 NoAlphaAdd, scaled by opacity alone. See CharMaterialUBO.
    int colourBlend;
    // 0 no fog, 1 the world's fog, 2 to black, 3 to white, 4 to grey.
    int fogMode;
    // The batch's texture matrix: rows of the linear part, and translation.
    float uvM00;
    float uvM01;
    float uvM10;
    float uvM11;
    float uvTx;
    float uvTy;
};

// What a colour-only blend (Mod, Mod2x, NoAlphaAdd) is handed. The 3.3.5a
// client (FUN_0081fe90) draws Mod and Mod2x unlit, with no diffuse and a
// constant emissive of 1.0 and 0.5: the texture, or half of it, whatever the
// alpha - Mod2x then doubles it back, so its net effect is dst * texture.
// NoAlphaAdd's factors ignore alpha, so its fade is in the colour.
vec4 colourBlendOutput(vec3 lit, vec3 tex, float alpha) {
    if (colourBlend == 1) return vec4(tex, alpha);
    if (colourBlend == 2) return vec4(tex * 0.5, alpha);
    if (colourBlend == 3) return vec4(lit * opacity, alpha);
    return vec4(lit, alpha);
}

// The 3.3.5a client's alpha-key reference (FUN_0081fe90): 224/255 of the
// batch's alpha, against an output alpha that carries the same batch alpha -
// so the texture's alpha against 224/255.
const float ALPHA_KEY_REF = 0.8784314;

layout(set = 1, binding = 2) uniform sampler2D uNormalHeightMap;

// After the vertex stage's model matrix: the light of the WMO interior floor
// the character stands on, w = 1 when it has one (0x007a0d60, 0x007c1730).
layout(push_constant) uniform Push {
    layout(offset = 64) vec4 interiorAmbient;
    vec4 interiorDirect;
    // x: 1 in a WMO group of the camera's interior pass, y: the scale on the
    // direct light (CharPushConstants::lightFlags).
    vec4 lightFlags;
} pc;

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 inTexCoord;
// The animated UV, set first thing in main from inTexCoord and the batch's
// texture matrix; everything below samples through it.
vec2 TexCoord;
layout(location = 3) in vec3 Tangent;
layout(location = 4) in vec3 Bitangent;

layout(location = 0) out vec4 outColor;

const int PREVIEW_SIMPLE_TEXTURE_MODE = -31336;

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

// LOD factor from screen-space UV derivatives
float computeLodFactor() {
    vec2 dx = dFdx(TexCoord);
    vec2 dy = dFdy(TexCoord);
    float texelDensity = max(dot(dx, dx), dot(dy, dy));
    return smoothstep(0.0001, 0.005, texelDensity);
}

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

bool finiteVec3(vec3 v) {
    return all(equal(v, v)) && all(lessThan(abs(v), vec3(1e10)));
}

bool isMagentaKeyColor(vec4 color) {
    return color.r >= 0.58 && color.b >= 0.58 && color.g <= 0.48 &&
           color.r >= color.g + 0.22 && color.b >= color.g + 0.22 &&
           abs(color.r - color.b) <= 0.38;
}

ivec2 wrapPreviewTexel(ivec2 texel, ivec2 texSize) {
    return ivec2((texel.x % texSize.x + texSize.x) % texSize.x,
                 (texel.y % texSize.y + texSize.y) % texSize.y);
}

vec4 samplePreviewTexture(sampler2D tex, vec2 uv) {
    ivec2 texSize = textureSize(tex, 0);
    if (texSize.x <= 0 || texSize.y <= 0) {
        return textureLod(tex, uv, 0.0);
    }

    vec2 wrappedUv = uv - floor(uv);
    ivec2 baseTexel = ivec2(floor(wrappedUv * vec2(texSize)));
    baseTexel = wrapPreviewTexel(baseTexel, texSize);

    vec4 color = texelFetch(tex, baseTexel, 0);
    if (!isMagentaKeyColor(color)) {
        return color;
    }

    for (int radius = 1; radius <= 4; ++radius) {
        for (int y = -radius; y <= radius; ++y) {
            for (int x = -radius; x <= radius; ++x) {
                if (abs(x) != radius && abs(y) != radius) {
                    continue;
                }
                vec4 candidate = texelFetch(tex, wrapPreviewTexel(baseTexel + ivec2(x, y), texSize), 0);
                if (!isMagentaKeyColor(candidate)) {
                    return vec4(candidate.rgb, 0.0);
                }
            }
        }
    }

    return vec4(0.0);
}

// Parallax Occlusion Mapping with angle-adaptive sampling
vec2 parallaxOcclusionMap(vec2 uv, vec3 viewDirTS, float lodFactor) {
    float VdotN = abs(viewDirTS.z);

    if (VdotN < 0.15) return uv;

    float angleFactor = clamp(VdotN, 0.15, 1.0);
    int maxS = pomMaxSamples;
    int minS = max(maxS / 4, 4);
    int numSamples = int(mix(float(minS), float(maxS), angleFactor));
    numSamples = int(mix(float(minS), float(numSamples), 1.0 - lodFactor));

    float layerDepth = 1.0 / float(numSamples);
    float currentLayerDepth = 0.0;

    vec2 P = viewDirTS.xy / max(VdotN, 0.15) * pomScale;
    float maxOffset = pomScale * 3.0;
    P = clamp(P, vec2(-maxOffset), vec2(maxOffset));
    vec2 deltaUV = P / float(numSamples);

    // The mip level is chosen once, from the undisplaced UV, and used for
    // every sample in the march. Inside the loop the UV differs from one
    // pixel to the next by how many steps each has taken, so the implicit
    // derivatives were noise and the level chosen from them was too - which
    // showed as sparkle on relief at distance and cost a gradient fetch
    // per sample on top.
    float lod = textureQueryLod(uNormalHeightMap, uv).x;
    vec2 currentUV = uv;
    float currentDepthMapValue = 1.0 - textureLod(uNormalHeightMap, currentUV, lod).a;

    for (int i = 0; i < 64; i++) {
        if (i >= numSamples || currentLayerDepth >= currentDepthMapValue) break;
        currentUV -= deltaUV;
        currentDepthMapValue = 1.0 - textureLod(uNormalHeightMap, currentUV, lod).a;
        currentLayerDepth += layerDepth;
    }

    vec2 prevUV = currentUV + deltaUV;
    float afterDepth = currentDepthMapValue - currentLayerDepth;
    float beforeDepth = (1.0 - textureLod(uNormalHeightMap, prevUV, lod).a) - currentLayerDepth + layerDepth;
    float weight = afterDepth / (afterDepth - beforeDepth + 0.0001);
    vec2 result = mix(currentUV, prevUV, weight);

    float fadeFactor = smoothstep(0.15, 0.35, VdotN);
    return mix(uv, result, fadeFactor);
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
// `distanceFog`: the zone's fog colour, or the camera's for an interior group
// and what stands in one (0x007a8440).
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
    TexCoord = vec2(uvM00 * inTexCoord.x + uvM01 * inTexCoord.y + uvTx,
                    uvM10 * inTexCoord.x + uvM11 * inTexCoord.y + uvTy);
    if (enablePOM == PREVIEW_SIMPLE_TEXTURE_MODE) {
        vec4 texColor = samplePreviewTexture(uTexture, TexCoord);
        if (isMagentaKeyColor(texColor)) {
            discard;
        }
        if (alphaTest == 1 && texColor.a < ALPHA_KEY_REF) {
            discard;
        }
        if (alphaTest == 2 && texColor.a * opacity < 1.0 / 255.0) {
            discard;
        }
        if (alphaTest == 1 && hairMaterial != 0) {
            texColor.a = 1.0;
        }
        outColor = colourBlendOutput(texColor.rgb, texColor.rgb, texColor.a * opacity);
        return;
    }

    float lodFactor = computeLodFactor();
    // Gradients of the authored UV, taken here where every pixel of the quad
    // still agrees on them. Parallax moves the UV by a different amount per
    // pixel, and a mip level chosen from the moved UV flickers.
    vec2 uvDx = dFdx(TexCoord);
    vec2 uvDy = dFdy(TexCoord);

    vec3 vertexNormal = safeNormalize(Normal, vec3(0.0, 0.0, 1.0));
    if (!gl_FrontFacing) vertexNormal = -vertexNormal;

    vec2 finalUV = TexCoord;

    bool usePOM = enablePOM != 0 &&
                  alphaTest != 1 &&
                  heightMapVariance > 0.001 &&
                  lodFactor < 0.99;
    bool useNormalMap = enableNormalMap != 0 &&
                        unlit == 0 &&
                        lodFactor < 0.99 &&
                        normalMapStrength > 0.001;
    mat3 TBN;
    if (usePOM || useNormalMap) {
        vec3 T = safeNormalize(Tangent, fallbackTangent(vertexNormal));
        T = safeNormalize(T - dot(T, vertexNormal) * vertexNormal, fallbackTangent(vertexNormal));
        vec3 B = safeNormalize(Bitangent, safeNormalize(cross(vertexNormal, T), vec3(0.0, 1.0, 0.0)));
        TBN = mat3(T, B, vertexNormal);
    }

    if (usePOM) {
        mat3 TBN_inv = transpose(TBN);
        vec3 viewDirWorld = normalize(viewPos.xyz - FragPos);
        vec3 viewDirTS = TBN_inv * viewDirWorld;
        finalUV = parallaxOcclusionMap(TexCoord, viewDirTS, lodFactor);
    }

    vec4 texColor = textureGrad(uTexture, finalUV, uvDx, uvDy);
    // Repair dark DXT fringes on alpha-cut character textures such as hair.
    // Transparent edge texels can carry black/garbage RGB even when alpha is
    // valid; pull color from a coarser mip and trust the source more as alpha
    // approaches opaque. This matches the generic M2 path.
    if (alphaTest == 1 && texColor.a > 0.01 && texColor.a < 1.0) {
        vec3 mipColor = textureLod(uTexture, finalUV, 4.0).rgb;
        float trust = smoothstep(0.0, 0.9, texColor.a);
        texColor.rgb = mix(mipColor, texColor.rgb, trust);
    }

    // Some classic/TBC character textures use bright magenta as a color key.
    // Apply this before any material-specific alpha path because a few preview
    // batches report as opaque/blended even when their texture still carries
    // mask-color texels.
    if (texColor.r > 0.78 && texColor.g < 0.28 && texColor.b > 0.78) {
        discard;
    }

    if (alphaTest == 1 && hairMaterial != 0) {
        if (texColor.a < ALPHA_KEY_REF) {
            discard;
        }
        texColor.a = 1.0;
    } else if (alphaTest == 1) {
        // Screen-space sharpened alpha for alpha-to-coverage anti-aliasing.
        // Rescales alpha so the cutoff maps to exactly the texel boundary,
        // giving smooth edges when MSAA + alpha-to-coverage is active.
        float aGrad = fwidth(texColor.a);
        texColor.a = clamp((texColor.a - ALPHA_KEY_REF) / max(aGrad, 0.001) * 0.5 + 0.5, 0.0, 1.0);
        if (texColor.a < 1.0 / 255.0) discard;
    } else if (alphaTest == 2 && texColor.a * opacity < 1.0 / 255.0) {
        // A blended mode: the client tests it at 1/255, so what is fully
        // transparent is not drawn and writes no depth. Nothing is discarded
        // for being dark: the client draws the batch by its blend mode and
        // never keys by colour.
        discard;
    }

    // Compute normal (with normal mapping if enabled)
    vec3 norm = vertexNormal;
    if (useNormalMap) {
        vec3 mapNormal = textureGrad(uNormalHeightMap, finalUV, uvDx, uvDy).rgb * 2.0 - 1.0;
        mapNormal.xy *= normalMapStrength;
        mapNormal = safeNormalize(mapNormal, vec3(0.0, 0.0, 1.0));
        vec3 worldNormal = safeNormalize(TBN * mapNormal, vertexNormal);
        if (!gl_FrontFacing) worldNormal = -worldNormal;
        float blendFactor = max(lodFactor, 1.0 - normalMapStrength);
        norm = safeNormalize(mix(worldNormal, vertexNormal, blendFactor), vertexNormal);
    }

    vec3 result;

    if (unlit != 0) {
        // The texture as it is: the client draws an unlit batch with its
        // colour as the emissive and no light (FUN_0081fb10). This was
        // texture * (1 + boost), twice the texture at the defaults.
        result = texColor.rgb;
    } else if (pc.interiorAmbient.w > 1.5) {
        // On an interior floor: its vertex colour as the ambient and as one
        // light down from a fixed direction (0xaeedf0, the same in render
        // space), nothing of the sun's shadow. On a transition face the
        // direction turns toward the sun's by the floor's alpha (0x007c1730),
        // the colours having been carried toward the zone's already.
        const vec3 interiorTravel = vec3(-0.30822, -0.30822, -0.9);
        vec3 toLight = normalize(-mix(interiorTravel, lightDir.xyz, pc.interiorDirect.w));
        float idiff = max(dot(norm, toLight), 0.0);
        result = pc.interiorAmbient.rgb * texColor.rgb
               + idiff * pc.interiorDirect.rgb * pc.lightFlags.y * texColor.rgb;
    } else {
        vec3 ldir = normalize(-lightDir.xyz);
        float diff = max(dot(norm, ldir), 0.0);

        // Ambient and diffuse only, as the client lights a model: no
        // specular term (its 'specular' option is off by default and
        // reaches only the terrain).

        float shadow = 1.0;
        if (shadowParams.x > 0.5) {
            float normalOffset = shadowTexel() * 2.0 * (1.0 - abs(dot(norm, ldir)));
            vec3 biasedPos = FragPos + norm * normalOffset;
            vec4 lsPos = lightSpaceMatrix * vec4(biasedPos, 1.0);
            vec3 proj = lsPos.xyz / lsPos.w;
            proj.xy = proj.xy * 0.5 + 0.5;
            if (proj.x >= 0.0 && proj.x <= 1.0 &&
                proj.y >= 0.0 && proj.y <= 1.0 &&
                proj.z >= 0.0 && proj.z <= 1.0) {
                float bias = max(0.0005 * (1.0 - abs(dot(norm, ldir))), 0.00005);
                shadow = sampleShadowPCF(uShadowMap, vec3(proj.xy, proj.z - bias));
            }
            shadow = mix(1.0, shadow, shadowParams.y);
        }
        RtLight rt = rtLightAt(FragPos);
        shadow = rtShadow(rt, shadow);

        // The ambient as it eases in from an interior's (w = 1), or the
        // zone's as it is (0x007a1e90). The direct light by its scale:
        // halved in the terrain's baked shadow (0x007c1730 multiplies by +0x8c).
        vec3 ambient = pc.interiorAmbient.w > 0.5 ? pc.interiorAmbient.rgb : ambientColor.rgb;
        result = rtAmbient(rt, ambient) * texColor.rgb
               + shadow * (diff * lightColor.rgb * pc.lightFlags.y * texColor.rgb);
    }

    if (!finiteVec3(result)) {
        result = texColor.rgb;
    }
    vec4 shaded = colourBlendOutput(result, texColor.rgb, texColor.a * opacity);

    // The client's fog for this blend mode: the world's fog, or toward the
    // colour that leaves the scene unchanged - black for an add, white for
    // Mod, grey for Mod2x - or none for an unfogged material.
    float dist = length(viewPos.xyz - FragPos);
    float fogFactor = pow(clamp((fogParams.y - dist) / (fogParams.y - fogParams.x), 0.0, 1.0), max(fogColor.w, 1.0));
    if (fogMode == 1) {
        // In a group of the camera's interior pass, the camera's fog colour
        // (0x007c1730 tests +0xc 0x8000).
        shaded.rgb = applyFog(shaded.rgb, FragPos, dist,
                              pc.lightFlags.x > 0.5 ? cameraFogColor.rgb : fogColor.rgb);
    } else if (fogMode == 2) {
        shaded.rgb *= fogFactor;
        if (volumetricParams.x > 0.5) shaded.rgb *= fogVolumeAt(FragPos).a;
    } else if (fogMode == 3) {
        shaded.rgb = mix(vec3(1.0), shaded.rgb, fogFactor);
    } else if (fogMode == 4) {
        shaded.rgb = mix(vec3(0.5019608), shaded.rgb, fogFactor);
    }

    outColor = shaded;
}
