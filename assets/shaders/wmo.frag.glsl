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
    vec4 averagedDirectColor;   // light mode 2's direct light (0xd38cb0)
    vec4 averagedAmbientColor;  // and its ambient (0xd38cb4)
    vec4 windowLight;           // x: the light's window level (0xd38cdc, wmo_sidn.hpp)
};

#include "rt_lighting.glsli"

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(set = 1, binding = 1) uniform WMOMaterial {
    int hasTexture;
    int alphaTest;
    int outsideLight;  // a transition batch's first pass: 0x007a8b10's mode, 0 unlit 1 outside 2 averaged 3 interior
    int insideLight;   // its second pass, or the only one
    float specularIntensity;
    int transition;    // a transition batch: its two passes blended by the output alpha (blends 9 and 7)
    int enableNormalMap;
    int enablePOM;
    float pomScale;
    int pomMaxSamples;
    float heightMapVariance;
    float normalMapStrength;
    int fogs;          // per pass, bits 0-1 and 2-3: 0 none, 1 the zone's, 2 the group's inside fog
    float wmoAmbientR;
    float wmoAmbientG;
    float wmoAmbientB;
    int unifiedPath;   // MOHD flag 0x2: the MapObjU* programs (0x007a9380)
    int hasVertexColors;  // 0 none, 1 MOCV as it is (MOHD 0x8), 2 as 0x007d7380 left it
    int shadowed;      // bit 0 the first pass samples the sun's shadow map, bit 1 the second
    int program;       // the pixel program: 0 Diffuse 1 Specular 2 Metal 3 Env 4 Opaque 5 EnvMetal 6 Composite
    float alphaRef;    // the alpha test's reference by blend mode (0x00ad8b7c), 0 for none
    float sidnR;       // MOMT sidnColor where the material has flag 0x10, else 0
    float sidnG;
    float sidnB;
};

layout(set = 1, binding = 2) uniform sampler2D uNormalHeightMap;
// MOMT texture_2: what MapObjEnv and MapObjEnvMetal reflect.
layout(set = 1, binding = 3) uniform sampler2D uEnvTexture;

// The group's draw, after the vertex stage's model matrix: 1 when the group
// is drawn in the camera's interior pass (0x007a9380's local_14).
layout(push_constant) uniform GroupPush {
    layout(offset = 64) int interiorPass;
    int highlight;   // the game object highlight: the light's ambient into c29
} gp;

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec4 VertColor;
layout(location = 4) in vec3 Tangent;
layout(location = 5) in vec3 Bitangent;
layout(location = 6) in vec2 EnvCoord;
layout(location = 7) in vec2 TexCoord2;   // the second MOTV
layout(location = 8) in vec4 VertColor2;  // the second MOCV

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

// LOD factor from screen-space UV derivatives
float computeLodFactor() {
    vec2 dx = dFdx(TexCoord);
    vec2 dy = dFdy(TexCoord);
    float texelDensity = max(dot(dx, dx), dot(dy, dy));
    // Low density = close/head-on = full detail (0)
    // High density = far/steep = vertex normals only (1)
    return smoothstep(0.0001, 0.005, texelDensity);
}

