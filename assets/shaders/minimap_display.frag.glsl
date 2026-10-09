#version 450

layout(set = 0, binding = 0) uniform sampler2D uComposite;
// Textures\MinimapMask: the client draws the map through its alpha in a
// second texture stage (0x00581740), the frame's border art over it.
layout(set = 1, binding = 0) uniform sampler2D uMask;

layout(push_constant) uniform Push {
    vec4 rect;
    vec2 playerUV;
    float rotation;
    float zoomRadius;
    int squareShape;
    float opacity;
    int hasMask;
} push;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 center = TexCoord - 0.5;
    float dist = length(center);

    float maskAlpha = 1.0;
    if (push.squareShape == 0) {
        if (push.hasMask != 0) maskAlpha = texture(uMask, TexCoord).a;
        else if (dist > 0.5) discard;
    }

    float cs = cos(push.rotation);
    float sn = sin(push.rotation);
    vec2 mapCenter = vec2(-center.x, center.y);
    vec2 rotated = vec2(mapCenter.x * cs - mapCenter.y * sn, mapCenter.x * sn + mapCenter.y * cs);
    vec2 mapUV = push.playerUV + vec2(rotated.y, -rotated.x) * push.zoomRadius * 2.0;

    vec4 mapColor = texture(uComposite, mapUV);

    outColor = vec4(mapColor.rgb, mapColor.a * maskAlpha * push.opacity);
}
