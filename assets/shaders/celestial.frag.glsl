#version 450

// The sun, the moons and their glare: the texture in the push colour.
// A body is cut at the horizon and faded in as 0x007edee0 sets its vertex
// alpha: 2.5 x the height for vertices within 0.4 units of the horizon, the
// colour's alpha (1 - storm) above, a row at 1 on the 0.4 line when the quad
// straddles it. daynight::celestialQuadAlpha is the same, for the tests.
// The glare is neither cut nor faded (0x009ac400).
layout(push_constant) uniform Push {
    vec4 dirSize;
    vec4 color;
    vec4 params;
} push;

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(location = 0) in vec2 TexCoord;
layout(location = 1) in float vHeight;

layout(location = 0) out vec4 outColor;

float low(float y) { return clamp(y * 2.5, 0.0, 1.0); }

float bodyAlpha(float h) {
    float a = push.color.a;
    float centre = push.dirSize.z * 12.0;
    float top = centre + push.dirSize.w * 0.5;
    float bottom = max(centre - push.dirSize.w * 0.5, 0.0);
    bool topLow = top - 0.4 < 0.001;
    bool bottomLow = bottom - 0.4 < 0.001;
    if (!topLow && bottomLow) {
        if (h <= 0.4) return low(h);
        return mix(1.0, a, clamp((h - 0.4) / (top - 0.4), 0.0, 1.0));
    }
    float span = top - bottom;
    float f = span > 0.0 ? clamp((h - bottom) / span, 0.0, 1.0) : 0.0;
    float aBottom = bottomLow ? low(bottom) : a;
    float aTop = topLow ? low(top) : a;
    return mix(aBottom, aTop, f);
}

void main() {
    float vertexAlpha = push.color.a;
    if (push.params.x > 0.5) {
        if (vHeight < 0.0) discard;
        vertexAlpha = bodyAlpha(vHeight);
    }
    vec4 tex = texture(uTexture, TexCoord);
    float alpha = tex.a * vertexAlpha;
    if (alpha < 0.002) discard;
    outColor = vec4(tex.rgb * push.color.rgb, alpha);
}
