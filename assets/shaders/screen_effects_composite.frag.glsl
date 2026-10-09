#version 450

// The client's full-screen effects over the finished world (see
// screen_effects.hpp), one at a time, as ScreenEffect.dbc's row picks them
// (0x004f7020):
//
//   0 glow    FFXGlow.bls: mix(frame, blurred, z) + blurred^2 x w - w the
//             light's glow, z the camera in liquid or drunkenness (0x004f8770);
//             in liquid FFXGlowWave.bls, both read through the wave texture.
//   1 death   FFXDeath.bls (0x007e87b0).
//   2 nether  FFXNetherCombine.bls (0x007e8c80) over the streaked blur.
//   3 fog     FFXFogCombine.bls (0x007e9670) with the propagated fog.
//
// The minimap is interface, drawn after these in the client, and is left as
// it was.

layout(set = 0, binding = 0) uniform sampler2D uFrame;
layout(set = 0, binding = 1) uniform sampler2D uGlow;
layout(set = 0, binding = 2) uniform sampler2D uFogA;
layout(set = 0, binding = 3) uniform sampler2D uFogB;
layout(set = 0, binding = 4) uniform sampler2D uWave;

layout(push_constant) uniform Push {
    vec4 params;    // x = glow w, y = blend z, z = mode, w = fog target (0 A, 1 B)
    vec4 keepRect;  // uv x, y, w, h of the minimap; w 0 for none
    vec4 waveRow;   // the wave's texture matrix: x' = (x, y).uv, y' = (z, w).uv
    vec4 waveMove;  // xy its scroll; zw how far a wave texel moves the reads, 0 for no wave
    vec4 extra;     // nether: x fade; fog: x desaturate, y brighten
} push;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 frame = texture(uFrame, TexCoord).rgb;

    if (push.keepRect.z > 0.0) {
        vec2 centre = push.keepRect.xy + push.keepRect.zw * 0.5;
        vec2 d = (TexCoord - centre) / (push.keepRect.zw * 0.5);
        if (dot(d, d) <= 1.0) {
            outColor = vec4(frame, 1.0);
            return;
        }
    }

    int mode = int(push.params.z + 0.5);
    vec3 c;
    if (mode == 2) {
        // FFXNetherCombine: (frame / 2 + blur) halved, plus a sixth of its
        // sum, times (0.6, 0.6, 0.78) (0xaf4940), the frame mixed toward that
        // by the fade.
        vec3 blur = texture(uGlow, TexCoord).rgb;
        vec3 r = frame * 0.5 + blur;
        float l = dot(r, vec3(0.16666649));
        r = r * 0.5 + l;
        c = mix(frame, r * vec3(0.6, 0.6, 0.78), push.extra.x);
    } else if (mode == 3) {
        // FFXFogCombine: the frame desaturated toward a third of its sum and
        // brightened toward white, then the fog over it by its alpha. The fog
        // target wraps round the screen (0x007e8410, 0x007e8380): across it
        // the angle from the middle, acos(dx / r) / pi, down it r, sqrt 2
        // times the distance from the middle - 1 at the corners.
        vec2 d = (TexCoord - 0.5) * 1.4142137;
        float r = length(d);
        float u = r > 0.0 ? acos(clamp(d.x / r, -1.0, 1.0)) * 0.31830987 : 0.0;
        vec2 fogUv = vec2(u + 0.5 / 256.0, r + 0.5 / 128.0);
        vec4 fog = push.params.w > 0.5 ? texture(uFogB, fogUv) : texture(uFogA, fogUv);
        float l = dot(frame, vec3(0.333333));
        vec3 r1 = frame + clamp(push.extra.x, 0.0, 1.0) * (vec3(l) - frame);
        r1 = r1 + clamp(push.extra.y, 0.0, 1.0) * (vec3(1.0) - r1);
        c = mix(r1, fog.rgb, fog.a);
    } else {
        vec2 frameUv = TexCoord;
        vec2 glowUv = TexCoord;
        if (push.waveMove.z > 0.0) {
            // FFXGlowWave: the wave texture, turned, scaled and scrolled
            // (0x008c2350), moves the frame's read 3 of its texels and the
            // blur's 0.75 of its own - the same distance.
            vec2 wuv = vec2(dot(push.waveRow.xy, TexCoord), dot(push.waveRow.zw, TexCoord)) +
                       push.waveMove.xy;
            vec2 w = texture(uWave, wuv).xy;
            frameUv += w * push.waveMove.zw;
            glowUv += w * push.waveMove.zw;
            frame = texture(uFrame, frameUv).rgb;
        }
        vec3 g = texture(uGlow, glowUv).rgb;
        vec3 glowed = mix(frame, g, push.params.y) + g * g * push.params.x;
        c = clamp(glowed, 0.0, 1.0);
        if (mode == 1) {
            // FFXDeath, drawn in the glow's place: the glowed frame's
            // luminance (0.299, 0.587, 0.144), and the vertex colour
            // (0x53, 0x93, 0xa8) over it by 4 x lum x (1 - lum).
            const vec3 deathTint = vec3(0x53, 0x93, 0xa8) / 255.0;
            float lum = clamp(dot(frame + g * g * push.params.x, vec3(0.299, 0.587, 0.144)), 0.0, 1.0);
            c = clamp(vec3(lum) + deathTint * clamp(4.0 * lum * (1.0 - lum), 0.0, 1.0), 0.0, 1.0);
        }
    }
    outColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
