#version 450

// The client's full-screen effects over the finished world (see
// screen_effects.hpp): the glow, its blurred frame squared - so it is the
// bright parts that bloom - screened on at LightParams.Glow; and while the
// death light is up, the frame desaturated toward ffxDeath's constant colour
// (0x53, 0x93, 0xa8; Wow.exe 3.3.5a 0x007e87b0). The minimap is interface,
// drawn after these in the client, and is left as it was.

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

    vec3 c = frame;
    if (push.params.x > 0.0) {
        vec3 g = texture(uGlow, TexCoord).rgb;
        c = 1.0 - (1.0 - c) * (1.0 - clamp(g * g * push.params.x, 0.0, 1.0));
    }
    if (push.params.y > 0.0) {
        const vec3 deathTint = vec3(0x53, 0x93, 0xa8) / 255.0;
        const vec3 luma = vec3(0.299, 0.587, 0.114);
        vec3 grey = vec3(dot(c, luma)) * deathTint / dot(deathTint, luma);
        c = mix(c, grey, clamp(push.params.y, 0.0, 1.0));
    }
    outColor = vec4(c, 1.0);
}
