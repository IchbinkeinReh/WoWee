#version 450

// The volumetric light, screened over the finished frame ahead of the
// interface - the blend does the screening: light + frame * (1 - light). The
// march's image is filtered up on the way; its light is soft and has nothing
// finer to lose. See volumetric_light.hpp.

layout(set = 0, binding = 0) uniform sampler2D uLight;

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
    outColor = vec4(shoulder(max(texture(uLight, TexCoord).rgb, vec3(0.0))), 0.0);
}
