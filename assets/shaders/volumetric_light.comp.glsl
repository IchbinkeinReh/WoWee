#version 460

// The light the air scatters toward the eye along each pixel's ray: the sun's
// (or at night the moon's) where it reaches the air, and every lamp's within
// its reach. See volumetric_light.hpp.
//
// Each invocation is one texel of the march's image (a half or a quarter of
// the screen a side), its ray from the camera to the nearest surface it
// covers (volumetric_light_depth.comp.glsl). Two gatherings along it:
//
//   the sky body   a fixed number of samples over the near air, each lit
//                  only where the sun (moon) itself reaches that point - the
//                  shadow map's cascades, or with the ray traced lighting on
//                  ray queries a ray to it - so whatever is opaque cuts the
//                  shafts, and a room lit by no opening has none;
//   each lamp      where the ray runs through the lamp's reach, a fixed
//                  number of samples over that stretch, each lit only if the
//                  lamp is in sight of it - a traced ray to the lamp with the
//                  ray traced lighting, otherwise a few steps toward it
//                  against the scene's depth.
//
// The samples are spread by a fixed pattern over the screen, not moved from
// frame to frame: nothing here is carried over from an earlier frame. The
// blur that follows (volumetric_light_blur.comp.glsl) evens the pattern out.
//
// Built three ways: as it is (the shadow map and the depth only), with
// VL_TRACE (the compute tracer, rt_trace_sw.glsli) and with VL_TRACE and
// RT_HARDWARE (ray queries, rt_trace_hw.glsli).

#ifdef RT_HARDWARE
#extension GL_EXT_ray_query : require
#endif

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

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
    vec4 volumetricParams;
    vec4 rtParams;
    vec4 cameraFogColor;
    vec4 averagedDirectColor;
    vec4 averagedAmbientColor;
    vec4 windowLight;
    vec4 specularColor;
    // The shadow cascades (shadow_csm.glsli); declared so they land where the
    // renderer writes them.
    mat4 cascadeMatrix[4];
    vec4 cascadeRect[4];
    vec4 cascadeTexel[4];
    vec4 cascadeInfo;
};

// One lamp, as rt_lights::PointLightGPU (rt_point_lights.hpp): position and
// reach (negative for a building's baked light), colour and full-strength
// radius, and the storey with in w what gave it (z -1: carried by a unit).
struct VLLight {
    vec4 positionEnd;
    vec4 colorStart;
    vec4 storey;
};

// VolumetricLight's VLParamsGPU (std140).
layout(set = 1, binding = 0) uniform VLParams {
    mat4 invViewProj;   // clip (reversed depth) to world
    mat4 viewProj;
    vec4 camPos;        // xyz = the eye, w = how far the sky body's light is gathered
    vec4 camForward;    // xyz = the view's forward axis
    vec4 bodyDir;       // xyz = toward the sun or moon, w = its share (0: none this frame)
    vec4 bodyColor;     // rgb = its light, w = the forward lobe's share (lobeShare)
    vec4 air;           // x = sky body scatter per yard, y = lamp scatter per yard,
                        // z = sky body samples, w = samples per lamp
    ivec4 modes;        // x = lamps, y = sky body occlusion (0 none, 1 shadow map, 2 traced),
                        // z = lamp occlusion (1 the depth, 2 traced), w = the traced scene has content
    vec4 screen;        // xy = the march's size
    VLLight lights[32];
} vl;

layout(set = 1, binding = 1) uniform sampler2D uShadowDepth;
layout(set = 1, binding = 3, r32f) uniform readonly image2D uLowDepth;
layout(set = 1, binding = 4, rgba16f) uniform writeonly image2D uMarch;

#ifdef VL_TRACE
#define RT_SCENE_SET 2
#define RT_SCENE_HAS_CONTENT (vl.modes.w > 0)
#include "rt_sampling.glsli"
#ifdef RT_HARDWARE
#include "rt_trace_hw.glsli"
#else
#include "rt_trace_sw.glsli"
#endif
#endif

const int RT_LIGHT_KIND_UNIT = 4;  // rt_lights::Source::Unit

