#version 450
// The tessellated water's evaluation stage: a point of the finer surface the
// tessellator cut the liquid grid's triangle into (water.tesc), moved by the
// waves as water.vert moves the grid's corners, with the waves the finer
// surface can carry near the camera included, and raised there by the
// swimmers' rings and the trails' wakes (water_interaction.glsli). The
// fragment stage still takes its normal from the waves and the interaction's
// slopes at every pixel, not from this surface, so the slopes are not
// counted twice: this only puts the surface where they say it is.

layout(triangles, fractional_odd_spacing, ccw) in;

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
#include "water_frame.glsli"

// As water.vert declares it (the fragment stage's fields spelled the same).
layout(push_constant) uniform Push {
    vec4 closeColor;
    vec4 farColor;
    float waveAmp;
    float waveFreq;
    float waveSpeed;
    float liquidBasicType; // 0=water, 1=ocean
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

layout(location = 0) in vec3 tcWorldPos[];
layout(location = 1) in vec2 tcTexCoord[];
layout(location = 2) in vec4 tcBed[];

layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 Normal;
layout(location = 2) out vec2 TexCoord;
layout(location = 3) out float WaveOffset;
layout(location = 4) out vec2 ScreenUV;
layout(location = 5) out vec2 WaveBase;
layout(location = 6) out vec4 BedInfo;

#include "water_waves.glsli"
#include "water_interaction.glsli"
#include "water_tess.glsli"

// The height of the swimmers' rings and the trails' wakes at `at`: the same
// rings and points, and the same reach, as water.frag's slopes (its main and
// kelvinWake), each slope's height in its place. Rain's rings stay slope only.
float interactionHeight(vec2 at) {
    float h = 0.0;
    bool physics = swimWaveInfo.z > 0.5;

    int swimWaveCount = clamp(int(swimWaveInfo.x), 0, MAX_SWIM_WAVES);
    int swimGroupCount = clamp(int(wakeBounds.y), 0, MAX_WATER_GROUPS);
    for (int g = 0; g < swimGroupCount; ++g) {
        vec4 grp = swimWaveGroups[g];
        vec2 toGroup = at - grp.xy;
        if (dot(toGroup, toGroup) >= grp.z * grp.z) continue;
        int first = clamp(int(grp.w), 0, swimWaveCount);
        int end = g + 1 < swimGroupCount ? clamp(int(swimWaveGroups[g + 1].w), first, swimWaveCount) : swimWaveCount;
        for (int i = first; i < end; ++i) {
            vec4 w = swimWaves[i];
            vec2 fromWave = at - w.xy;
            if (physics) {
                h += impulseRingHeight(fromWave, w.z, 0.25, w.w);
                continue;
            }
            float width = 0.28 + w.z * 0.25;
            float along = (length(fromWave) - (0.15 + w.z * 1.7)) / width;
            h += ringCrestHeight(along, 4.0, width, w.w * 0.22 * (1.0 - w.z / 1.6));
        }
    }

    if (wakeBounds.z < 0.5) return h;
    int count = clamp(int(wakeBounds.w), 0, MAX_WAKE_POINTS);
    int groups = clamp(int(wakeBounds.x), 0, MAX_WATER_GROUPS);
    if (physics) {
        const float PHYS_REACH = 12.0;
        for (int g = 0; g < groups; ++g) {
            vec2 toGroup = at - wakeGroups[g].xy;
            if (dot(toGroup, toGroup) >= wakeGroups[g].z * wakeGroups[g].z) continue;
            int first = clamp(int(wakeGroups[g].w), 0, count);
            int end = g + 1 < groups ? clamp(int(wakeGroups[g + 1].w), first, count) : count;
            for (int i = first; i < end; ++i) {
                vec4 wp = wakePoints[i];
                vec2 from = at - wp.xy;
                if (dot(from, from) >= PHYS_REACH * PHYS_REACH) continue;
                h += impulseRingHeight(from, wp.z, 0.45, wp.w * 0.5);
            }
        }
        return h;
    }
    const float KELVIN_REACH = 7.0;
    for (int g = 0; g < groups; ++g) {
        int first = clamp(int(wakeGroups[g].w), 0, count);
        int end = g + 1 < groups ? clamp(int(wakeGroups[g + 1].w), first, count) : count;
        if (end - first < 1) continue;
        vec2 head = wakePoints[end - 1].xy;
        vec2 toHead = at - head;
        if (dot(toHead, toHead) >= KELVIN_REACH * KELVIN_REACH) continue;
        for (int i = first; i < end; ++i) {
            vec4 wp = wakePoints[i];
            float radius = 0.2 + length(head - wp.xy) / 3.0;
            float along = (length(at - wp.xy) - radius) / 0.32;
            if (abs(along) > 3.0) continue;
            h += ringCrestHeight(along, 3.2, 0.32, wp.w * (1.0 - wp.z) * 0.14 / (1.0 + 0.6 * radius));
        }
        // The bow wave: its crest's height, on the side the trail moves to.
        if (end - first >= 3) {
            vec2 fwd = head - wakePoints[end - 3].xy;
            float fl = length(fwd);
            if (fl > 0.05) {
                fwd /= fl;
                float d = length(toHead);
                float ahead = dot(toHead / max(d, 0.01), fwd);
                float along = (d - 0.55) / 0.22;
                h += ringCrestHeight(along, 3.2, 0.22, 0.2 * wakePoints[end - 1].w * smoothstep(-0.3, 0.7, ahead));
            }
        }
    }
    return h;
}

void main() {
    pwWindBlend = push.windBlend.xyz;
    // Weighed the same way on both triangles that share an edge, so the
    // points along it land in the same places for both.
    precise vec3 base = gl_TessCoord.x * tcWorldPos[0] + gl_TessCoord.y * tcWorldPos[1] +
                        gl_TessCoord.z * tcWorldPos[2];
    vec2 texCoord = gl_TessCoord.x * tcTexCoord[0] + gl_TessCoord.y * tcTexCoord[1] +
                    gl_TessCoord.z * tcTexCoord[2];
    vec4 bed = gl_TessCoord.x * tcBed[0] + gl_TessCoord.y * tcBed[1] + gl_TessCoord.z * tcBed[2];
    float time = fogParams.z;
    // From the point itself, not its triangle: a point on a shared edge is
    // moved alike by both triangles.
    float dist = length(base - viewPos.xyz);

    // The waves as water.vert has them, but with the physics waves' shortest
    // moved wave following how finely the surface is cut here
    // (waterTessMinWavelength) instead of the whole grid's 9 yards; each wave
    // fades in over half again its cut-off, so none appears at a set
    // distance as a step.
    float minWavelength = waterTessMinWavelength(dist);
    // With the shore waves on, over the bed as water.vert has them.
    GerstnerResult waves;
    if (push.shore.x > 0.5) {
        waves = push.pad2 > 0.0
            ? evaluatePhysicalWavesShore(base.xy, time, push.waveAmp, push.waveFreq, push.pad2, push.waveSpeed,
                                         minWavelength, minWavelength * 1.5, bed)
            : evaluateGerstnerWavesShore(base.xy, time, push.waveAmp, push.waveFreq, push.waveSpeed,
                                         push.liquidBasicType, bed);
    } else {
        waves = push.pad2 > 0.0
            ? evaluatePhysicalWavesFaded(base.xy, time, push.waveAmp, push.waveFreq, push.pad2, push.waveSpeed,
                                         minWavelength, minWavelength * 1.5)
            : evaluateGerstnerWaves(base.xy, time, push.waveAmp, push.waveFreq, push.waveSpeed,
                                    push.liquidBasicType);
    }

    vec4 worldPos = vec4(base + waves.displacement, 1.0);
    WaveOffset = waves.waveHeight;
    Normal = normalize(cross(waves.binormal, waves.tangent));

    // The rings and wakes as height where the surface is cut finely enough to
    // carry them, taken where water.frag takes their slopes (the point after
    // the waves have moved it).
    float interaction = waterTessInteraction(dist);
    if (interaction > 0.0) worldPos.z += interaction * interactionHeight(worldPos.xy);

    WaveBase = base.xy;
    BedInfo = bed;
    FragPos = worldPos.xyz;
    TexCoord = texCoord;
    farClip(view * worldPos);
    vec4 clipPos = projection * view * worldPos;
    gl_Position = clipPos;
    vec2 ndc = clipPos.xy / max(clipPos.w, 1e-5);
    ScreenUV = ndc * 0.5 + 0.5;
}
