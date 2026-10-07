#version 450

// vsLiquidProcWater (CMaterialProcWater 0x008a48f0; the nvvp3 program, whose
// four variants are one). The surface is not moved: three circular and
// three plane waves (c46-c57, 0x008a3620 and 0x008a3710) are summed at the
// vertex and half a unit along x and y, and the differences tilt the
// tangent frame the pixel program turns its normals by.

#include "liquid_proc.glsli"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aSurfaceUV;  // attribute 6
layout(location = 2) in vec2 aDepthUV;    // attribute 7
layout(location = 3) in vec4 aColor;      // unread

layout(location = 0) out vec4 vTex0;  // texcoord[0]: xy Texture2's coordinate
layout(location = 1) out vec4 vTex1;  // texcoord[1]: xy and zw Texture3's two
layout(location = 2) out vec4 vTex2;  // texcoord[2]: xy the depth coordinate, zw the mask's
layout(location = 3) out vec3 vPos;   // texcoord[3]: the position
layout(location = 4) out vec3 vT;     // texcoord[4]
layout(location = 5) out vec3 vB;     // texcoord[5]
layout(location = 6) out vec3 vN;     // texcoord[6]
layout(location = 7) out float vFog;

// The waves' height at p, offset by `off`: a circular wave by its distance
// from its centre, a plane wave by the scaled position along its direction,
// each fading out with distance from its centre.
float height(vec2 p, vec2 off) {
    float h = 0.0;
    for (int i = 0; i < 3; ++i) {
        float d = length(p + off - pw.circle[i].xy);
        h += sin(d * pw.circle[i].z - pw.c46[i]) * clamp(1.0 - d * pw.c51[i], 0.0, 1.0) * pw.circle[i].w;
    }
    for (int i = 0; i < 3; ++i) {
        float d = length(p + off - pw.plane[i].xy);
        float a = dot(p * pw.c56[i] + off, pw.plane[i].zw) - pw.c47[i];
        h += sin(a) * clamp(1.0 - d * pw.c52[i], 0.0, 1.0) * pw.c57[i];
    }
    return h;
}

vec2 stage(vec4 m, vec2 uv) { return mat2(m.xy, m.zw) * uv; }

void main() {
    vec4 viewSpace = view * vec4(aPos, 1.0);
    gl_Position = projection * viewSpace;

    vec3 P = toClient(aPos);
    float h0 = height(P.xy, vec2(0.0));
    float hx = height(P.xy, vec2(0.5, 0.0));
    float hy = height(P.xy, vec2(0.0, 0.5));
    vec3 T = normalize(vec3(1.0, 0.0, hx - h0));
    vec3 B = normalize(vec3(0.0, 1.0, hy - h0));
    vT = T;
    vB = B;
    vN = cross(T, B);
    vPos = P;

    vTex0 = vec4(stage(pw.normalMat[0], aSurfaceUV), stage(pw.normalMat[1], aSurfaceUV));
    vTex1 = vec4(stage(pw.normalMat[2], aSurfaceUV), stage(pw.normalMat[3], aSurfaceUV));
    vTex2 = vec4(aDepthUV.x, aDepthUV.y * pw.depthParams.x, stage(pw.maskMat, aSurfaceUV));

    // The fog factor, by view depth (c4), as liquid.vert.glsl.
    float depth = -viewSpace.z;
    float f = max((fogParams.y - depth) / max(fogParams.y - fogParams.x, 1e-4), 0.0);
    vFog = min(pow(f, max(fogColor.w, 1.0)), 1.0);
}
