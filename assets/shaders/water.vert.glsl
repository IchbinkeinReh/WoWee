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
};

#include "far_clip.glsli"

layout(push_constant) uniform Push {
    vec4 closeColor;  // the light's water colours, fragment stage only
    vec4 farColor;
    float waveAmp;
    float waveFreq;
    float waveSpeed;
    float liquidBasicType; // 0=water, 1=ocean, 2=magma, 3=slime
    // The rest belongs to the fragment stage, and is spelled the same way here
    // so the two declarations of one range agree. This said `float brightness`
    // where the fragment stage has a vec2, which is a field that does not exist
    // sitting at another field's offset - harmless only for as long as nobody
    // reads it.
    vec2 screenSize;
    vec2 depthRange;
    float sceneValid;
    float lightColors;
    float pad1;
    float pad2;  // > 0: physics waves, the wind in m/s (water_waves.glsli)
    // x 1: the shore waves (water_waves.glsli's evaluatePhysicalWavesShore),
    // y how much rough weather whips up their surf (water.frag).
    vec4 shore;
    // x the physics waves' upper rung's share, y its wind, z the lower rung's
    // place (water_waves.glsli's pwWindBlend, wave_surface::WaveParams'
    // windUpperShare, windUpper and windRung).
    vec4 windBlend;
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
// The bed under the vertex (WaterRenderer::createWaterMesh): x its depth,
// yz the way to the shallows, w its slope (water_waves.glsli).
layout(location = 2) in vec4 aBed;

layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 Normal;
layout(location = 2) out vec2 TexCoord;
layout(location = 3) out float WaveOffset;
layout(location = 4) out vec2 ScreenUV;
// Where the vertex stood before the waves moved it: the fragment stage
// takes the waves' normal there (water.frag).
layout(location = 5) out vec2 WaveBase;
layout(location = 6) out vec4 BedInfo;

#include "water_waves.glsli"

void main() {
    float time = fogParams.z;
    pwWindBlend = push.windBlend.xyz;
    vec4 worldPos = vec4(aPos, 1.0);

    // Evaluate Gerstner waves using X,Y horizontal plane. With physics waves
    // on (pad2 = the wind in m/s) the waves come from the wind's spectrum
    // (water_waves.glsli): waveAmp scales them, waveFreq is the fetch in
    // yards and waveSpeed the wind's direction. Waves under 9 yards
    // (PW_VERTEX_MIN_WAVELENGTH), shorter than the liquid grid can carry,
    // stay in the fragment stage's normal. The swimmers and boats that ride
    // the waves take this same surface on the CPU (wave_surface.hpp).
    // With the shore waves on (push.shore.x) the same waves over the bed:
    // slowed, raised, turned and broken as the water shallows.
    GerstnerResult waves;
    if (push.shore.x > 0.5) {
        waves = push.pad2 > 0.0
            ? evaluatePhysicalWavesShore(worldPos.xy, time, push.waveAmp, push.waveFreq, push.pad2,
                                         push.waveSpeed, PW_VERTEX_MIN_WAVELENGTH,
                                         PW_VERTEX_MIN_WAVELENGTH, aBed)
            : evaluateGerstnerWavesShore(worldPos.xy, time, push.waveAmp, push.waveFreq, push.waveSpeed,
                                         push.liquidBasicType, aBed);
    } else {
        waves = push.pad2 > 0.0
            ? evaluatePhysicalWaves(vec2(worldPos.x, worldPos.y), time, push.waveAmp, push.waveFreq, push.pad2,
                                    push.waveSpeed, PW_VERTEX_MIN_WAVELENGTH)
            : evaluateGerstnerWaves(vec2(worldPos.x, worldPos.y), time,
                                    push.waveAmp, push.waveFreq, push.waveSpeed, push.liquidBasicType);
    }

    // Apply displacement: xy = horizontal, z = vertical (up)
    worldPos.x += waves.displacement.x;
    worldPos.y += waves.displacement.y;
    worldPos.z += waves.displacement.z;
    WaveOffset = waves.waveHeight; // raw wave height for fragment shader foam

    // Analytical normal from Gerstner tangent/binormal (cross product gives Z-up normal)
    Normal = normalize(cross(waves.binormal, waves.tangent));

    WaveBase = aPos.xy;
    BedInfo = aBed;
    FragPos = worldPos.xyz;
    TexCoord = aTexCoord;
    farClip(view * worldPos);
    vec4 clipPos = projection * view * worldPos;
    gl_Position = clipPos;
    vec2 ndc = clipPos.xy / max(clipPos.w, 1e-5);
    ScreenUV = ndc * 0.5 + 0.5;
}
