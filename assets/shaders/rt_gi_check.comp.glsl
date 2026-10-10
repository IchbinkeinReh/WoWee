#version 450
// The probe volume as a surface reads it, at points a test chooses
// (tools/rt_probe.cpp). The same rt_gi.glsli functions the terrain, building,
// doodad and character shaders call through rt_lighting.glsli, so what the
// test sees is what the screen gets. Not used by the client.

#include "rt_gi.glsli"

layout(set = 0, binding = 0) uniform sampler2D uAtlas;
layout(std140, set = 0, binding = 1) uniform Check {
    vec4 vol[2];
    vec4 volMod[2];
    vec4 sky;     // w strength
    vec4 ground;  // w largest ratio
    vec4 params;  // x chroma
} c;
// Per point: position, normal, direction toward the viewer.
layout(std430, set = 0, binding = 2) readonly buffer Points { vec4 pts[]; };
// Per point: the irradiance (w its cover), then the ratio.
layout(std430, set = 0, binding = 3) writeonly buffer Results { vec4 results[]; };

layout(local_size_x = 1) in;

void main() {
    uint i = gl_WorkGroupID.x;
    vec3 p = pts[i * 3u].xyz;
    vec3 n = normalize(pts[i * 3u + 1u].xyz);
    vec3 v = normalize(pts[i * 3u + 2u].xyz);
    vec4 e = giIrradiance(uAtlas, c.vol[0], c.volMod[0], c.vol[1], c.volMod[1], p, n, v);
    results[i * 2u] = e;
    float lift = texelFetch(uAtlas, GI_EXPOSURE_TEXEL, 0).r;
    results[i * 2u + 1u] = vec4(giAmbientRatio(e, c.sky.rgb, c.ground.rgb, n, c.sky.w, c.ground.w,
                                               c.params.x, lift > 0.0 ? lift : 1.0),
                                lift);
}
