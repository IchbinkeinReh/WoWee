#version 450

// psLiquidProcWater (CMaterialProcWater 0x008a48f0). Units as 0x008a48f0
// binds them: 0 and 1 cube maps (LiquidType Textures 0 and 1), 2 the depth
// colour (Texture 4), 3 the highlight mask (Texture 5), 4 the far normals
// (Texture 2), 5 the near ones (Texture 3, RG at one coordinate and AB at
// the other). c5 is the camera (0xb24170), c6 the way to the sun, c9 the
// light's specular colour (0x008a3c90).
//
//   n    = mix(normalize(n5(tc1.xy) + n5(tc1.wz)), n4(tc0), sat(|V.xy| / F11 + 0.5))
//   N    = (n . T, n . B, n . N)
//   v    = (P - c5) / |(P - c5).xy|,   f = (1 - sat(-v . N))^2
//   rgb  = mix(tex2 + cube0(-N), cube1(reflect(v, N)), F13 + F14 f)
//        + 4 x sat(normalize(v + c6) . N)^50 x c9 x mask.a x (F15 + F16 f)
//   a    = tex2.a + F17 f
//
// Blended by its alpha as the water is (render state 6 set to 2).

#include "liquid_proc.glsli"

layout(set = 1, binding = 0) uniform samplerCube uCube0;
layout(set = 1, binding = 1) uniform samplerCube uCube1;
layout(set = 1, binding = 2) uniform sampler2D uDepthTex;
layout(set = 1, binding = 3) uniform sampler2D uMaskTex;
layout(set = 1, binding = 4) uniform sampler2D uFarNormals;
layout(set = 1, binding = 5) uniform sampler2D uNearNormals;

// The procedural depth textures this frame, as liquid.frag.glsl reads them.
layout(set = 2, binding = 0) uniform LiquidRamps {
    vec4 ramps[192];
};

layout(location = 0) in vec4 vTex0;
layout(location = 1) in vec4 vTex1;
layout(location = 2) in vec4 vTex2;
layout(location = 3) in vec3 vPos;
layout(location = 4) in vec3 vT;
layout(location = 5) in vec3 vB;
layout(location = 6) in vec3 vN;
layout(location = 7) in float vFog;

layout(location = 0) out vec4 outColor;

vec4 rampTexel(int ramp, int row, float whiteness) {
    vec4 c = ramps[ramp * 64 + row];
    return vec4(mix(c.rgb, vec3(1.0), whiteness), c.a);
}

vec4 sampleRamp(int ramp, vec2 uv) {
    float f = clamp(uv.y * 64.0 - 0.5, 0.0, 63.0);
    int r0 = int(floor(f));
    int r1 = min(r0 + 1, 63);
    float t = f - float(r0);
    float whiteness = 0.0;
    if (ramp == 2) whiteness = clamp(clamp(uv.x * 8.0 - 0.5, 0.0, 7.0) - 3.0, 0.0, 1.0);
    return mix(rampTexel(ramp, r0, whiteness), rampTexel(ramp, r1, whiteness), t);
}

vec4 depthTexture(vec2 uv, float sourceParam) {
    int source = int(sourceParam + 0.5);
    if (source <= 2) return sampleRamp(source, uv);
    if (source == 3) return vec4(0.0, 1.0, 0.0, 1.0);
    return texture(uDepthTex, uv);
}

// A normal from two channels: z is what is left of unit length.
vec3 unpack(vec2 c) {
    vec2 xy = c * 2.0 - 1.0;
    return vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
}

void main() {
    vec3 nearN = unpack(texture(uNearNormals, vTex1.xy).rg) + unpack(texture(uNearNormals, vTex1.zw).ab);
    vec3 farN = unpack(texture(uFarNormals, vTex0.xy).rg);
    vec3 V = vPos - toClient(viewPos.xyz);
    float far = clamp(length(V.xy) * pw.c10.x + 0.5, 0.0, 1.0);
    vec3 n = mix(normalize(nearN), farN, far);
    vec3 N = vec3(dot(n, vT), dot(n, vB), dot(n, vN));

    vec3 v = V / length(V.xy);
    vec3 R = v - 2.0 * N * dot(v, N);
    float f = 1.0 - clamp(dot(-v, N), 0.0, 1.0);
    f *= f;

    vec4 base = depthTexture(vTex2.xy, pw.depthParams.y);
    vec3 refr = base.rgb + texture(uCube0, (-N).xzy).rgb;
    vec3 refl = texture(uCube1, R.xzy).rgb;
    vec3 color = mix(refr, refl, f * pw.c10.w + pw.c10.z);

    vec3 H = normalize(v - toClient(lightDir.xyz));
    float s = pow(clamp(dot(H, N), 0.0, 1.0), pw.c9.w);
    float mask = texture(uMaskTex, vTex2.zw).a;
    color += 4.0 * s * pw.c9.rgb * mask * (f * pw.c11.z + pw.c11.y);
    float alpha = f * pw.c11.x + base.a;
    if (clamp(alpha, 0.0, 1.0) * 255.0 < 1.0) discard;

    outColor = vec4(mix(fogColor.rgb, clamp(color, 0.0, 1.0), vFog), clamp(alpha, 0.0, 1.0));
}
