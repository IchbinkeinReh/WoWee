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
    vec4 rtParams;
    vec4 cameraFogColor;  // the camera's fog, its interior's blended in
    // Unread here; declared so the cascades below land where the renderer writes them.
    vec4 averagedDirectColor;
    vec4 averagedAmbientColor;
    vec4 windowLight;
    vec4 specularColor;
    // The shadow cascades (shadow_csm.glsli); GPUPerFrameData ends with them.
    mat4 cascadeMatrix[4];
    vec4 cascadeRect[4];
    vec4 cascadeTexel[4];
    vec4 cascadeInfo;
    // The probe volume (rt_lighting.glsli); GPUPerFrameData ends with it.
    vec4 giVolume[2];
    vec4 giVolumeMod[2];
    vec4 giSky;
    vec4 giGround;
    vec4 giParams;
    vec4 rtTune;
    vec4 lightTune;
    vec4 rtTune2;
};

#include "rt_lighting.glsli"
#include "texture_filter.glsli"
#include "shadow_csm.glsli"
#include "model_light.glsli"

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(set = 1, binding = 1) uniform CharMaterial {
    float opacity;
    int alphaTest;
    // The whole unit's alpha (its own, a kit's, the camera's): what a Mod or
    // Mod2x layer fades by (colourBlendOutput). Was the black colour key.
    float instanceFade;
    int unlit;
    float emissiveBoost;
    // Keep these as scalar floats to match the C++ UBO packing. A std140 vec3
    // would insert padding here and shift the following material flags.
    float emissiveTintR;
    float emissiveTintG;
    float emissiveTintB;
    float specularIntensity;
    int enableNormalMap;
    // 1: the character preview's main model, drawn with its texture alone.
    int previewSimpleTexture;
    float normalMapStrength;
    int hairMaterial;
    // 0 normal; 1 Mod and 2 Mod2x, faded toward their neutral 1.0 and 0.5 by
    // the unit's alpha; 3 NoAlphaAdd, scaled by opacity alone. See CharMaterialUBO.
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
    int combiners;     // stage modes in bits 0-3 and 4-7, stage count in 8-9 (as m2.frag)
    int coordSources;  // per stage: 0 UV0, 1 UV1, 2 env, 3 UV0 untransformed
    float tintR;       // the batch's colour track
    float tintG;
    float tintB;
    float uv2M00;      // the second stage's texture matrix
    float uv2M01;
    float uv2M10;
    float uv2M11;
    float uv2Tx;
    float uv2Ty;
};

layout(set = 1, binding = 3) uniform sampler2D uTexture2;

// The client's texture stages, as its Combiners_*.bls pixel programs apply
// them (see m2.frag.glsl): each stage to the running colour, the diffuse
// first, by the colour op (0x00af5a08) and alpha op (0x00af59e8) of its mode.
const int kCombinerColorOp[8] = int[8](0, 0, 4, 2, 1, 5, 1, 2);
const int kCombinerAlphaOp[8] = int[8](3, 0, 3, 2, 1, 3, 3, 3);


// What a colour-only blend (Mod, Mod2x, NoAlphaAdd) is handed. The 3.3.5a
// client (FUN_0081fe90) draws Mod and Mod2x unlit, with no diffuse and a
// constant emissive of 1.0 and 0.5: the texture, or half of it, whatever the
// alpha - Mod2x then doubles it back, so its net effect is dst * texture.
// NoAlphaAdd's factors ignore alpha, so its fade is in the colour.
//
// The unit's own alpha is another matter. The client fades a unit as one
// model, what it carries with it (0x00737390 into the model's alpha, an
// attached model's multiplied by its parent's, 0x0082e140), and the layer
// fades with it. A multiply has no alpha in its factors, so the fade goes in
// the colour: toward 1 (Mod) or 0.5 (Mod2x), which leave the scene as it is.
// Without it a staff's Mod2x reflection layer went on multiplying the room
// behind at full strength as the body faded around it - the staff's outline
// in black where the camera closes in.
vec4 colourBlendOutput(vec3 lit, vec3 tex, float alpha) {
    if (colourBlend == 1) return vec4(mix(vec3(1.0), tex, instanceFade), alpha);
    if (colourBlend == 2) return vec4(mix(vec3(0.5), tex * 0.5, instanceFade), alpha);
    if (colourBlend == 3) return vec4(lit * opacity, alpha);
    return vec4(lit, alpha);
}

