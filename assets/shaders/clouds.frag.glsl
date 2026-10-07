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
    vec4 playerPos;
    vec4 playerWake;
    vec4 volumetricParams;  // x = on, y = near, z = 1 / ln(far / near), w = slices
};

layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

// The client's clouds (Wow.exe 3.3.5a): a 128-texel texture of fractal noise
// built on the CPU ("DNClouds0/1", 0x007efd00), cut at the light's cloud
// cover and lit by its three cloud colours (0x007efae0):
//   colour = ch12 + ch11 x shade + ch10 x max(0, n.l) x glow
// with shade thinner-is-brighter and n.l the noise's slope against the sun or
// moon, placed over the texture at a height of 64 texels. Here the texture is
// worked out per pixel instead of uploaded; the formula is the client's.
layout(push_constant) uniform Push {
    vec4 sunLit;     // rgb = ch10, w = glow (1 - 0.75 storm)
    vec4 shade;      // rgb = ch11, w = coverage threshold, a noise byte
    vec4 base;       // rgb = ch12, w = the light's height over the texture, texels
    vec4 light;      // xy = the sun's or moon's texel on the texture, z = noise time
} push;

layout(location = 0) in vec3 vWorldDir;
layout(location = 1) in vec2 vUV;
layout(location = 2) in float vAlpha;

layout(location = 0) out vec4 outColor;

const float kTexels = 128.0;  // SkyCloudLOD 0, the default (0x007f1b10, 0xa41aac)

// The whole depth of the fog volume at this point of the screen, for the sky:
// it lies behind everything, so it takes all the air there is.
vec4 fogVolumeSky(vec2 uv) {
    return textureLod(uFogVolume, vec3(uv, 1.0), 0.0);
}

vec3 hash3(vec3 p) {
    p = vec3(dot(p, vec3(127.1, 311.7, 74.7)),
             dot(p, vec3(269.5, 183.3, 246.1)),
             dot(p, vec3(113.5, 271.9, 124.6)));
    return fract(sin(p) * 43758.5453) * 2.0 - 1.0;
}

// Gradient noise in about -1..1, smooth between lattice points, as the
// client's table-driven lattice noise is (0xd38188 fades, 0xd38688 values).
float latticeNoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    float n000 = dot(hash3(i + vec3(0, 0, 0)), f - vec3(0, 0, 0));
    float n100 = dot(hash3(i + vec3(1, 0, 0)), f - vec3(1, 0, 0));
    float n010 = dot(hash3(i + vec3(0, 1, 0)), f - vec3(0, 1, 0));
    float n110 = dot(hash3(i + vec3(1, 1, 0)), f - vec3(1, 1, 0));
    float n001 = dot(hash3(i + vec3(0, 0, 1)), f - vec3(0, 0, 1));
    float n101 = dot(hash3(i + vec3(1, 0, 1)), f - vec3(1, 0, 1));
    float n011 = dot(hash3(i + vec3(0, 1, 1)), f - vec3(0, 1, 1));
    float n111 = dot(hash3(i + vec3(1, 1, 1)), f - vec3(1, 1, 1));
    float nx00 = mix(n000, n100, u.x);
    float nx10 = mix(n010, n110, u.x);
    float nx01 = mix(n001, n101, u.x);
    float nx11 = mix(n011, n111, u.x);
    return mix(mix(nx00, nx10, u.y), mix(nx01, nx11, u.y), u.z) * 1.6;
}

// The texture's octaves at texel `t`: 8, 16, 32, 64 and 128 lattice cells
// across it, each half the last (0xaf4dc4 for LOD 0), evolving through the
// third lattice axis rather than drifting.
float cloudNoise(vec2 t, int octaves) {
    float sum = 0.0;
    float amp = 1.0;
    float scale = 1.0 / 16.0;
    for (int o = 0; o < octaves; ++o) {
        sum += latticeNoise(vec3(t * scale, push.light.z + float(o) * 17.0)) * amp;
        amp *= 0.5;
        scale *= 2.0;
    }
    return sum;
}

void main() {
    vec2 t = vUV * kTexels;

    // The noise as the texture's byte, over the cover threshold.
    float h = cloudNoise(t, 5);
    float k = round(clamp(h * 64.0 + 128.0, 0.0, 255.0)) - push.shade.w;
    if (k <= 0.0) discard;
    // 255 - 255 x 0.96^(k x 153/256) (0x007edb50).
    float alphaByte = round(255.0 - 255.0 * pow(0.96, k * (153.0 / 256.0)));
    if (alphaByte <= 0.0) discard;

    // Thinner cloud is the brighter: ((255 - alpha) / 2 + 64) / 255.
    float shadeAmount = (floor((255.0 - alphaByte) * 0.5) + 64.0) / 255.0;
    vec3 rgb = push.base.rgb + push.shade.rgb * shadeAmount;

    // The slope of the first three octaves, a texel back on each axis, as
    // the normal; the sun or moon over the texture as the light.
    float h3 = cloudNoise(t, 3);
    vec2 slope = vec2(cloudNoise(t - vec2(1.0, 0.0), 3) - h3,
                      cloudNoise(t - vec2(0.0, 1.0), 3) - h3);
    vec3 n = vec3(slope, 1.0);
    vec3 l = vec3(push.light.xy - t, push.base.w);
    float lit = dot(n, l) * inversesqrt(dot(n, n) * dot(l, l));
    if (lit > 0.0) rgb += push.sunLit.rgb * lit * push.sunLit.w;
    rgb = min(rgb, vec3(1.0));

    float alpha = alphaByte / 255.0 * vAlpha;
    if (alpha < 0.004) discard;

    // Behind the air, as the sky it is drawn over is: blended by alpha, so
    // the cloud and the dome under it come out with the same air in front.
    if (volumetricParams.x > 0.5) {
        vec4 clip = projection * mat4(mat3(view)) * vec4(vWorldDir, 1.0);
        vec4 air = fogVolumeSky(clip.xy / max(clip.w, 1e-4) * 0.5 + 0.5);
        rgb = rgb * air.a + air.rgb;
    }
    outColor = vec4(rgb, alpha);
}
