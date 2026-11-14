#version 450

// The horizon past the far clip: a .wdl tile in world coordinates
// (0x007d5150), drawn in a projection of its own (0x00791170). Its near plane
// is the far clip less 50 and its far plane the scale times the far clip; the
// depth it writes runs from planes.z at the near plane to planes.w at the far
// one, below every depth the world writes and above the sky's, as the
// client's viewport depth range 0.998..0.999 puts it behind the world's
// 0..0.94 (0x00795f80).

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
};

layout(push_constant) uniform Horizon {
    vec4 planes;  // x near, y far, z depth at near, w depth at far
} horizon;

layout(location = 0) in vec3 aPos;

// Where on screen it lies, for the fog volume (horizon.frag).
layout(location = 0) out vec3 vWorldPos;

out float gl_ClipDistance[2];

void main() {
    vec4 viewSpace = view * vec4(aPos, 1.0);
    vec4 clip = projection * viewSpace;
    float w = clip.w;  // the view depth
    float n = horizon.planes.x;
    float f = horizon.planes.y;
    gl_ClipDistance[0] = w - n;
    gl_ClipDistance[1] = f - w;
    // The standard depth of its own projection, (f / (f - n)) (1 - n / w),
    // taken from 1 and laid between the two depths: z / w is that, and z
    // linear in w keeps it right across the triangle on screen.
    float k = (horizon.planes.z - horizon.planes.w) * n / (f - n);
    clip.z = horizon.planes.w * w + k * (f - w);
    gl_Position = clip;
    vWorldPos = aPos;
}
