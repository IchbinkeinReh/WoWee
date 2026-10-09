#version 450

// One of a WMO group's minimap pictures into the indoor map's composite
// (Minimap::compositeIndoor). The four corners are composite coordinates and
// need not be square to the composite: a building can stand at any angle.

layout(push_constant) uniform Push {
    vec4 corners01;  // corner 0 (low x, low y of the picture), corner 1
    vec4 corners23;  // corner 2, corner 3
    vec4 uv01;
    vec4 uv23;
} push;

layout(location = 0) in vec2 aPos;

layout(location = 0) out vec2 TexCoord;

void main() {
    vec2 bottom = mix(push.corners01.xy, push.corners01.zw, aPos.x);
    vec2 top = mix(push.corners23.zw, push.corners23.xy, aPos.x);
    vec2 pos = mix(bottom, top, aPos.y);
    TexCoord = mix(mix(push.uv01.xy, push.uv01.zw, aPos.x),
                   mix(push.uv23.zw, push.uv23.xy, aPos.x), aPos.y);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
