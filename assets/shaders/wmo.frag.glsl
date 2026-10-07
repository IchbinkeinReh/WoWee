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

layout(set = 1, binding = 1) uniform WMOMaterial {
    int hasTexture;
    int alphaTest;
    int unlit;
    int isInterior;
    float specularIntensity;
    int transition;    // a transition batch: the outside and inside light blended by the vertex alpha
    int enableNormalMap;
    int enablePOM;
    float pomScale;
    int pomMaxSamples;
    float heightMapVariance;
    float normalMapStrength;
    int unfogged;     // a transition batch whose material has F_UNFOGGED: no fog
    float wmoAmbientR;
    float wmoAmbientG;
    float wmoAmbientB;
    int unused64;      // unused; was a per-texture-name emissive mode
    int hasVertexColors;  // 0 none, 1 MOCV as it is (MOHD 0x8), 2 as 0x007d7380 left it
    int padding1;
    int padding2;
};

layout(set = 1, binding = 2) uniform sampler2D uNormalHeightMap;

// The group's draw, after the vertex stage's model matrix: 1 when the group
// is drawn in the camera's interior pass (0x007a9380's local_14).
layout(push_constant) uniform GroupPush {
    layout(offset = 64) int interiorPass;
} gp;

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec4 VertColor;
layout(location = 4) in vec3 Tangent;
layout(location = 5) in vec3 Bitangent;

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

void main() {
    float lodFactor = computeLodFactor();
    // Gradients of the authored UV, taken here where every pixel of the quad
    // still agrees on them. Parallax moves the UV by a different amount per
    // pixel, and a mip level chosen from the moved UV flickers.
    vec2 uvDx = dFdx(TexCoord);
    vec2 uvDy = dFdy(TexCoord);

    vec3 vertexNormal = normalize(Normal);
    if (!gl_FrontFacing) vertexNormal = -vertexNormal;

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
    if (alphaTest != 0 && texColor.a < 0.5) discard;

    // Compute normal (with normal mapping if enabled)
    vec3 norm = vertexNormal;
    if (enableNormalMap != 0 && lodFactor < 0.99 && normalMapStrength > 0.001) {
        vec3 mapNormal = textureGrad(uNormalHeightMap, finalUV, uvDx, uvDy).rgb * 2.0 - 1.0;
        mapNormal = normalize(mapNormal);
        vec3 worldNormal = normalize(TBN * mapNormal);
        if (!gl_FrontFacing) worldNormal = -worldNormal;
        // Linear blend: strength controls how much normal map detail shows,
        // LOD fades out at distance. Both multiply for smooth falloff.
        float blend = clamp(normalMapStrength, 0.0, 1.0) * (1.0 - lodFactor);
        norm = normalize(mix(vertexNormal, worldNormal, blend));
    }

    vec3 result;

    // Sample shadow map for all groups.  Interior groups receive attenuated
    // shadow (30%) so they get subtle light/shadow variation without the full
    // outdoor darkening that makes them look wrong.
    float shadow = 1.0;
    if (shadowParams.x > 0.5) {
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

    // Windows, lamp glass and clock faces are lit like any other surface:
    // the client draws a batch by its material's blend mode and flags.
    //
    // The vertex colour on the scale of the MOCV: 0x007d7380 halved it when
    // the group was loaded (and brightened it by its alpha, off the
    // transition batches), so twice what it left.
    vec3 mocv = VertColor.rgb * (hasVertexColors == 2 ? 2.0 : 1.0);

    // WMO interior: vertex colors (MOCV) are pre-baked lighting from the
    // artist, floored by the root's MOHD ambient. No sun and no sun
    // shadow: the client lights an interior pass with the MOHD ambient
    // and a black direct light (Wow.exe 3.3.5a 0x007a8b10, mode 3). The
    // 0.15 floor and the shadow clamp that were here are not the
    // client's. How the two colours combine is in the MapObj shaders in
    // the MPQs, not in Wow.exe; the max() is WoWee's reading.
    vec3 wmoAmbient = vec3(wmoAmbientR, wmoAmbientG, wmoAmbientB);
    vec3 inside = texColor.rgb * max(mocv, wmoAmbient);

    vec3 outside;
    if (unlit != 0) {
        // Outdoor unlit surface - still receives directional shadows
        outside = texColor.rgb * shadow;
    } else {
        vec3 ldir = normalize(-lightDir.xyz);
        float diff = max(dot(norm, ldir), 0.0);

        // No specular: MapObjSpecular is used only with the client's
        // 'specular' option, which is off by default.
        outside = rtAmbient(rt, ambientColor.rgb) * texColor.rgb
                + shadow * diff * lightColor.rgb * texColor.rgb;

        // An exterior group's vertex colour is light baked into it - a lamp's
        // pool on a wall - and it adds to the sun, as it does in the client.
        // It was multiplied in as if it were baked shadow, floored at a
        // quarter: Stormwind has two exterior groups with vertex colours at
        // all, both near black, and they drew at a quarter of the light the
        // rest of the city had, shadows on or off - and so did 278 of the 362
        // exterior groups in the game that carry any. A group without them has
        // white filled in by the loader, which must add nothing.
        //
        // At half: a handful of Icecrown exteriors ship them pure white, and
        // added at full those would double in brightness.
        if (hasVertexColors != 0) outside += texColor.rgb * mocv * 0.5;
    }

    float dist = length(viewPos.xyz - FragPos);
    // The camera's fog colour for an interior group in the camera's interior
    // pass, the zone's for the rest (0x007a9380 by way of 0x007a8440).
    vec3 insideFog = gp.interiorPass != 0 ? cameraFogColor.rgb : fogColor.rgb;
    if (transition != 0) {
        // A transition batch, drawn twice by the client (0x007a9380): by the
        // outside light in the zone's fog, colour times alpha (blend 9), then
        // by the interior light in the group's fog, colour times one minus
        // alpha, added (blend 7). The alpha is the vertex colour's, which
        // 0x007d7380 kept on those vertices. F_UNFOGGED takes the fog off both.
        if (unfogged == 0) {
            outside = applyFog(outside, FragPos, dist, fogColor.rgb);
            inside = applyFog(inside, FragPos, dist, insideFog);
        }
        result = outside * VertColor.a + inside * (1.0 - VertColor.a);
    } else if (isInterior != 0) {
        result = applyFog(inside, FragPos, dist, insideFog);
    } else {
        result = applyFog(outside, FragPos, dist, fogColor.rgb);
    }

    outColor = vec4(result, texColor.a);
}
