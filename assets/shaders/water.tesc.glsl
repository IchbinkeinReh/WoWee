#version 450
// The tessellated water's control stage: how finely each of the liquid
// grid's triangles is cut, by how near the camera each of its edges is
// (water_tess.glsli). Triangles well outside the view are dropped.

layout(vertices = 3) out;

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
};

// The traced water's push constants (water.frag's), for the target's size.
layout(push_constant) uniform Push {
    vec4 closeColor;
    vec4 farColor;
    float waveAmp;
    float waveFreq;
    float waveSpeed;
    float liquidBasicType;
    vec2 screenSize;
    vec2 depthRange;
    float sceneValid;
    float lightColors;
    float pad1;
    float pad2;
    vec4 shore;
    // w the "Water tessellation" slider's density (water_tess.glsli's wtDensity).
    vec4 windBlend;
} push;

layout(location = 0) in vec3 vWorldPos[];
layout(location = 1) in vec2 vTexCoord[];
layout(location = 2) in vec4 vBed[];

layout(location = 0) out vec3 tcWorldPos[];
layout(location = 1) out vec2 tcTexCoord[];
layout(location = 2) out vec4 tcBed[];

#include "water_tess.glsli"

// How far outside the view a triangle may lie and still be drawn, yards: the
// waves move the surface (the physics waves' crests stand some 5 yards up in
// a 20 m/s wind on the open sea, twice that at the amplitude slider's top),
// and a triangle they lift into view must not have been dropped.
const float WT_CULL_MARGIN = 12.0;

float edgeLevel(vec3 a, vec3 b) {
    // The midpoint and the length are the same whichever end comes first.
    return waterTessLevel(length(a - b), length((a + b) * 0.5 - viewPos.xyz));
}

void main() {
    tcWorldPos[gl_InvocationID] = vWorldPos[gl_InvocationID];
    tcTexCoord[gl_InvocationID] = vTexCoord[gl_InvocationID];
    tcBed[gl_InvocationID] = vBed[gl_InvocationID];
    if (gl_InvocationID != 0) return;
    wtYardsPerPixel = wtYardsPerPixelFor(projection, push.screenSize.y);
    wtDensity = wtDensityFor(push.windBlend.w);

    vec3 p0 = vWorldPos[0];
    vec3 p1 = vWorldPos[1];
    vec3 p2 = vWorldPos[2];

    // The view's four side planes, from the rows of projection * view (w + x,
    // w - x, w + y, w - y); the near and far are left alone, the far being
    // the sky's. A triangle wholly beyond one of them by more than the margin
    // is not drawn: a level of 0 drops the patch. Behind the camera is
    // outside the sides as well.
    mat4 vp = projection * view;
    vec4 rowX = vec4(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
    vec4 rowY = vec4(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
    vec4 rowW = vec4(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);
    vec4 planes[4] = vec4[4](rowW + rowX, rowW - rowX, rowW + rowY, rowW - rowY);
    for (int i = 0; i < 4; ++i) {
        float reach = -WT_CULL_MARGIN * length(planes[i].xyz);
        if (dot(planes[i], vec4(p0, 1.0)) < reach && dot(planes[i], vec4(p1, 1.0)) < reach &&
            dot(planes[i], vec4(p2, 1.0)) < reach) {
            gl_TessLevelOuter[0] = 0.0;
            gl_TessLevelOuter[1] = 0.0;
            gl_TessLevelOuter[2] = 0.0;
            gl_TessLevelInner[0] = 0.0;
            return;
        }
    }

    // Outer level i is the edge facing corner i (water.tese weighs corner i
    // by gl_TessCoord[i]).
    float e0 = edgeLevel(p1, p2);
    float e1 = edgeLevel(p2, p0);
    float e2 = edgeLevel(p0, p1);
    gl_TessLevelOuter[0] = e0;
    gl_TessLevelOuter[1] = e1;
    gl_TessLevelOuter[2] = e2;
    gl_TessLevelInner[0] = max(e0, max(e1, e2));
}
