#version 450

// The client's own liquid: psLiquidWater and psLiquidMagma (see
// liquid.vert.glsl). texture[0] is render state Texture0 - the depth slot at
// the depth coordinate - and texture[1] Texture1, the animated slot at the
// surface coordinate (0x008a5590 binds 0x15 and 0x16).
//
//   psLiquidWater:  rgb = primary x tex0 + tex1.rgb + tex1.a x (secondary + 0.25)
//                   a   = primary.a x tex0.a
//   psLiquidMagma:  rgb = primary x tex0 (its one texture), a = 1
//
// Water is blended by source alpha (render state 6 set to 2) with alpha
// reference 1 (0xad8b7c[2]); magma sets no blend.

layout(set = 1, binding = 0) uniform sampler2D uAnimTex;
layout(set = 2, binding = 0) uniform sampler2D uDepthTex;

// The procedural depth textures this frame (0x008a2bf0, 0x008a2ac0), 64 rows
// each from the shallows down: proceduralRiverDepthTex, proceduralOceanDepthTex
// and proceduralWmoWaterTex's coloured half, whose white half shares its alpha.
layout(set = 3, binding = 0) uniform LiquidRamps {
    vec4 ramps[192];
};

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
    vec4 volumetricParams;  // x = on, y = near, z = 1 / ln(far / near), w = slices
};
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(push_constant) uniform Push {
    vec4 animMatrix;
    vec4 params;   // x depth v scale, y light, z kind (0 water, 1 magma), w depth source
    vec4 scroll;
    vec4 specular;
} push;

layout(location = 0) in vec2 vSurfaceUV;
layout(location = 1) in vec2 vDepthUV;
layout(location = 2) in vec4 vPrimary;
layout(location = 3) in vec3 vSecondary;
layout(location = 4) in float vFog;
layout(location = 5) in vec3 vWorld;

layout(location = 0) out vec4 outColor;

// One of the 8 x 64 procedural textures as the device samples it: bilinear
// and clamped (0x008a2e20 creates them with filtering on and no wrap).
vec4 rampTexel(int ramp, int row, float whiteness) {
    vec4 c = ramps[ramp * 64 + row];
    return vec4(mix(c.rgb, vec3(1.0), whiteness), c.a);
}

vec4 sampleRamp(int ramp, vec2 uv) {
    float f = clamp(uv.y * 64.0 - 0.5, 0.0, 63.0);
    int r0 = int(floor(f));
    int r1 = min(r0 + 1, 63);
    float t = f - float(r0);
    // Only proceduralWmoWaterTex differs across a row: four coloured
    // texels, then four white.
    float whiteness = 0.0;
    if (ramp == 2) whiteness = clamp(clamp(uv.x * 8.0 - 0.5, 0.0, 7.0) - 3.0, 0.0, 1.0);
    return mix(rampTexel(ramp, r0, whiteness), rampTexel(ramp, r1, whiteness), t);
}

vec4 depthTexture(vec2 uv, float sourceParam) {
    int source = int(sourceParam + 0.5);
    if (source <= 2) return sampleRamp(source, uv);
    // A "procedural" name the client has not registered: its one green texel.
    if (source == 3) return vec4(0.0, 1.0, 0.0, 1.0);
    return texture(uDepthTex, uv);
}


// The air between the camera and the liquid, out of the fog volume, as the
// terrain and the traced water take it (water.frag's fogVolumeAt): without
// it a far lava lake under Blackrock Spire stood out of the orange haze
// that hides the ground round it, in the distance fog's bare lavender.
vec3 liquidAir(vec3 color, vec3 worldPos) {
    if (volumetricParams.x < 0.5) return color;
    vec4 clip = projection * view * vec4(worldPos, 1.0);
    float depth = max(clip.w, 1e-4);
    vec2 uv = clip.xy / depth * 0.5 + 0.5;
    float slice = log(max(depth, volumetricParams.y) / volumetricParams.y) * volumetricParams.z;
    vec4 a = textureLod(uFogVolume, vec3(uv, slice - 0.5 / volumetricParams.w), 0.0);
    return color * a.a + a.rgb;
}

void main() {
    vec4 color;
    if (push.params.z < 0.5) {
        vec4 tex0 = depthTexture(vDepthUV, push.params.w);
        vec4 tex1 = texture(uAnimTex, vSurfaceUV);
        color.rgb = vPrimary.rgb * tex0.rgb + tex1.rgb;
        // The specular term and the quarter that rides with it, both gated by
        // the animated texture's alpha. NoSpec (specular.a 0) has neither.
        if (push.specular.a > 0.0) color.rgb += tex1.a * (vSecondary + 0.25);
        color.a = vPrimary.a * tex0.a;
        if (color.a * 255.0 < 1.0) discard;
    } else {
        color = vec4(vPrimary.rgb * texture(uAnimTex, vSurfaceUV).rgb, 1.0);
    }
    outColor = vec4(liquidAir(mix(fogColor.rgb, clamp(color.rgb, 0.0, 1.0), vFog), vWorld), color.a);
}
