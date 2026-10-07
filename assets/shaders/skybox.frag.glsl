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

// The client's sky dome (Wow.exe 3.3.5a 0x007f2470, coloured by 0x007f0530):
// an untextured unit sphere centred cos(45 deg) below the eye, whose rows sit
// at polar angles {0, .17, .20, .23, .24, .25, 1} x pi, carrying the light's
// sky channels ch2..ch6, then the fog colour ch7 at the horizon ring and the
// nadir. Seen from the eye those rows are at 90, 16.8, 9.8, 3.7, 1.8 and 0
// degrees of elevation, so the sky ends in exactly the colour distant
// terrain fogs to. The four banded rows also carry LightParams.HighlightSky's
// dawn and dusk glow, column by column round the dome's 24; nothing else is
// added.
layout(push_constant) uniform Push {
    vec4 skyTop;      // ch2; w = the glow's strength
    vec4 skyMiddle;   // ch3; w = where the glow's azimuth curve starts
    vec4 skyBand1;    // ch4
    vec4 skyBand2;    // ch5
    vec4 skySmog;     // ch6
    vec4 skyFog;      // ch7, the fog colour
} push;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

// The whole depth of the fog volume at this point of the screen, for the sky:
// it lies behind everything, so it takes all the air there is.
vec4 fogVolumeSky(vec2 uv) {
    return textureLod(uFogVolume, vec3(uv, 1.0), 0.0);
}

// The dome's polar angle, as a fraction of pi, where the ray from the eye
// along `dir` meets it.
float domePolar(vec3 dir) {
    const float c = 0.70710678;  // the centre's depth below the eye
    // |t*dir - (0,0,-c)| = 1, the far root: t^2 + 2c dir.z t + c^2 - 1 = 0.
    float t = -c * dir.z + sqrt(max(c * c * dir.z * dir.z - c * c + 1.0, 0.0));
    float cosPhi = clamp(t * dir.z + c, -1.0, 1.0);
    return acos(cosPhi) * 0.31830989;
}

// The glow's azimuth curve (0xaf4bac), as 0x007ed3b0 reads a curve: the key
// after t is the first not below it, wrapping from the last to the first.
float highlightAzimuth(float t) {
    const float kt[6] = float[6](0.125, 0.375, 0.5, 0.625, 0.75, 0.875);
    const float kv[6] = float[6](1.0, 0.0, -0.5, -0.7, -0.5, 0.0);
    int next = 0;
    while (next < 6 && t > kt[next]) ++next;
    int prev;
    if (next == 6) { next = 0; prev = 5; } else { prev = next == 0 ? 5 : next - 1; }
    float span = kt[next] - kt[prev];
    if (span < 0.0) span += 1.0;
    float into = t - kt[prev];
    if (into < 0.0) into += 1.0;
    return kv[prev] + (kv[next] - kv[prev]) * (into / span);
}

// One row's colour at a column whose azimuth curve reads `az` (0x007f0530).
// Rows 1 to 4, the bands ch3..ch6, carry the glow: pulled toward ch3 by its
// strength h, back toward the band as az rises to 1, on toward ch2 below 0.
// daynight::skyHighlightRow is the same, for the tests.
vec3 domeRow(int row, float az) {
    vec3 base;
    if (row == 0) return push.skyTop.rgb;
    else if (row == 1) base = push.skyMiddle.rgb;
    else if (row == 2) base = push.skyBand1.rgb;
    else if (row == 3) base = push.skyBand2.rgb;
    else if (row == 4) base = push.skySmog.rgb;
    else return push.skyFog.rgb;
    float h = push.skyTop.w;
    vec3 toward = mix(base, push.skyMiddle.rgb, h);
    if (az >= 0.0) return mix(base, toward, (1.0 - az) * h);
    vec3 lifted = mix(toward, push.skyTop.rgb, 0.7 * h);
    return mix(toward, lifted, -az * h);
}

void main() {
    // Reconstruct world-space ray direction from screen position.
    float ndcX =  TexCoord.x * 2.0 - 1.0;
    float ndcY = -(TexCoord.y * 2.0 - 1.0);

    vec3 viewDir = vec3(ndcX / projection[0][0],
                        ndcY / abs(projection[1][1]),
                        -1.0);

    mat3 invViewRot = transpose(mat3(view));
    vec3 worldDir = normalize(invViewRot * viewDir);

    // Gouraud shading between the dome's rows, as linear in the polar angle,
    // and between its columns, as linear in azimuth.
    float u = domePolar(worldDir);
    float r;  // row index with its fraction
    if (u < 0.17) r = u / 0.17;
    else if (u < 0.20) r = 1.0 + (u - 0.17) / 0.03;
    else if (u < 0.23) r = 2.0 + (u - 0.20) / 0.03;
    else if (u < 0.24) r = 3.0 + (u - 0.23) / 0.01;
    else if (u < 0.25) r = 4.0 + (u - 0.24) / 0.01;
    else r = 5.0;
    int r0 = int(floor(r));
    int r1 = min(r0 + 1, 5);
    float rf = r - float(r0);

    // The dome's column j stands at world azimuth pi/2 - 2 pi j / 24. World
    // x and y are render y and x.
    float phi = atan(worldDir.x, worldDir.y);
    float col = fract(0.25 - phi * 0.15915494) * 24.0;
    float j0 = floor(col);
    float cf = col - j0;
    float az0 = highlightAzimuth(fract(push.skyMiddle.w - j0 / 24.0));
    float az1 = highlightAzimuth(fract(push.skyMiddle.w - (j0 + 1.0) / 24.0));

    vec3 c0 = mix(domeRow(r0, az0), domeRow(r1, az0), rf);
    vec3 c1 = mix(domeRow(r0, az1), domeRow(r1, az1), rf);
    vec3 sky = mix(c0, c1, cf);

    if (volumetricParams.x > 0.5) {
        vec4 air = fogVolumeSky(TexCoord);
        sky = sky * air.a + air.rgb;
    }

    outColor = vec4(sky, 1.0);
}
