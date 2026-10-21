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
    vec4 averagedDirectColor;   // light mode 2's direct light (0xd38cb0)
    vec4 averagedAmbientColor;  // and its ambient (0xd38cb4)
    vec4 windowLight;           // x: the light's window level (0xd38cdc, wmo_sidn.hpp)
    // Unread here; declared so the cascades below land where the renderer writes them.
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

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(set = 1, binding = 1) uniform WMOMaterial {
    int hasTexture;
    int alphaTest;
    int outsideLight;  // a transition batch's first pass: 0x007a8b10's mode, 0 unlit 1 outside 2 averaged 3 interior
    int insideLight;   // its second pass, or the only one
    float specularIntensity;
    int transition;    // a transition batch: its two passes blended by the output alpha (blends 9 and 7)
    int enableNormalMap;
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

layout(set = 1, binding = 2) uniform sampler2D uNormalMap;
// MOMT texture_2: what MapObjEnv and MapObjEnvMetal reflect.
layout(set = 1, binding = 3) uniform sampler2D uEnvTexture;

// The group's draw, after the vertex stage's model matrix: 1 when the group
// is drawn in the camera's interior pass (0x007a9380's local_14).
layout(push_constant) uniform GroupPush {
    layout(offset = 64) int interiorPass;
    int highlight;   // the game object highlight: the light's ambient into c29
    int interiorGroup;  // 1 for a group of the inside, wherever the camera is
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

// LOD factor from screen-space UV derivatives
float computeLodFactor() {
    vec2 dx = dFdx(TexCoord);
    vec2 dy = dFdy(TexCoord);
    float texelDensity = max(dot(dx, dx), dot(dy, dy));
    // Low density = close/head-on = full detail (0)
    // High density = far/steep = vertex normals only (1)
    return smoothstep(0.0001, 0.005, texelDensity);
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

//
// knee: the same as the traced occlusion sees it (rt_gbuffer.glsli), the
// colour as min(cap, y * ao + z) with ao over the ambient of modes 1 and 2.
// The two clamps fold into one: min(1, s min(1, x) + t) = min(1, s + t, s x + t).
struct PassKnee {
    vec3 y;
    vec3 z;
    vec3 cap;
};

vec3 passColour(int mode, vec3 v, vec3 norm, RtLight rt, out PassKnee knee) {
    knee = PassKnee(vec3(0.0), v, vec3(1.0));
    if (mode == 0) return v;
    vec3 ambient = vec3(wmoAmbientR, wmoAmbientG, wmoAmbientB);
    vec3 direct = vec3(0.0);
    // Inside, the probes' ratio is left out: a batch the camera's interior
    // pass draws with the outdoor light is one the artists lit as the open
    // air - a window, its shutters and the daylight in its panes - and the
    // probes in a lamplit room would take that light back out of it, down to
    // black (an interior's own colours are its baked light, as a unit's floor
    // colour is in character.frag). The traced occlusion stays.
    if (mode == 1) {
        ambient = gp.interiorPass != 0 ? rtAmbient(rt, ambientColor.rgb)
                                       : rtAmbient(rt, ambientColor.rgb, FragPos, norm);
        direct = lightColor.rgb;
    } else if (mode == 2) {
        ambient = gp.interiorPass != 0 ? rtAmbient(rt, averagedAmbientColor.rgb)
                                       : rtAmbient(rt, averagedAmbientColor.rgb, FragPos, norm);
        direct = averagedDirectColor.rgb;
    }
    float ndl = clamp(dot(norm, normalize(-lightDir.xyz)), 0.0, 1.0);
    vec3 light = clamp(ambient + ndl * direct, 0.0, 1.0);
    vec3 c29 = windowEmissive();
    // What the occlusion scales: the zone's ambient of modes 1 and 2, not the
    // MOHD ambient of mode 3.
    vec3 occluded = mode == 3 ? vec3(0.0) : ambient;
    vec3 rest = ndl * direct + ambient - occluded;
    knee = unifiedPath != 0 ? PassKnee(0.5 * occluded, 0.5 * rest + v + c29, min(vec3(1.0), 0.5 + v + c29))
                            : PassKnee(v * occluded, v * rest + c29, min(vec3(1.0), v + c29));
    return unifiedPath != 0 ? clamp(light * 0.5 + v + c29, 0.0, 1.0) : clamp(v * light + c29, 0.0, 1.0);
}

// The torches' and lamps' light, where the ray tracing has it, over a pass's
// colour `c` - halved, as surface() doubles that colour. A pass whose colour
// is the group's MOCV - unlit, the interior's ambient, or the unified
// programs that add it - holds the building's own lights already, baked by
// the artists' tools from the very MOLT lights traced here: it takes only
// the other lights, and the building's lights' shadows over its colour.
bool passBaked(int mode) {
    return hasVertexColors != 0 && (mode == 0 || mode == 3 || unifiedPath != 0);
}

// A baked pass of a group of a building's inside, not under the sun: a
// room's own painted light, which its lamps' traced light shades too
// (rtInteriorShade, RT_GB_INTERIOR). By the group, not by the camera's
// interior pass: gated by that, the shade came on as the camera crossed the
// door and a room seen through it from outside looked lit as painted.
bool passInterior(int mode, bool shadowedPass) {
    return passBaked(mode) && !shadowedPass && gp.interiorGroup != 0;
}

vec3 withPointLights(vec3 c, int mode, bool shadowedPass, RtLight rt) {
    if (passBaked(mode)) {
        float shade = passInterior(mode, shadowedPass) ? rtInteriorBakedShade(rt) : rtBakedShade(rt);
        return c * (rtBakedLight() * shade) + 0.5 * rtPointLightOwn(rt);
    }
    return c + 0.5 * rtPointLight(rt);
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
vec4 surfaceTexel(vec4 tex) {
    return (program == 6 && unifiedPath != 0) ? mix(texture(uEnvTexture, TexCoord2), tex, VertColor2.a) : tex;
}

vec3 surface(vec3 c, vec4 tex, vec3 env) {
    tex = surfaceTexel(tex);
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

// One pass as the traced terms will change it (rt_gbuffer.glsli): its colour
// c (passColour's, before the sun's shadow) scaled by that shadow where the
// pass is shadowed and by the lamps' shade where it is baked light, the
// occlusion inside its clamps, the point lights halved and doubled again by
// surface(), all of it through the doubling texel and the pass's fog.
RtSurface passSurface(int mode, vec3 c, PassKnee knee, bool shadowedPass, float shadowScale,
                      vec3 texel, int fogWhich, float dist) {
    RtSurface s = rtSurfaceNone();
    s.flags = RT_GB_VALID | (shadowedPass ? RT_GB_SUN : 0u) | (passBaked(mode) ? RT_GB_BAKED : 0u) |
              (passInterior(mode, shadowedPass) ? RT_GB_INTERIOR : 0u);
    // A room's baked colour: the building's lights of other storeys leave it
    // as painted (rt_trace).
    s.storeyBound = passInterior(mode, shadowedPass) ? 1u : 0u;
    float scale = (shadowedPass ? shadowScale : 1.0) * (passBaked(mode) ? rtBakedLight() : 1.0);
    s.g = c * scale;
    rtSurfaceOcclusion(s, knee.y, knee.z, knee.cap);
    s.occY *= scale;
    s.occE *= scale;
    s.kp = vec3(0.5);
    rtSurfaceScale(s, 2.0 * texel * (fogWhich == 0 ? 1.0 : fogShare(FragPos, dist)));
    return s;
}

void main() {
    float lodFactor = computeLodFactor();
    // Gradients of the UV, taken here in uniform control flow: the diffuse
    // below is sampled only for a textured batch.
    vec2 uvDx = dFdx(TexCoord);
    vec2 uvDy = dFdy(TexCoord);

    // The vertex normal as it is, on either face: the vertex programs light
    // by it and never know which side of a two-sided batch is showing.
    vec3 vertexNormal = normalize(Normal);
    // The face's own normal, on the vertex normal's side: a room's baked
    // colour takes the traced lamps by it (rtFaceBasedNormal).
    vec3 faceNormal = cross(dFdx(FragPos), dFdy(FragPos));
    faceNormal = dot(faceNormal, faceNormal) > 1e-20 ? normalize(faceNormal) : vertexNormal;
    if (dot(faceNormal, vertexNormal) < 0.0) faceNormal = -faceNormal;

    // Build TBN matrix
    vec3 T = normalize(Tangent);
    vec3 B = normalize(Bitangent);
    vec3 N = vertexNormal;
    mat3 TBN = mat3(T, B, N);

    vec4 texColor = hasTexture != 0
        ? textureFilteredGrad(uTexture, TexCoord, uvDx, uvDy, textureFilterMode(viewPos.w)) : vec4(1.0);

    // Compute normal (with normal mapping if enabled)
    vec3 norm = vertexNormal;
    if (enableNormalMap != 0 && lodFactor < 0.99 && normalMapStrength > 0.001) {
        vec3 mapNormal = textureGrad(uNormalMap, TexCoord, uvDx, uvDy).rgb * 2.0 - 1.0;
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
        float nl = dot(norm, ldir);
        float bias = max(0.0005 * (1.0 - abs(nl)), 0.00005);
        shadow = csmShadow(uShadowMap, FragPos, norm, nl, bias);
        shadow = mix(1.0, shadow, shadowParams.y);
    }
    // Drawn into the G-buffer, with the traced terms neutral (rt_lighting.glsli).
    RtLight rt = kGbuffer ? rtNeutral() : rtLightAt(FragPos);
    // The shadow variants let a face turned edge-on to the sun out of the
    // map's shadow (rtEdgeOnLift); the traced one keeps its own.
    const float mapLit = rtMapEdgeOn(shadow, norm, normalize(-lightDir.xyz));
    shadow = rtShadowEdgeOn(rt, shadow, norm, normalize(-lightDir.xyz));
    // Not here: their sat(dot(P, c3.xyz) + c3.w) lift. 0x008745d0 sets c3 to
    // 0x007bb670's plane (normal from the world matrix, through the player
    // two yards up) for groups without flags 0x48, and to zero for the rest
    // at shadow levels above 2; the M2 and terrain variants always get zero
    // (0x00874760, 0xd431ac is never written). The plane's space is not
    // pinned down, so it is left out.
    const float shadowScale = rtShadowScale(rt, shadow);

    const vec3 env = (program == 3 || program == 5) ? texture(uEnvTexture, EnvCoord).rgb : vec3(0.0);
    // The fog's distance is the view depth: every vertex program takes it
    // from the model-view matrix's z row (c33) into ((end - z) / (end - start))^exp
    // (c30, 0x00873210), not the distance to the eye.
    float dist = -(view * vec4(FragPos, 1.0)).z;

    PassKnee insideKnee;
    const vec3 insideLit = passColour(insideLight, v.rgb, norm, rt, insideKnee);
    vec3 inside = insideLit;
    if ((shadowed & 2) != 0) inside *= shadowScale;
    inside = withPointLights(inside, insideLight, (shadowed & 2) != 0, rt);
    inside = fogPass(surface(inside, texColor, env), (fogs >> 2) & 3, dist);
    RtSurface rtSurface = rtSurfaceNone();
    if (kGbuffer) {
        rtSurface = passSurface(insideLight, insideLit, insideKnee, (shadowed & 2) != 0, shadowScale,
                                surfaceTexel(texColor).rgb, (fogs >> 2) & 3, dist);
    }

    vec3 result = inside;
    if (transition != 0) {
        // A transition batch, drawn twice (0x007a9380, 0x007ac9f0): the
        // first pass times the output alpha (blend 9), then the second times
        // one minus it, added (blend 7).
        PassKnee outsideKnee;
        const vec3 outsideLit = passColour(outsideLight, v.rgb, norm, rt, outsideKnee);
        vec3 outside = outsideLit;
        if ((shadowed & 1) != 0) outside *= shadowScale;
        outside = withPointLights(outside, outsideLight, (shadowed & 1) != 0, rt);
        outside = fogPass(surface(outside, texColor, env), fogs & 3, dist);
        result = outside * alpha + inside * (1.0 - alpha);
        if (kGbuffer) {
            rtSurface = rtSurfaceMix(passSurface(outsideLight, outsideLit, outsideKnee, (shadowed & 1) != 0,
                                                 shadowScale, surfaceTexel(texColor).rgb, fogs & 3, dist),
                                     rtSurface, alpha);
        }
    }

    outColor = vec4(rtGiDebug(result), alpha);
    // A room's baked colour: lit by its face, with the bumps on it, not by
    // the smoothed vertex normal (rtFaceBasedNormal).
    rtSurface.normal = (rtSurface.flags & RT_GB_INTERIOR) != 0u
                           ? rtFaceBasedNormal(faceNormal, vertexNormal, norm) : norm;
    rtSurface.map = mapLit;
    rtWriteGbuffer(rtSurface, outColor.rgb);
}
