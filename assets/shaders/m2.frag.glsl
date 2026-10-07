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

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(set = 1, binding = 2) uniform M2Material {
    int hasTexture;
    int alphaTest;
    int unused0;       // was the black colour key: the client never keys by colour
    float unused1;     // was the colour key's threshold
    int unlit;
    int blendMode;
    float fadeAlpha;
    float unused4;     // was interiorDarken, always 0: the client has no such darkening
    float specularIntensity;
    float emissiveBoost;
    float tintR;
    float tintG;
    float tintB;
    int unused3;       // was a WoWee-only light-beam softening; kept for the layout
    int unused2;       // was a WoWee-only fire-card fade; kept for the layout
    int unfogged;      // material flag 0x2: the client draws it without fog
};

// The 3.3.5a client's M2 blends: 3 NoAlphaAdd and 4 Add add to the scene,
// 5 Mod and 6 Mod2x multiply it, 7 is plain alpha. A multiply's factors
// ignore alpha, and white (Mod) or mid-grey (Mod2x) leaves the scene as it is.
bool blendAdds() { return blendMode == 3 || blendMode == 4; }
bool blendMultiplies() { return blendMode == 5 || blendMode == 6; }

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 5) in float vFadeAlpha;
layout(location = 6) flat in int vSkyMode;
layout(location = 7) flat in float vHighlight;
layout(location = 8) flat in vec4 vColorMul;
layout(location = 9) flat in int vInteriorLit;
layout(location = 10) flat in vec3 vInteriorAmbient;
layout(location = 11) flat in vec3 vInteriorDirect;

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