// Parallax Occlusion Mapping with angle-adaptive sampling
vec2 parallaxOcclusionMap(vec2 uv, vec3 viewDirTS, float lodFactor) {
    float VdotN = abs(viewDirTS.z);  // 1=head-on, 0=grazing

    // Fade out POM at grazing angles to avoid distortion
    if (VdotN < 0.15) return uv;

    float angleFactor = clamp(VdotN, 0.15, 1.0);
    int maxS = pomMaxSamples;
    int minS = max(maxS / 4, 4);
    int numSamples = int(mix(float(minS), float(maxS), angleFactor));
    numSamples = int(mix(float(minS), float(numSamples), 1.0 - lodFactor));

    float layerDepth = 1.0 / float(numSamples);
    float currentLayerDepth = 0.0;

    // Direction to shift UV per layer - clamp denominator to prevent explosion at grazing angles
    vec2 P = viewDirTS.xy / max(VdotN, 0.15) * pomScale;
    // Hard-clamp total UV offset to prevent texture swimming
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

    // Ray march through layers
    for (int i = 0; i < 64; i++) {
        if (i >= numSamples || currentLayerDepth >= currentDepthMapValue) break;
        currentUV -= deltaUV;
        currentDepthMapValue = 1.0 - textureLod(uNormalHeightMap, currentUV, lod).a;
        currentLayerDepth += layerDepth;
    }

    // Interpolate between last two layers for smooth result
    vec2 prevUV = currentUV + deltaUV;
    float afterDepth = currentDepthMapValue - currentLayerDepth;
    float beforeDepth = (1.0 - textureLod(uNormalHeightMap, prevUV, lod).a) - currentLayerDepth + layerDepth;
    float weight = afterDepth / (afterDepth - beforeDepth + 0.0001);
    vec2 result = mix(currentUV, prevUV, weight);

    // Fade toward original UV at grazing angles for smooth transition
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

// One pass's vertex colour, as the MapObj vertex programs work it out from
// the vertex colour `v` (attrib 4). The unlit variant passes it through. The
// lit ones take the light L = clamp(ambient + clamp(N.L) x direct) of
// 0x007a8b10's mode and either multiply the vertex colour by it
// (MapObjDiffuse_T1 and the rest, 0x007ac6a0/0x007ac9f0) or add half of it
// (MapObjUDiffuse_T1 and the rest, unified, 0x007a9380: c28 is 0x7f7f7f, from
// 0x007a8940), clamped. Mode 3 is the MOHD ambient with no direct light.
//
// The lit programs add c29 as well: half the material's frameSidnColor - its
// sidnColor times the light's window level, a byte at a time (0x007a8520) -
// which 0x007ac6a0, 0x007ac9f0 and 0x007a9380 hand them (0x007a8940). It
// lights a window at night. For the WMO of the highlighted game object the
// light's ambient (0xd38cac, via 0x007964a0 / 0x007a8430) is added to it
// first, byte by byte and saturating.
vec3 windowEmissive() {
    vec3 sidn = round(vec3(sidnR, sidnG, sidnB) * 255.0);
    float level = max(roundEven(windowLight.x * 255.0 - 0.5), 0.0);
    vec3 frameSidn = floor(sidn * level / 256.0);
    if (gp.highlight != 0)
        frameSidn = min(frameSidn + round(clamp(ambientColor.rgb, 0.0, 1.0) * 255.0), vec3(255.0));
    return floor(frameSidn * 0.5) / 255.0;
}

vec3 passColour(int mode, vec3 v, vec3 norm, RtLight rt) {
    if (mode == 0) return v;
    vec3 ambient = vec3(wmoAmbientR, wmoAmbientG, wmoAmbientB);
    vec3 direct = vec3(0.0);
    if (mode == 1) {
        ambient = rtAmbient(rt, ambientColor.rgb);
        direct = lightColor.rgb;
    } else if (mode == 2) {
        ambient = rtAmbient(rt, averagedAmbientColor.rgb);
        direct = averagedDirectColor.rgb;
    }
    float ndl = clamp(dot(norm, normalize(-lightDir.xyz)), 0.0, 1.0);
    vec3 light = clamp(ambient + ndl * direct, 0.0, 1.0);
    vec3 c29 = windowEmissive();
    return unifiedPath != 0 ? clamp(light * 0.5 + v + c29, 0.0, 1.0) : clamp(v * light + c29, 0.0, 1.0);
}

// The pixel program over a pass's colour `c` (the vertex colour doubled, as
// every MapObj program has it): MapObjEnv adds the reflection by the
// texture's alpha, MapObjEnvMetal by the texture times its alpha. Specular
// and Metal draw as Opaque: their programs read no specular.
//
// MapObjComposite (MapObjUComposite, unified WMOs only - 0x007afee0 gives the
// other table none, and those are drawn as Diffuse here) lays MOMT texture_2 at the second MOTV under texture_1 by
// the second MOCV's alpha: mix(tex2, tex1, secondary.a), its alpha the same
// way, and doubles it over the colour as the others do.
vec3 surface(vec3 c, vec4 tex, vec3 env) {
    if (program == 6 && unifiedPath != 0) tex = mix(texture(uEnvTexture, TexCoord2), tex, VertColor2.a);
    vec3 rgb = 2.0 * c * tex.rgb;
    if (program == 3) rgb += tex.a * env;
    else if (program == 5) rgb += tex.rgb * tex.a * env;
    return rgb;
}

// A pass's fog: `which` 0 none, 1 the zone's, 2 the group's inside fog - the
// camera's in the camera's interior pass, the zone's otherwise (0x007a8440).
vec3 fogPass(vec3 color, int which, float dist) {
    if (which == 0) return color;
    vec3 colour = (which == 2 && gp.interiorPass != 0) ? cameraFogColor.rgb : fogColor.rgb;
    return applyFog(color, FragPos, dist, colour);
}

void main() {
    float lodFactor = computeLodFactor();
    // Gradients of the authored UV, taken here where every pixel of the quad
    // still agrees on them. Parallax moves the UV by a different amount per
    // pixel, and a mip level chosen from the moved UV flickers.
    vec2 uvDx = dFdx(TexCoord);
    vec2 uvDy = dFdy(TexCoord);

    // The vertex normal as it is, on either face: the vertex programs light
    // by it and never know which side of a two-sided batch is showing.
    vec3 vertexNormal = normalize(Normal);

    // Compute final UV (with POM if enabled)
    vec2 finalUV = TexCoord;

    // Build TBN matrix
    vec3 T = normalize(Tangent);
    vec3 B = normalize(Bitangent);
    vec3 N = vertexNormal;
    mat3 TBN = mat3(T, B, N);

    if (enablePOM != 0 && heightMapVariance > 0.001 && lodFactor < 0.99) {
        mat3 TBN_inv = transpose(TBN);
        vec3 viewDirWorld = normalize(viewPos.xyz - FragPos);
        vec3 viewDirTS = TBN_inv * viewDirWorld;
        finalUV = parallaxOcclusionMap(TexCoord, viewDirTS, lodFactor);
    }

    vec4 texColor = hasTexture != 0 ? textureGrad(uTexture, finalUV, uvDx, uvDy) : vec4(1.0);

    // Compute normal (with normal mapping if enabled)
    vec3 norm = vertexNormal;
    if (enableNormalMap != 0 && lodFactor < 0.99 && normalMapStrength > 0.001) {
        vec3 mapNormal = textureGrad(uNormalHeightMap, finalUV, uvDx, uvDy).rgb * 2.0 - 1.0;
        mapNormal = normalize(mapNormal);
        vec3 worldNormal = normalize(TBN * mapNormal);
        // Linear blend: strength controls how much normal map detail shows,
        // LOD fades out at distance. Both multiply for smooth falloff.
        float blend = clamp(normalMapStrength, 0.0, 1.0) * (1.0 - lodFactor);
        norm = normalize(mix(vertexNormal, worldNormal, blend));
    }

    // The vertex colour the programs read: the MOCV as loaded, or for a
    // group without one 0x7f7f7f, black in a unified WMO, alpha 255
    // (0x007c8560).
    vec4 v = hasVertexColors != 0 ? VertColor
           : vec4(vec3(unifiedPath != 0 ? 0.0 : 127.0 / 255.0), 1.0);

    // The output alpha: the vertex alpha, times the texture's for
    // MapObjDiffuse alone. It is what the alpha test reads and what blends a
    // transition batch's two passes.
    float alpha = v.a * (program == 0 ? texColor.a
                       : (program == 6 && unifiedPath != 0) ? mix(texture(uEnvTexture, TexCoord2).a, texColor.a, VertColor2.a)
                       : 1.0);
    if (transition == 0 && alphaRef > 0.0 && alpha < alphaRef) discard;

    // The sun's shadow map, where the pass samples it: the shadow variants of
    // the pixel programs scale the colour by 0.7 + 0.3 x the light let
    // through. A pass that does not binds a blank map (0x008745d0(1)).
    float shadow = 1.0;
    if (shadowParams.x > 0.5 && shadowed != 0) {
        vec3 ldir = normalize(-lightDir.xyz);
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
    // The shadow variants let a face turned edge-on to the sun out of the
    // shadow: by |1.2 - |N.L||^4, saturated (c4 is the light's view-space
    // direction, 0x00875c10).
    // Not here: their sat(dot(P, c3.xyz) + c3.w) lift. 0x008745d0 sets c3 to
    // 0x007bb670's plane (normal from the world matrix, through the player
    // two yards up) for groups without flags 0x48, and to zero for the rest
    // at shadow levels above 2; the M2 and terrain variants always get zero
    // (0x00874760, 0xd431ac is never written). The plane's space is not
    // pinned down, so it is left out.
    shadow += (1.0 - shadow) * clamp(pow(abs(1.2 - abs(dot(norm, normalize(-lightDir.xyz)))), 4.0), 0.0, 1.0);
    const float shadowScale = 0.7 + 0.3 * shadow;

    const vec3 env = (program == 3 || program == 5) ? texture(uEnvTexture, EnvCoord).rgb : vec3(0.0);
    // The fog's distance is the view depth: every vertex program takes it
    // from the model-view matrix's z row (c33) into ((end - z) / (end - start))^exp
    // (c30, 0x00873210), not the distance to the eye.
    float dist = -(view * vec4(FragPos, 1.0)).z;

    vec3 inside = passColour(insideLight, v.rgb, norm, rt);
    if ((shadowed & 2) != 0) inside *= shadowScale;
    inside = fogPass(surface(inside, texColor, env), (fogs >> 2) & 3, dist);

    vec3 result = inside;
    if (transition != 0) {
        // A transition batch, drawn twice (0x007a9380, 0x007ac9f0): the
        // first pass times the output alpha (blend 9), then the second times
        // one minus it, added (blend 7).
        vec3 outside = passColour(outsideLight, v.rgb, norm, rt);
        if ((shadowed & 1) != 0) outside *= shadowScale;
        outside = fogPass(surface(outside, texColor, env), fogs & 3, dist);
        result = outside * alpha + inside * (1.0 - alpha);
    }

    outColor = vec4(result, alpha);
}
