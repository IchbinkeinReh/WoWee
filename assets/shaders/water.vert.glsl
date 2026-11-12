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
    float pad1, pad2;
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 Normal;
layout(location = 2) out vec2 TexCoord;
layout(location = 3) out float WaveOffset;
layout(location = 4) out vec2 ScreenUV;
// Where the vertex stood before the waves moved it: the fragment stage
// takes the waves' normal there (water.frag).
layout(location = 5) out vec2 WaveBase;

#include "water_waves.glsli"

void main() {
    float time = fogParams.z;
    vec4 worldPos = vec4(aPos, 1.0);

    // Evaluate Gerstner waves using X,Y horizontal plane
    GerstnerResult waves = evaluateGerstnerWaves(
        vec2(worldPos.x, worldPos.y), time,
        push.waveAmp, push.waveFreq, push.waveSpeed, push.liquidBasicType
    );

    // Apply displacement: xy = horizontal, z = vertical (up)
    worldPos.x += waves.displacement.x;
    worldPos.y += waves.displacement.y;
    worldPos.z += waves.displacement.z;
    WaveOffset = waves.waveHeight; // raw wave height for fragment shader foam

    // Analytical normal from Gerstner tangent/binormal (cross product gives Z-up normal)
    Normal = normalize(cross(waves.binormal, waves.tangent));

    WaveBase = aPos.xy;
    FragPos = worldPos.xyz;
    TexCoord = aTexCoord;
    farClip(view * worldPos);
    vec4 clipPos = projection * view * worldPos;
    gl_Position = clipPos;
    vec2 ndc = clipPos.xy / max(clipPos.w, 1e-5);
    ScreenUV = ndc * 0.5 + 0.5;
}
