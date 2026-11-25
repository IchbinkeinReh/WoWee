#version 450

// WaterPoop02.blp as it is, unlit; in the scene's fog where the liquid says so
// (LiquidType flag 0x10), as ((end - depth) / (end - start))^exponent (0x00873210).

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

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform Push { vec4 params; };

layout(location = 0) in vec2 vUV;
layout(location = 1) in float vDepth;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 c = texture(uTexture, vUV);
    if (c.a < 1.0 / 255.0) discard;
    if (params.x > 0.5) {
        float f = pow(clamp((fogParams.y - vDepth) / (fogParams.y - fogParams.x), 0.0, 1.0), max(fogColor.w, 1.0));
        c.rgb = mix(fogColor.rgb, c.rgb, f);
    }
    outColor = c;
}
