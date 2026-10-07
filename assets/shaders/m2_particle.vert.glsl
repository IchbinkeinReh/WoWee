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
    vec4 playerPos;
    vec4 playerWake;
    vec4 volumetricParams;  // x = on, y = near, z = 1 / ln(far / near), w = slices
};

layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(push_constant) uniform Push {
    vec2 tileCount;
    int alphaKey;
    int lit;      // lit by the scene: no M2 flag 0x1, blends 0-4 (FUN_0081fb10)
    int fogMode;  // 0 to black by alpha, 1 toward the fog, 2 none, 3 white, 4 grey
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in float aSize;
layout(location = 3) in float aTile;

layout(location = 0) out vec4 vColor;
layout(location = 1) out float vTile;
layout(location = 2) out float vFogVisibility;
layout(location = 3) out vec2 vCorner;
layout(location = 4) flat out vec3 vFogColor;

// The quad's corners in the order the client builds them (0x00b2d5b4): its
// strip runs down the left edge and up the right, each corner one size from
// the centre.
const vec2 kCorner[4] = vec2[4](vec2(-1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(1.0, -1.0));

// The air between the camera and this point, out of the fog volume: rgb is
// the light it scatters toward the camera, a how much of the point shows
// through it. See VolumetricFog.
vec4 fogVolumeAt(vec3 worldPos) {
    vec4 clip = projection * view * vec4(worldPos, 1.0);
    float depth = max(clip.w, 1e-4);
    vec2 uv = clip.xy / depth * 0.5 + 0.5;
    float slice = log(max(depth, volumetricParams.y) / volumetricParams.y) * volumetricParams.z;
    // Each slice holds the air up to its far edge, so a point is read half a
    // slice back from where it stands.
    return textureLod(uFogVolume, vec3(uv, slice - 0.5 / volumetricParams.w), 0.0);
}

void main() {
    // A camera-facing quad, as the client draws a particle: the centre moved
    // into view space, and each corner a size away from it there - so the quad
    // is two sizes across in yards, at any distance, with no point-size limit.
    const vec2 corner = kCorner[gl_VertexIndex];
    vec4 viewPos4 = view * vec4(aPos, 1.0);
    vCorner = vec2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
    viewPos4.xy += corner * aSize;
    vColor = aColor;
    // Lit as the client lights a particle (FUN_0097a390, FUN_0081fe90): a
    // white material whose normal is the view's third row - facing the
    // camera - under the scene's ambient and sun.
    if (push.lit != 0) {
        vec3 n = normalize(vec3(view[0][2], view[1][2], view[2][2]));
        vec3 light = ambientColor.rgb + lightColor.rgb * max(dot(n, normalize(-lightDir.xyz)), 0.0);
        vColor.rgb *= clamp(light, 0.0, 1.0);
    }
    vFogColor = push.fogMode == 1 ? fogColor.rgb : (push.fogMode == 3 ? vec3(1.0) : vec3(0.5));
    vTile = aTile;
    float worldDist = length(viewPos.xyz - aPos);
    float fogRange = max(fogParams.y - fogParams.x, 0.001);
    vFogVisibility = pow(clamp((fogParams.y - worldDist) / fogRange, 0.0, 1.0), max(fogColor.w, 1.0));
    // And thinned by the air in front of it, the way distance thins it:
    // a spark deep in a bank of mist is mostly the mist.
    if (volumetricParams.x > 0.5) vFogVisibility *= fogVolumeAt(aPos).a;
    gl_Position = projection * viewPos4;
}
