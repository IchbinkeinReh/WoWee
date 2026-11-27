#version 450

// The volumetric light, screened over the finished frame ahead of the
// interface - the blend does the screening: light + frame * (1 - light). The
// march's image is filtered up on the way; its light is soft and has nothing
// finer to lose. See volumetric_light.hpp.

layout(set = 0, binding = 0) uniform sampler2D uLight;

// The minimap's place when it was drawn with the scene (uv x, y, w, h; w 0
// for none): interface, not air, so the light is left off it (T435) - as
// the screen's glow leaves it (screen_effects_composite.frag.glsl).
//
// mode.x 1 is the debug view "VL only" (dbgvlonly, U23): the light alone,
// drawn over the picture with no blend - black where there is none - times
// mode.y (volumetric_light::kDebugViewGain, ten) so faint light shows,
// through the same shoulder (volumetric_light::debugViewColor). The minimap's
// disc is then left as it was drawn.
layout(push_constant) uniform Push {
    vec4 keepRect;
    vec4 mode;
} push;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

// Brought under white without changing its hue, smooth from the knee up -
// the mist's airLightShoulder (volumetric_fog_inject.comp.glsl,
// fogAirLightShoulder): a lamp's core or the air toward a low sun keeps its
// colour instead of clipping channel by channel into a flat disc.
vec3 shoulder(vec3 light) {
    const float knee = 0.4;
    float peak = max(max(light.r, light.g), light.b);
    if (peak <= knee) return light;
    float over = (peak - knee) / (1.0 - knee);
    float shaped = knee + (1.0 - knee) * over / (1.0 + over);
    return light * (shaped / peak);
}

void main() {
    if (push.keepRect.z > 0.0) {
        vec2 centre = push.keepRect.xy + push.keepRect.zw * 0.5;
        vec2 d = (TexCoord - centre) / (push.keepRect.zw * 0.5);
        if (dot(d, d) <= 1.0) {
            if (push.mode.x > 0.5) discard;
            outColor = vec4(0.0);
            return;
        }
    }
    vec3 light = max(texture(uLight, TexCoord).rgb, vec3(0.0));
    if (push.mode.x > 0.5) light *= push.mode.y;
    outColor = vec4(shoulder(light), 0.0);
}
