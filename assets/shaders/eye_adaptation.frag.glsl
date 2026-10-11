#version 450

// The eye's exposure over the finished frame, ahead of the interface. The
// blend multiplies: source by destination plus destination by source, twice
// the product, so half the exposure goes out here. See eye_adaptation_pass.hpp.

layout(push_constant) uniform Push {
    float halfExposure;
} pc;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(vec3(pc.halfExposure), 0.0);
}
