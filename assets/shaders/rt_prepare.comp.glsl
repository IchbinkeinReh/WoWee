#version 460
// The scene's depth and the opaque surfaces' G-buffer -> the tracer's
// G-buffer, at trace resolution: per 2x2 block of scene pixels (per pixel
// when tracing at full resolution) the nearest or, on every other texel, the
// farthest surface of the tracing's (RT_GB_VALID; any surface where the block has
// none of those, so the ground under grass is still traced), its normal
// rebuilt from the depth, its distance from the camera (0 for sky), which
// pixel of the block it is and whether it is the tracing's (rtGbufValid).
//
// The normal is the geometric one, from the depth, rather than the shading
// normal the G-buffer holds: a ray leaves along it, and a smoothed or
// normal-mapped normal points rays into the surface they start from. The
// point lights take the shading normal, which goes to rt_trace in the image
// it writes the point lights' light to (uPointNormal), turned to the side
// of the geometric face (not toward the camera: see below), and beside it
// whether the surface is a building's baked colour (rtGbufStoreyBound),
// which the building's lights shade from their own storey only (rt_trace,
// rt_light_storey.hpp).
//
// Built with RT_MSAA when the scene is multisampled; sample 0 stands for the
// pixel then.

#include "rt_common.glsli"

#ifdef RT_MSAA
layout(set = 0, binding = 1) uniform sampler2DMS uSceneDepth;
layout(set = 0, binding = 13) uniform sampler2DMS uSceneGbuf0;
layout(set = 0, binding = 14) uniform sampler2DMS uSceneGbuf1;
#else
layout(set = 0, binding = 1) uniform sampler2D uSceneDepth;
layout(set = 0, binding = 13) uniform sampler2D uSceneGbuf0;
layout(set = 0, binding = 14) uniform sampler2D uSceneGbuf1;
#endif
layout(set = 0, binding = 2, rgba16f) uniform writeonly image2D uGbuf;
layout(set = 0, binding = 11, rgba16f) uniform writeonly image2D uPointNormal;

layout(local_size_x = 8, local_size_y = 8) in;

float loadDepth(ivec2 p) {
    p = clamp(p, ivec2(0), ivec2(pass.sceneExtent.xy) - 1);
    return texelFetch(uSceneDepth, p, 0).r;
}

bool surfaceValid(ivec2 p) {
    return (rtGbufFlags(texelFetch(uSceneGbuf0, p, 0)) & RT_GB_VALID) != 0u;
}

// Relative to the camera (rtRelFromDepth): the normal is made from the
// differences between neighbours, which world coordinates would round away.
vec3 relAt(ivec2 p, float depth) {
    return rtRelFromDepth((vec2(p) + 0.5) * pass.sceneExtent.zw, depth);
}

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(px, ivec2(pass.extent.xy)))) return;

    // A surface of the tracing's in this texel's block of scene pixels, else
    // any: the nearest, or the farthest on every other texel in a
    // checkerboard fixed to the screen (rtTakesFarthest), so that along an
    // edge both the object in front and what lies behind it keep texels of
    // their own (rt_upsample.glsli). The sky is never taken while there is
    // a surface.
    // At full resolution the block is the pixel itself (rtBlockSize).
    ivec2 base = rtBlockBase(px);
    int sub = 0;
    float d = 0.0;  // reversed depth: 0 is the sky, nearer is larger
    bool valid = false;
    bool found = false;
    const int samples = rtBlockSize() == 1 ? 1 : 4;
    const bool farthest = rtTakesFarthest(px);
    for (int i = 0; i < samples; ++i) {
        ivec2 sp = clamp(base + ivec2(i & 1, i >> 1), ivec2(0), ivec2(pass.sceneExtent.xy) - 1);
        float di = loadDepth(sp);
        if (di <= 0.0) continue;
        bool vi = surfaceValid(sp);
        if (rtBlockPrefers(farthest, found, d, valid, di, vi)) {
            d = di;
            sub = i;
            valid = vi;
            found = true;
        }
    }
    if (d <= 0.0) {
        imageStore(uGbuf, px, vec4(0.0, 0.0, 0.0, 0.0));
        return;
    }
    ivec2 sp = base + ivec2(sub & 1, sub >> 1);
    vec3 p = relAt(sp, d);

    // Pick, on each axis, the neighbour on the same surface - the one whose
    // depth continues the slope best - so edges do not bend the normal.
    float dl = loadDepth(sp + ivec2(-1, 0)), dr = loadDepth(sp + ivec2(1, 0));
    float dd = loadDepth(sp + ivec2(0, -1)), du = loadDepth(sp + ivec2(0, 1));
    vec3 dx = abs(dr - d) < abs(d - dl) ? relAt(sp + ivec2(1, 0), dr) - p
                                        : p - relAt(sp + ivec2(-1, 0), dl);
    vec3 dy = abs(du - d) < abs(d - dd) ? relAt(sp + ivec2(0, 1), du) - p
                                        : p - relAt(sp + ivec2(0, -1), dd);
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, p) > 0.0) n = -n;

    // The normal the point lights' N.L takes: the surface's own (its vertex
    // normal, a normal map's), turned to the side of the geometric face, and
    // the geometric one where the two disagree by more than 60 degrees.
    // Turned by the view instead - toward the camera - a normal that leans
    // across the face flipped as the camera moved, and lit or blacked out
    // patches of floor with it: the Goldshire inn's interior floors carry
    // vertex normals tilted far off up near the walls and doors (averaged
    // with the walls'), and the patches turned with the camera.
    vec3 shading = n;
    uint storeyBound = 0u;
    if (valid) {
        vec4 gb0 = texelFetch(uSceneGbuf0, sp, 0);
        shading = rtGbufShadingNormal(gb0);
        if (dot(shading, n) < 0.0) shading = -shading;
        if (dot(shading, n) < 0.5) shading = n;
        storeyBound = rtGbufStoreyBound(gb0, texelFetch(uSceneGbuf1, sp, 0));
    }
    imageStore(uPointNormal, px, vec4(rtOctEncode(shading), float(storeyBound), 0.0));
    imageStore(uGbuf, px, vec4(rtOctEncode(n), length(p), float(sub) + (valid ? 4.0 : 0.0)));
}
