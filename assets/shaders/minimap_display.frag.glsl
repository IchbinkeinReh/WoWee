#version 450

layout(set = 0, binding = 0) uniform sampler2D uComposite;

layout(push_constant) uniform Push {
    vec4 rect;
    vec2 playerUV;
    float rotation;
    float zoomRadius;
    int squareShape;
    float opacity;
} push;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 center = TexCoord - 0.5;
    float dist = length(center);

    if (push.squareShape == 0) {
        if (dist > 0.5) discard;
    }

    float cs = cos(push.rotation);
    float sn = sin(push.rotation);
    vec2 mapCenter = vec2(-center.x, center.y);
    vec2 rotated = vec2(mapCenter.x * cs - mapCenter.y * sn, mapCenter.x * sn + mapCenter.y * cs);
    vec2 mapUV = push.playerUV + vec2(rotated.y, -rotated.x) * push.zoomRadius * 2.0;

    vec4 mapColor = texture(uComposite, mapUV);

    // Dark border ring
    float border = smoothstep(0.48, 0.5, dist);
    if (push.squareShape == 0) {
        mapColor.rgb *= 1.0 - border * 0.7;
    }

    outColor = vec4(mapColor.rgb, mapColor.a * push.opacity);
}