vec3 unproject(vec2 uv, float depth) {
    vec4 w = vl.invViewProj * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return w.xyz / w.w;
}

// Where a point lies across cascade c's tile: -1..1 (volumetric_fog_inject).
vec2 cascadeTileCoord(int c, vec2 uv) {
    vec4 rect = cascadeRect[c];
    return (uv - rect.xy) / (rect.zw - rect.xy) * 2.0 - 1.0;
}

// Whether the sun (moon) reaches p, off the shadow map the surfaces use: the
// finest cascade whose tile holds it, one tap. The mist's sunVisibility,
// except that a point the map does not cover is dark rather than lit: with
// no word on what stands between it and the sky, it takes none of the sky's
// light (the user's rule: sun and moon only where they reach).
float mapVisibility(vec3 p) {
    if (shadowParams.x < 0.5) return 0.0;
    int count = min(int(cascadeInfo.x + 0.5), 4);
    for (int c = 0; c < count; ++c) {
        vec4 ls = cascadeMatrix[c] * vec4(p, 1.0);
        vec3 proj = ls.xyz / ls.w;
        vec2 tile = abs(cascadeTileCoord(c, proj.xy));
        vec2 inset = 2.0 * cascadeTexel[c].yz / (cascadeRect[c].zw - cascadeRect[c].xy);
        if (tile.x > 1.0 - inset.x || tile.y > 1.0 - inset.y) continue;
        if (proj.z < 0.0 || proj.z > 1.0) return 0.0;
        float stored = textureLod(uShadowDepth, proj.xy, 0.0).r;
        return (proj.z - 0.0005 <= stored) ? 1.0 : 0.0;
    }
    return 0.0;
}

// volumetric_light::phaseHG.
float phaseHG(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5);
}

// volumetric_light::glowFalloff.
float vlGlowFalloff(float d, float end) {
    if (!(end > 0.0) || !(d < end)) return 0.0;
    float core = clamp(0.2 * end, 0.75, 3.0);
    float q = d / core;
    float x = d / end;
    float w = 1.0 - x * x * x * x;
    return min(2.0 * w * w / (1.0 + q * q), 2.0);
}

// Whether something drawn on screen stands between p and the lamp at l: four
// points along the way projected into the march's depth, and one counted as
// hidden when the scene there is nearer the eye than it by more than a hand
// and by less than four yards - a wall, a pillar or a floor the line passes
// through, not something far in front of it. What the screen does not show
// cannot be asked, and is taken as open.
bool depthOccluded(vec3 p, vec3 l) {
    ivec2 size = ivec2(vl.screen.xy);
    for (int k = 1; k <= 4; ++k) {
        vec3 q = mix(p, l, float(k) / 5.0);
        vec4 clip = vl.viewProj * vec4(q, 1.0);
        if (clip.w <= 0.05) continue;
        vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) continue;
        ivec2 texel = clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1);
        float d = imageLoad(uLowDepth, texel).r;
        if (d <= 0.0) continue;  // sky
        vec3 s = unproject((vec2(texel) + 0.5) / vec2(size), d);
        float sceneDepth = dot(s - vl.camPos.xyz, vl.camForward.xyz);
        float pointDepth = dot(q - vl.camPos.xyz, vl.camForward.xyz);
        float gap = pointDepth - sceneDepth;
        if (gap > 0.25 && gap < 4.0) return true;
    }
    return false;
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = ivec2(vl.screen.xy);
    if (any(greaterThanEqual(pixel, size))) return;

    vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
    float depth = imageLoad(uLowDepth, pixel).r;
    vec3 eye = vl.camPos.xyz;
    // The sky has no surface: the ray runs to the far plane, which the reach
    // below cuts short anyway.
    vec3 surface = unproject(uv, max(depth, 1e-7));
    vec3 toSurface = surface - eye;
    float dist = length(toSurface);
    if (dist < 1e-3) {
        imageStore(uMarch, pixel, vec4(0.0));
        return;
    }
    vec3 dir = toSurface / dist;

    // The same fraction of a step for every sample of this texel, from a
    // fixed pattern over the screen (interleaved gradient noise).
    float offset = fract(52.9829189 * fract(dot(vec2(pixel), vec2(0.06711056, 0.00583715))));