// 4x4 Bayer dither matrix (normalized to 0..1)
float bayerDither4x4(ivec2 p) {
    int idx = (p.x & 3) + (p.y & 3) * 4;
    float m[16] = float[16](
         0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0,
        12.0/16.0,  4.0/16.0, 14.0/16.0,  6.0/16.0,
         3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0,
        15.0/16.0,  7.0/16.0, 13.0/16.0,  5.0/16.0
    );
    return m[idx];
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
vec3 applyFog(vec3 color, vec3 worldPos, float dist) {
    // Raised to fogColor.w: 1 before map 530, the later fog's exponent after (0x00873210).
    float fogFactor = pow(clamp((fogParams.y - dist) / (fogParams.y - fogParams.x), 0.0, 1.0), max(fogColor.w, 1.0));
    color = mix(fogColor.rgb, color, fogFactor);
    if (volumetricParams.x > 0.5) {
        vec4 air = fogVolumeAt(worldPos);
        color = color * air.a + air.rgb;
    }
    return color;
}

// The whole depth of the fog volume at this point of the screen, for the sky:
// it lies behind everything, so it takes all the air there is.
vec4 fogVolumeSky(vec2 uv) {
    return textureLod(uFogVolume, vec3(uv, 1.0), 0.0);
}

void main() {
    vec4 texColor = hasTexture != 0 ? texture(uTexture, TexCoord) : vec4(1.0);
    // A multiply takes the texture alone: the client's colour goes in through
    // the diffuse, and a multiply has none.
    const vec3 rawTexRgb = texColor.rgb;
    // The batch's authored colour. A glow card is painted white and coloured
    // here - Orgrimmar's bonfire carries (1.0, 0.329, 0.0) - so without it
    // every fire in the world burns white.
    //
    // The client takes the batch's colour and alpha every draw (FUN_0081fe90),
    // so a track that moves is sampled per frame and handed in per instance;
    // otherwise the material carries its one value.
    const bool batchAnimated = vColorMul.a >= 0.0;
    texColor.rgb *= batchAnimated ? vColorMul.rgb : vec3(tintR, tintG, tintB);
    // The distance fade and the batch's own alpha together: what a blended
    // batch is drawn at.
    const float batchFade = vFadeAlpha * (batchAnimated ? vColorMul.a : fadeAlpha);
    // A batch whose alpha has run to zero is not drawn, whatever it blends
    // as: that is how a model hides a part for the length of a sequence.
    if (batchAnimated && vColorMul.a < 1.0 / 255.0) discard;

    // Original client sky M2s carry their authored colour and alpha, and are
    // taken as they are. They are camera-centered and unlit, and must not be
    // swallowed by world-distance fog.
    //
    // This used to sit below the three discards, which meant the sky was not
    // taken as it is. The alpha test in particular rescales alpha by its own
    // screen-space derivative:
    //
    //     float aGrad = fwidth(texColor.a);
    //     texColor.a = clamp((texColor.a - alphaCutoff) / max(aGrad, 0.001) ...
    //
    // fwidth is how fast alpha changes from one pixel to the next, so it
    // changes whenever the view does - and a nebula's alpha ramp is gentle,
    // which makes the divisor tiny and the result a hard edge. Turning the
    // camera moved that edge, so Hellfire's sky flickered while the view moved
    // and stood still when it did not, on its blended layers alone. The rescale
    // is for foliage cutouts, where a hard edge is the point.
    if (vSkyMode != 0) {
        // An opaque layer's alpha channel says nothing - it was never
        // blended - so only the fade is its alpha. That matters while the
        // sky crossfades between zones, when every layer is drawn blended.
        float skyAlpha = (blendMode == 0) ? 1.0 : texColor.a;
        // Behind all the air there is, as the procedural sky is. An
        // additive layer only loses what the air hides of it; the air's own
        // light is already in the layer it is added to.
        vec3 skyColor = texColor.rgb;
        if (volumetricParams.x > 0.5) {
            vec4 clip = projection * view * vec4(FragPos, 1.0);
            vec4 air = fogVolumeSky(clip.xy / max(clip.w, 1e-4) * 0.5 + 0.5);
            skyColor = (blendAdds()) ? skyColor * air.a : skyColor * air.a + air.rgb;
        }
        if (blendMultiplies()) skyColor = rawTexRgb * (blendMode == 5 ? 1.0 : 0.5);
        outColor = vec4(skyColor, skyAlpha * batchFade);
        return;
    }

    // The client's alpha reference (FUN_0081fe90): 224/255 of the batch's
    // alpha for an alpha key, which against an output alpha of texture times
    // that same batch alpha is the texture's alpha against 224/255, for every
    // model alike: the client draws a batch by its material.
    const float alphaCutoff = 0.8784314;
    if (alphaTest != 0) {
        // Screen-space sharpened alpha: rescale so the cutoff maps to the
        // texel boundary. With MSAA + alpha-to-coverage on the cutout
        // pipeline this dithers the edge band across samples, smoothing
        // leaf silhouettes instead of the old hard binary discard.
        float aGrad = fwidth(texColor.a);
        texColor.a = clamp((texColor.a - alphaCutoff) / max(aGrad, 0.001) * 0.5 + 0.5, 0.0, 1.0);
        if (texColor.a < 1.0 / 255.0) discard;
    } else if (blendMode >= 2 && texColor.a * batchFade < 1.0 / 255.0) {
        // Every blended mode is alpha tested at 1/255 in the client: what is
        // fully transparent is not drawn at all, so it writes no depth.
        // Nothing is discarded for being dark: the client draws the batch by
        // its blend mode and never keys by colour.
        discard;
    }
    if (blendMode == 1 && texColor.a < 0.004) discard;

    // The vertex normal as it is, on either face: the client lights M2s per
    // vertex with the fixed-function light (FUN_0081fb10), which never knows
    // which side of a two-sided batch is showing.
    vec3 norm = normalize(Normal);

    vec3 ldir = normalize(-lightDir.xyz);
    float diff = max(dot(norm, ldir), 0.0);

    vec3 result;
    if (unlit != 0) {
        // The texture as it is, with the batch colour already in it: the
        // client draws an unlit batch with no light and adds nothing
        // (FUN_0081fb10).
        result = texColor.rgb;
    } else if (vInteriorLit != 0) {
        // A doodad of a WMO interior group (0x007c1150): its MODD colour as
        // the ambient and as one light down from a fixed direction (0xaeedf0,
        // the same in render space), and nothing of the sun - neither its
        // light nor its shadow.
        const vec3 interiorDir = normalize(vec3(0.30822, 0.30822, 0.9));
        float idiff = max(dot(norm, interiorDir), 0.0);
        result = vInteriorAmbient * texColor.rgb + idiff * vInteriorDirect * texColor.rgb;
    } else {
        // Ambient and diffuse only. The client lights an M2 batch with the
        // fixed-function light (FUN_0081fb10) and no specular term; its
        // 'specular' option defaults off and only the terrain reads it.
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
        result = rtAmbient(rt, ambientColor.rgb) * texColor.rgb
               + shadow * (diff * lightColor.rgb * texColor.rgb);

    }

    float dist = length(viewPos.xyz - FragPos);
    // The client's fog per blend mode (FUN_0081fb10, table at 0x00a45390):
    // the world's fog for opaque, alpha key and alpha; black for the adds,
    // white for Mod and grey for Mod2x - the colour that leaves the scene as
    // it is - and none at all for an unfogged material (flag 0x2). Mode 7
    // reads past the table's end and gets none.
    const bool fogOn = unfogged == 0 && blendMode != 7;
    const float fogFactor = pow(clamp((fogParams.y - dist) / (fogParams.y - fogParams.x), 0.0, 1.0), max(fogColor.w, 1.0));
    if (!fogOn) {
        // Drawn as it is.
    } else if (blendAdds()) {
        // Additive. Mixing toward the fog colour would give the card's black
        // corners the fog's colour, and additive then adds that to the scene -
        // the whole quad shows up as a lit rectangle hanging in the air, which
        // is what Orgrimmar's bonfire glow was doing to the wall behind it.
        // Distance can only take an additive contribution away, and so can
        // the air in front of it: its own light is already in the scene
        // behind the card.
        result *= fogFactor;
        if (volumetricParams.x > 0.5) result *= fogVolumeAt(FragPos).a;
    } else if (!blendMultiplies()) {
        // A multiply fogs toward its neutral colour, below with its output.
        result = applyFog(result, FragPos, dist);
    }

    float outAlpha = texColor.a * batchFade;
    // Cutout materials output the sharpened coverage alpha computed above -
    // alpha-to-coverage turns it into per-sample coverage for smooth edges.
    // The distance fade, for a batch drawn with no blending to fade through.
    // Sixteen ordered steps against the fragment's own screen position: a tree
    // at the edge of the draw distance thins out rather than switching off,
    // and it reads the same at every sample count, where leaving the fade in
    // the coverage alone would give two steps at 2x MSAA and none with MSAA
    // off. Multiplying it into the coverage as well would take the leaf edges
    // with it, so the cutout keeps its own alpha.
    if (alphaTest != 0 && blendMode <= 1) {
        if (vFadeAlpha <= bayerDither4x4(ivec2(gl_FragCoord.xy))) discard;
        outAlpha = texColor.a;
    }
    // Pressed on. The real client lifts the whole model while the button is
    // down over it, which is what says "this one, and the click landed": a
    // warm brightening rather than a tint, so a dark door reads as lit and a
    // pale one does not blow out.
    if (vHighlight > 0.0) {
        float lift = clamp(vHighlight, 0.0, 1.0);
        result = result * (1.0 + 0.6 * lift) + vec3(0.22, 0.19, 0.10) * lift;
    }

    if (blendMultiplies()) {
        // The client (FUN_0081fe90) draws a multiply unlit, with no diffuse
        // and an emissive of 1.0 (Mod) or 0.5 (Mod2x), whatever the alpha:
        // Mod2x's doubling then makes its net effect dst * texture.
        result = rawTexRgb * (blendMode == 5 ? 1.0 : 0.5);
        if (fogOn) result = mix(vec3(blendMode == 5 ? 1.0 : 0.5019608), result, fogFactor);
    } else if (blendMode == 3) {
        // NoAlphaAdd ignores alpha: the fade has to be in the colour.
        result *= vFadeAlpha;
    }
    outColor = vec4(result, outAlpha);
}
