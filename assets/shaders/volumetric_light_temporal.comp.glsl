#version 450

// The volumetric light over the last few frames (U23: the user allowed it,
// 2026-11-27 - the lamps' glow was grainy). Each texel's light of this frame,
// blurred (volumetric_light_blur.comp.glsl), is blended into what the frames
// before left where the same air was last frame:
//
//   found     the texel's surface (or, for the sky, its direction far out)
//             taken to last frame's screen (prevViewProj), and the four
//             texels round it in the history read, each only if the distance
//             it kept from last frame's eye agrees with the distance from
//             last frame's eye to this surface
//             (volumetric_light::historyDepthAgrees) - so what a unit
//             walking by or a door swinging uncovers takes no old light;
//   held      the old light kept inside the range of this frame's light in
//             the 3x3 round the texel, so a lamp carried away or a head
//             turned leaves no trail;
//   blended   at volumetric_light::kHistoryWeight, 0.2: about five frames.
//
// With no history to build on (screen.w 0: the first frame, after a cut or
// a teleport, a new size or quality - volumetric_light::historyUsable) the
// light is this frame's alone. Beside its light each texel keeps the
// distance from this frame's eye to what it shows (the sky's
// kHistorySkyDistance), for the next frame's test. See volumetric_light.hpp.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

struct VLLight {
    vec4 positionEnd;
    vec4 colorStart;
    vec4 storey;
};

// VolumetricLight's VLParamsGPU (std140), as volumetric_light.comp.glsl has
// it, and its two last members.
layout(set = 1, binding = 0) uniform VLParams {
    mat4 invViewProj;
    mat4 viewProj;
    vec4 camPos;
    vec4 camForward;
    vec4 bodyDir;
    vec4 bodyColor;
    vec4 air;
    ivec4 modes;
    vec4 screen;        // xy = the march's size, z = the frame's number, w = history usable
    VLLight lights[32];
    mat4 prevViewProj;  // last frame's
    vec4 prevCamPos;    // last frame's eye
} vl;

layout(set = 1, binding = 3, r32f) uniform readonly image2D uLowDepth;
layout(set = 1, binding = 5, rgba16f) uniform readonly image2D uBlurred;
layout(set = 1, binding = 6, rgba16f) uniform readonly image2D uHistoryIn;
layout(set = 1, binding = 7, rgba16f) uniform writeonly image2D uHistoryOut;

const float kHistoryWeight = 0.2;          // volumetric_light::kHistoryWeight
const float kSkyDistance = 60000.0;        // volumetric_light::kHistorySkyDistance

vec3 unproject(vec2 uv, float depth) {
    vec4 w = vl.invViewProj * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return w.xyz / w.w;
}

// volumetric_light::historyDepthAgrees.
bool depthAgrees(float expected, float kept) {
    if (!(expected > 0.0) || !(kept > 0.0)) return false;
    return abs(expected - kept) <= 0.05 * max(expected, kept) + 0.1;
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(uHistoryOut);
    if (any(greaterThanEqual(pixel, size))) return;

    vec3 current = imageLoad(uBlurred, pixel).rgb;
    vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
    float depth = imageLoad(uLowDepth, pixel).r;
    bool sky = depth <= 0.0;
    vec3 eye = vl.camPos.xyz;
    vec3 world;
    if (sky) {
        // Its direction only: far enough out that only the turn of the view
        // moves it.
        vec3 far = unproject(uv, 1e-7);
        world = eye + normalize(far - eye) * 5000.0;
    } else {
        world = unproject(uv, depth);
    }
    float keep = sky ? kSkyDistance : length(world - eye);

    vec3 result = current;
    if (vl.screen.w > 0.5) {
        vec4 clip = vl.prevViewProj * vec4(world, 1.0);
        if (clip.w > 1e-4) {
            vec2 prevUv = clip.xy / clip.w * 0.5 + 0.5;
            if (all(greaterThanEqual(prevUv, vec2(0.0))) && all(lessThanEqual(prevUv, vec2(1.0)))) {
                float expected = sky ? kSkyDistance : length(world - vl.prevCamPos.xyz);
                // The four texels round it, each weighed bilinearly and only
                // if it kept this surface's distance.
                vec2 at = prevUv * vec2(size) - 0.5;
                ivec2 base = ivec2(floor(at));
                vec2 f = at - vec2(base);
                vec3 sum = vec3(0.0);
                float weight = 0.0;
                for (int j = 0; j <= 1; ++j) {
                    for (int i = 0; i <= 1; ++i) {
                        ivec2 q = base + ivec2(i, j);
                        if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, size))) continue;
                        vec4 h = imageLoad(uHistoryIn, q);
                        if (!depthAgrees(expected, h.a)) continue;
                        float w = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
                        sum += h.rgb * w;
                        weight += w;
                    }
                }
                if (weight > 0.05) {
                    vec3 history = sum / weight;
                    // Held to this frame's light round the texel.
                    vec3 lo = current;
                    vec3 hi = current;
                    for (int j = -1; j <= 1; ++j) {
                        for (int i = -1; i <= 1; ++i) {
                            ivec2 q = clamp(pixel + ivec2(i, j), ivec2(0), size - 1);
                            vec3 c = imageLoad(uBlurred, q).rgb;
                            lo = min(lo, c);
                            hi = max(hi, c);
                        }
                    }
                    history = clamp(history, lo, hi);
                    // volumetric_light::historyBlend.
                    result = history + (current - history) * kHistoryWeight;
                }
            }
        }
    }
    imageStore(uHistoryOut, pixel, vec4(max(result, vec3(0.0)), keep));
}
