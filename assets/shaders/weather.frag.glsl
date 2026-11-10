#version 450

// Rain drops and their splashes: the texture by the vertex colour, the
// fixed-function stage's modulate. Drawn Mod2x with the 1/255 alpha reference
// that blend mode carries (0x00ad8b7c), which is also what hides a drop that
// is not alive.

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in float vFog;  // the share left by the fog (weather_streak.vert)

layout(location = 0) out vec4 outColor;

void main() {
    vec4 c = texture(uTexture, vUV) * vColor;
    if (c.a < 1.0 / 255.0) discard;
    // Fogged toward the mid grey Mod2x leaves the picture alone with.
    outColor = vec4(mix(vec3(0.5), c.rgb, vFog), c.a);
}