#ifdef VL_TRACE
    uint rng = rtHash(uint(pixel.x) * 1973u + uint(pixel.y) * 9277u + 26699u);
#endif

    vec3 light = vec3(0.0);

    // The sun or the moon.
    if (vl.modes.y > 0 && vl.bodyDir.w > 0.0) {
        float reach = min(dist, vl.camPos.w);
        int steps = max(int(vl.air.z + 0.5), 1);
        float dt = reach / float(steps);
        vec3 toBody = normalize(vl.bodyDir.xyz);
        // Mostly forward, as off dust and haze: toward the sun the air takes
        // many times the light it does seen from the side. The lobe shrinks
        // while the disc itself is hidden from the eye (bodyColor.w). Its
        // peak held to six times even: straight at the sun the open air
        // would otherwise pile into a white disc round it, the look the
        // mist's shoulder was made against - the shafts live off its centre.
        float phase = mix(0.25, min(phaseHG(dot(dir, toBody), 0.7), 6.0), vl.bodyColor.w);
        float lit = 0.0;
        for (int i = 0; i < steps; ++i) {
            vec3 p = eye + dir * ((float(i) + offset) * dt);
            float v;
#ifdef VL_TRACE
            if (vl.modes.y == 2) {
                // The sun's ray from the air to the sky; whatever is opaque
                // stops it, a unit included.
                v = rtOccluded(p, toBody, 2000.0, rng) ? 0.0 : 1.0;
            } else
#endif
            {
                v = mapVisibility(p);
            }
            lit += v;
        }
        light += vl.bodyColor.rgb * (phase * lit * dt * vl.air.x * vl.bodyDir.w);
    }

    // The lamps.
    int lamps = min(vl.modes.x, 32);
    int lampSteps = max(int(vl.air.w + 0.5), 1);
    for (int i = 0; i < lamps; ++i) {
        vec3 lp = vl.lights[i].positionEnd.xyz;
        float end = abs(vl.lights[i].positionEnd.w);
        // Where the ray runs inside its reach (volumetric_light::segmentThroughSphere).
        vec3 oc = eye - lp;
        float b = dot(oc, dir);
        float c = dot(oc, oc) - end * end;
        float disc = b * b - c;
        if (disc <= 0.0) continue;
        float s = sqrt(disc);
        float t0 = max(-b - s, 0.0);
        float t1 = min(-b + s, dist);
        if (t1 <= t0) continue;
        float dt = (t1 - t0) / float(lampSteps);
        float sum = 0.0;
        for (int k = 0; k < lampSteps; ++k) {
            vec3 p = eye + dir * (t0 + (float(k) + offset) * dt);
            vec3 toLamp = lp - p;
            float d = length(toLamp);
            float f = vlGlowFalloff(d, end);
            if (f <= 0.0) continue;
            bool hidden = false;
#ifdef VL_TRACE
            if (vl.modes.z == 2) {
                if (d > 0.4) {
                    // Short of the lamp by its standoff, so its own lantern
                    // and the hand that holds it do not stop the ray; fixed
                    // lamps meet the fixtures by their faces turned toward
                    // the light only, as rt_trace's point light rays do.
                    rtSkipFixtures = int(vl.lights[i].storey.w + 0.5) != RT_LIGHT_KIND_UNIT &&
                                     vl.lights[i].storey.z > -0.5;
                    rtFixtureFacesOnly = true;
                    hidden = rtOccluded(p, toLamp / d, d - 0.35, rng);
                    rtFixtureFacesOnly = false;
                    rtSkipFixtures = false;
                }
            } else
#endif
            if (vl.modes.z == 1) {
                hidden = depthOccluded(p, lp);
            }
            if (!hidden) sum += f;
        }
        light += vl.lights[i].colorStart.rgb * (sum * dt * vl.air.y);
    }

    imageStore(uMarch, pixel, vec4(max(light, vec3(0.0)), 1.0));
}
