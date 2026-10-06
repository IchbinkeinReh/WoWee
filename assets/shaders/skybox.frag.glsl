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
// terrain fogs to. Nothing else is added: the warm scatter, the sun glow,
// the horizon haze and the moonlight tint that used to be here are not in
// the client.
layout(push_constant) uniform Push {
    vec4 skyTop;      // ch2
    vec4 skyMiddle;   // ch3
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

void main() {
    // Reconstruct world-space ray direction from screen position.
    float ndcX =  TexCoord.x * 2.0 - 1.0;
    float ndcY = -(TexCoord.y * 2.0 - 1.0);

    vec3 viewDir = vec3(ndcX / projection[0][0],
                        ndcY / abs(projection[1][1]),
                        -1.0);

    mat3 invViewRot = transpose(mat3(view));
    vec3 worldDir = normalize(invViewRot * viewDir);

    // Gouraud shading along the dome's rows, as linear in the polar angle.
    float u = domePolar(worldDir);
    vec3 sky;
    if (u < 0.17) {
        sky = mix(push.skyTop.rgb, push.skyMiddle.rgb, u / 0.17);
    } else if (u < 0.20) {
        sky = mix(push.skyMiddle.rgb, push.skyBand1.rgb, (u - 0.17) / 0.03);
    } else if (u < 0.23) {
        sky = mix(push.skyBand1.rgb, push.skyBand2.rgb, (u - 0.20) / 0.03);
    } else if (u < 0.24) {
        sky = mix(push.skyBand2.rgb, push.skySmog.rgb, (u - 0.23) / 0.01);
    } else if (u < 0.25) {
        sky = mix(push.skySmog.rgb, push.skyFog.rgb, (u - 0.24) / 0.01);
    } else {
        // The horizon ring and the nadir are both the fog colour.
        sky = push.skyFog.rgb;
    }

    if (volumetricParams.x > 0.5) {
        vec4 air = fogVolumeSky(TexCoord);
        sky = sky * air.a + air.rgb;
    }

    outColor = vec4(sky, 1.0);
}