// The 3.3.5a client's alpha-key reference (FUN_0081fe90): 224/255 of the
// batch's alpha, against an output alpha that carries the same batch alpha -
// so the texture's alpha against 224/255.
const float ALPHA_KEY_REF = 0.8784314;

layout(set = 1, binding = 2) uniform sampler2D uNormalMap;

vec3 combineRgb(int op, vec3 cur, float curA, vec3 tex, float diffA) {
    if (op == 0) return tex * cur;
    if (op == 1) return tex * cur * 2.0;
    if (op == 2) return tex + cur;
    if (op == 4) return cur * curA + tex * (1.0 - curA);
    if (op == 5) return tex * diffA + cur * (1.0 - diffA);
    return cur;
}

float combineA(int op, float cur, float tex, float diffA) {
    if (op == 0) return tex * cur;
    if (op == 1) return tex * cur * 2.0;
    if (op == 2) return tex + cur;
    if (op == 5) return tex * diffA + cur * (1.0 - diffA);
    return cur;
}

// The batch's textures over a diffuse of `d`, stage by stage.
vec4 combineStages(vec4 d, vec4 t0, vec4 t1) {
    int m0 = combiners & 7;
    vec4 cur = vec4(combineRgb(kCombinerColorOp[m0], d.rgb, d.a, t0.rgb, d.a),
                    combineA(kCombinerAlphaOp[m0], d.a, t0.a, d.a));
    if (((combiners >> 8) & 3) > 1) {
        int m1 = (combiners >> 4) & 15;
        if (m1 == 8) {
            // Combiners_Opaque_Mod2xNA_Alpha (0x8001): the second texture
            // doubled over the first where the first's alpha is clear.
            cur.rgb = mix(cur.rgb * t1.rgb * 2.0, cur.rgb, t0.a);
        } else if (m1 == 9) {
            // Combiners_Opaque_AddAlpha (0x8002): the second added by its alpha.
            cur.rgb += t1.rgb * t1.a;
        } else if (m1 == 10) {
            // Combiners_Opaque_AddAlpha_Alpha (0x8003): the same, lit, and only
            // where the first texture's alpha is clear.
            cur.rgb += t1.rgb * t1.a * (1.0 - t0.a) * d.rgb;
        } else {
            m1 &= 7;
            cur = vec4(combineRgb(kCombinerColorOp[m1], cur.rgb, cur.a, t1.rgb, d.a),
                       combineA(kCombinerAlphaOp[m1], cur.a, t1.a, d.a));
        }
    }
    return cur;
}


