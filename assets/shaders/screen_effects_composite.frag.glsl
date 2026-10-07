#version 450

// The client's full-screen effects over the finished world (see
// screen_effects.hpp), as its FFXGlow and FFXDeath programs draw them: the
// blurred frame squared - so it is the bright parts that bloom - added at
// LightParams.Glow; and while the death light is up, FFXDeath in its place.
// The minimap is interface, drawn after these in the client, and is left as
// it was.

layout(set = 0, binding = 0) uniform sampler2D uFrame;
layout(set = 0, binding = 1) uniform sampler2D uGlow;

layout(push_constant) uniform Push {
    vec4 params;    // x = glow, y = death
    vec4 keepRect;  // uv x, y, w, h of the minimap; w 0 for none
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

    // FFXGlow.bls: mix(frame, blurred, z) + blurred^2 x w, the vertex colour
    // carrying w, the light's glow, and z, the player's own blend (0x004f8770
    // via 0x008bfde0), which is 0 but for a state not drawn here. Added, not
    // screened.
    vec3 g = push.params.x > 0.0 ? texture(uGlow, TexCoord).rgb : vec3(0.0);
    vec3 c = clamp(frame + g * g * push.params.x, 0.0, 1.0);
    if (push.params.y > 0.0) {
        // FFXDeath.bls, drawn in the glow's place (0x007e87b0): the glowed
        // frame's luminance (0.299, 0.587, 0.144), and the vertex colour
        // (0x53, 0x93, 0xa8) over it by 4 x lum x (1 - lum).
        const vec3 deathTint = vec3(0x53, 0x93, 0xa8) / 255.0;
        float lum = clamp(dot(frame + g * g * push.params.x, vec3(0.299, 0.587, 0.144)), 0.0, 1.0);
        c = clamp(vec3(lum) + deathTint * clamp(4.0 * lum * (1.0 - lum), 0.0, 1.0), 0.0, 1.0);
    }
    outColor = vec4(c, 1.0);
}
