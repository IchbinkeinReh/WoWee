#version 450

// The horizon in the fog through and through: fog from 0 to 1 yard, in the
// zone's fog colour (0x007d5e70, render states 8, 9, 10, 12), so a tile is a
// silhouette of the fog against the sky.
//
// Then the air in front of it, as the ground and the sky take it: with the
// light shafts and mist on (VolumetricFog), the mist between the eye and
// anything past its far edge scatters the sunlight toward the camera - the
// warm glow toward a low sun. The ground past that edge (terrain.frag's
// applyFog) and the sky (skybox.frag) both take all of it; without it here
// the horizon stood as a cold band of bare fog colour between the two.
// Not in the original client, which has no such mist.

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

layout(location = 0) in vec3 vWorldPos;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 color = fogColor.rgb;
    if (volumetricParams.x > 0.5) {
        // The whole depth of the volume at this point of the screen: the
        // horizon lies past the far clip, far behind the volume's last slice
        // (400 yards), where the ground's own lookup clamps to it too.
        vec4 clip = projection * view * vec4(vWorldPos, 1.0);
        vec2 uv = clip.xy / max(clip.w, 1e-4) * 0.5 + 0.5;
        vec4 air = textureLod(uFogVolume, vec3(uv, 1.0), 0.0);
        color = color * air.a + air.rgb;
    }
    outColor = vec4(color, 1.0);
}