// After the vertex stage's model matrix: the light of the WMO interior floor
// the character stands on, w = 1 when it has one (0x007a0d60, 0x007c1730).
layout(push_constant) uniform Push {
    layout(offset = 64) vec4 interiorAmbient;
    vec4 interiorDirect;
    // x: 1 in a WMO group of the camera's interior pass, y: the scale on the
    // direct light, z: the model's own depth in yards, within which it does
    // not receive the sun's shadow (CharPushConstants::lightFlags).
    vec4 lightFlags;
    // The unit's colour, which its direct light is multiplied by (CM2Model
    // +0x180: a spell kit's, 0x00720db0); white for none.
    vec4 diffuseColour;
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
layout(location = 5) in vec2 inTexCoord2;
layout(location = 6) in vec2 inEnvCoord;
// The second stage's coordinate, set with TexCoord.
vec2 TexCoord2;

layout(location = 0) out vec4 outColor;

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

// How much of a colour applyFog lets through: it takes c to c * fogShare + the
// fog's own light. The ray traced lighting's G-buffer scales what it writes
// by it (rtSurfaceScale).
float fogShare(vec3 worldPos, float dist) {
    float share = pow(clamp((fogParams.y - dist) / (fogParams.y - fogParams.x), 0.0, 1.0), max(fogColor.w, 1.0));
    if (volumetricParams.x > 0.5) share *= fogVolumeAt(worldPos).a;
    return share;
}

void main() {
    // In a G-buffer pipeline every fragment writes the G-buffer; what is not
    // lit as a surface of the tracing's writes none (rt_lighting.glsli).
    rtWriteGbuffer(rtSurfaceNone(), vec3(0.0));
    // Each stage's coordinates (0x00836600): a UV set through the stage's
    // matrix, or the sphere map.
    const int src0 = coordSources & 3;
    const int src1 = (coordSources >> 2) & 3;
    const vec2 set0 = src0 == 1 ? inTexCoord2 : inTexCoord;
    const vec2 set1 = src1 == 1 ? inTexCoord2 : inTexCoord;
    TexCoord = src0 == 2 ? inEnvCoord
             : vec2(uvM00 * set0.x + uvM01 * set0.y + uvTx, uvM10 * set0.x + uvM11 * set0.y + uvTy);
    TexCoord2 = src1 == 2 ? inEnvCoord
              : src1 == 3 ? inTexCoord
              : vec2(uv2M00 * set1.x + uv2M01 * set1.y + uv2Tx, uv2M10 * set1.x + uv2M11 * set1.y + uv2Ty);
    if (previewSimpleTexture != 0) {
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
    // Gradients of the UV, taken here before anything is discarded.
    vec2 uvDx = dFdx(TexCoord);
    vec2 uvDy = dFdy(TexCoord);

    // The vertex normal as it is, on either face: the client lights a model
    // per vertex (the Diffuse_* programs), which never knows which side of a
    // two-sided batch is showing.
    vec3 vertexNormal = safeNormalize(Normal, vec3(0.0, 0.0, 1.0));

    bool useNormalMap = enableNormalMap != 0 &&
                        unlit == 0 &&
                        lodFactor < 0.99 &&
                        normalMapStrength > 0.001;
    mat3 TBN;
    if (useNormalMap) {
        vec3 T = safeNormalize(Tangent, fallbackTangent(vertexNormal));
        T = safeNormalize(T - dot(T, vertexNormal) * vertexNormal, fallbackTangent(vertexNormal));
        vec3 B = safeNormalize(Bitangent, safeNormalize(cross(vertexNormal, T), vec3(0.0, 1.0, 0.0)));
        TBN = mat3(T, B, vertexNormal);
    }

    const int filterMode = textureFilterMode(viewPos.w);
    const vec4 tex0 = textureFilteredGrad(uTexture, TexCoord, uvDx, uvDy, filterMode);
    const vec4 tex1 = ((combiners >> 8) & 3) > 1 ? textureFiltered(uTexture2, TexCoord2, filterMode) : vec4(1.0);
    // The stages' alpha over the batch's (opacity), and the same divided back
    // out: what the alpha reference, 224/255 of the batch's alpha, is
    // measured against. An Opaque stage keeps the diffuse's alpha.
    const float stageAlpha = combineStages(vec4(1.0, 1.0, 1.0, opacity), tex0, tex1).a;
    vec4 texColor = vec4(tex0.rgb, stageAlpha / max(opacity, 1e-6));

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
        vec3 mapNormal = textureGrad(uNormalMap, TexCoord, uvDx, uvDy).rgb * 2.0 - 1.0;
        mapNormal.xy *= normalMapStrength;
        mapNormal = safeNormalize(mapNormal, vec3(0.0, 0.0, 1.0));
        vec3 worldNormal = safeNormalize(TBN * mapNormal, vertexNormal);
        float blendFactor = max(lodFactor, 1.0 - normalMapStrength);
        norm = safeNormalize(mix(worldNormal, vertexNormal, blendFactor), vertexNormal);
    }

    // The light the diffuse carries, which the stages then combine with, and
    // how the traced terms will change it (rt_gbuffer.glsli), in its units.
    vec3 light;
    RtSurface rtSurface = rtSurfaceNone();

    if (unlit != 0) {
        // No light: the client draws an unlit batch with its colour as the
        // emissive and nothing else (FUN_0081fb10).
        light = vec3(1.0);
    } else {
        // Drawn into the G-buffer with the traced terms neutral; drawn after
        // the trace - see-through, or a blended batch - with what it finds
        // of itself in this frame's result, or what stands behind it
        // (rtLightBehind): a unit drawn see-through is in no G-buffer.
        RtLight rt = kGbuffer ? rtNeutral() : rtLightBehind(FragPos);
        // Its light as refreshInteriorLights samples it at its feet, as a
        // game object's (model_light.glsli): w 2 on an interior floor, 1 the
        // zone's light eased in, 0 not known yet (the zone's as it is). The
        // direct light by its scale (halved in the terrain's baked shadow,
        // 0x007c1730 multiplies by +0x8c) and the unit's colour (CM2Model
        // +0x180).
        const bool known = pc.interiorAmbient.w > 0.5;
        const vec3 direct = (known ? pc.interiorDirect.rgb * pc.lightFlags.y : lightColor.rgb) * pc.diffuseColour.rgb;
        if (pc.interiorAmbient.w > 1.5) {
            light = modelBakedLight(rt, norm, pc.interiorAmbient.rgb, direct, pc.interiorDirect.w, true, rtSurface);
        } else {
            float shadow = 1.0;
            if (shadowParams.x > 0.5) {
                // The interpolated vertex normal, not the normal-mapped one, for
                // the normal offset: a map's per-texel tilt moves the sample
                // point about and speckles the result.
                vec3 ldir = normalize(-lightDir.xyz);
                float nl = dot(vertexNormal, ldir);
                float bias = max(0.0005 * (1.0 - abs(nl)), 0.00005);
                // No shadow from the model's own body. With the near cascade at
                // three or four hundredths of a yard a texel, the normal offset
                // no longer carries a fragment out of its own limb as the single
                // 0.29-yard map's did, and a constant bias of about a tenth of a
                // yard is near an arm's or a shin's thickness: blotches where the
                // body is a little thicker, none where it is thinner. The client
                // never self-shadows a model, so the bias is the model's whole
                // depth along the ray (lightFlags.z yards, cascadeInfo.w a yard in
                // depth units); only what is farther toward the sun than that -
                // a roof, a tree, a cliff - shadows a character.
                bias = max(bias, pc.lightFlags.z * cascadeInfo.w);
                shadow = csmShadow(uShadowMap, FragPos, vertexNormal, nl, bias);
                shadow = mix(1.0, shadow, shadowParams.y);
            }
            const vec3 ambient = known ? pc.interiorAmbient.rgb : ambientColor.rgb;
            light = modelZoneLight(rt, norm, FragPos, shadow, ambient, direct, rtSurface);
        }
    }

    // The batch's colour times the light is the diffuse the stages combine
    // over (0x0081fe90).
    vec3 result = combineStages(vec4(light * vec3(tintR, tintG, tintB), opacity), tex0, tex1).rgb;
    // The stages are affine in the diffuse: what multiplies the light on its
    // way through them multiplies the traced terms' part of it too, and so
    // does a NoAlphaAdd's opacity. Mod and Mod2x are drawn unlit.
    if (kGbuffer) {
        rtSurface.normal = norm;
        rtSurfaceScale(rtSurface, combineStages(vec4(vec3(tintR, tintG, tintB), opacity), tex0, tex1).rgb -
                                  combineStages(vec4(vec3(0.0), opacity), tex0, tex1).rgb);
        if (colourBlend == 3) rtSurfaceScale(rtSurface, vec3(opacity));
        if (colourBlend == 1 || colourBlend == 2 || !finiteVec3(result)) rtSurface = rtSurfaceNone();
    }
    if (!finiteVec3(result)) {
        result = texColor.rgb;
    }
    // A multiply is drawn unlit, over a diffuse of 1 (Mod) or 0.5 (Mod2x).
    vec3 unlitStages = combineStages(vec4(1.0, 1.0, 1.0, opacity), tex0, tex1).rgb;
    vec4 shaded = colourBlendOutput(result, unlitStages, texColor.a * opacity);

    // The client's fog for this blend mode: the world's fog, or toward the
    // colour that leaves the scene unchanged - black for an add, white for
    // Mod, grey for Mod2x - or none for an unfogged material.
    // The fog's distance is the view depth: every vertex program takes it
    // from the model-view matrix's z row (c33) into ((end - z) / (end - start))^exp
    // (c30, 0x00873210), not the distance to the eye.
    float dist = -(view * vec4(FragPos, 1.0)).z;
    float fogFactor = pow(clamp((fogParams.y - dist) / (fogParams.y - fogParams.x), 0.0, 1.0), max(fogColor.w, 1.0));
    if (kGbuffer && (fogMode == 1 || fogMode == 2)) rtSurfaceScale(rtSurface, vec3(fogShare(FragPos, dist)));
    if (kGbuffer && (fogMode == 3 || fogMode == 4)) rtSurfaceScale(rtSurface, vec3(fogFactor));
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

    shaded.rgb = rtGiDebug(shaded.rgb);
    outColor = shaded;
    rtWriteGbuffer(rtSurface, outColor.rgb);
}
