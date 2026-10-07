#version 450

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform Push {
    vec2 tileCount;
    int alphaKey;
    int lit;      // lit by the scene: no M2 flag 0x1, blends 0-4 (FUN_0081fb10)
    int fogMode;  // 0 to black by alpha, 1 toward the fog, 2 none, 3 white, 4 grey
} push;

layout(location = 0) in vec4 vColor;
layout(location = 1) in float vTile;
layout(location = 2) in float vFogVisibility;
layout(location = 3) in vec2 vCorner;
layout(location = 4) flat in vec3 vFogColor;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 p = vCorner;
    float tile = floor(vTile);
    float tx = mod(tile, push.tileCount.x);
    float ty = floor(tile / push.tileCount.x);
    vec2 uv = (vec2(tx, ty) + p) / push.tileCount;
    vec4 texColor = texture(uTexture, uv);

    // An alpha-key particle keeps what passes the client's reference, 224/255
    // of its alpha (FUN_0081fe90).
    if (push.alphaKey != 0 && texColor.a * vColor.a < 224.0 / 255.0) discard;

    // The whole of the texture, to its edges, as the client draws it: the
    // circular falloff this had was a point sprite's.
    float alpha = texColor.a * vColor.a;
    // The colour goes out as it is: both particle pipelines blend with SRC_ALPHA,
    // which is where the alpha comes in. Multiplying it in here as well - as this
    // did - applied it twice, so every particle added alpha squared and was
    // drawn dimmer than the client draws it.
    vec3 rgb = texColor.rgb * vColor.rgb;
    // Fogged by blend (table 0x00a45390): an add fades to black, which is
    // its alpha going; the rest move toward their fog colour; 0x8 has none.
    if (push.fogMode == 0) alpha *= vFogVisibility;
    else if (push.fogMode != 2) rgb = mix(vFogColor, rgb, vFogVisibility);
    outColor = vec4(rgb, alpha);
}
