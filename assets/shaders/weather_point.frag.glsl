#version 450

// Snow's flakes are SnowFlake01 on a point sprite by the vertex colour; sand
// is the colour alone (0x0078bee0 binds no texture). Drawn alpha blended with
// that mode's 1/255 alpha reference (0x00ad8b7c).

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform Push {
    vec4 color;
    vec4 params;  // y = 0 snow, 1 sand
} push;

layout(location = 0) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 c = push.params.y < 0.5 ? texture(uTexture, gl_PointCoord) * vColor : vColor;
    if (c.a < 1.0 / 255.0) discard;
    outColor = c;
}
