#version 450

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform Push {
    mat4 lightSpaceModel;
    vec4 sway;
    ivec4 flags;            // x useTexture, y alphaTest, z unused
    vec4 wind;
} push;

layout(location = 0) in vec2 TexCoord;

// A value in [0, 1) for a cell of the texture, the same every frame.
float cellHash(uvec2 c) {
    uint h = c.x * 0x8da6b343u ^ c.y * 0xd8163841u;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return float(h >> 8) * (1.0 / 16777216.0);
}

void main() {
    if (push.flags.x != 0) {
        // Mipmapped: read at the base level, a leaf sheet a few texels wide
        // in the map pulled its whole top level through the texture cache,
        // and that was most of the shadow pass (7 of 10 ms at three 8192
        // cascades).
        //
        // A mip's alpha is the share of the leaf sheet that is leaf under
        // it. Each texel of the map is opaque with that chance - the cut-off
        // is a hash of the mip texel the point lies in, so it stays put from
        // frame to frame and the filter averages the canopy to the light it
        // lets through, as the ray-traced shadows and the base-level read
        // had it. Scaling that alpha up by the level instead (1 + lod / 4
        // against 0.5) made a canopy solid wherever a texel of the map took
        // in a few of the sheet's: with one cascade over 500 yards that was
        // every tree, and Teldrassil lay wholly in shadow (T447). At the
        // base level the cut-off is the plain 0.5.
        vec4 texColor = texture(uTexture, TexCoord);
        if (push.flags.y != 0) {
            float lod = max(textureQueryLod(uTexture, TexCoord).x, 0.0);
            float cutoff = 0.5;
            if (lod > 0.0) {
                float level = floor(lod);
                vec2 cells = vec2(textureSize(uTexture, 0)) / exp2(level);
                uvec2 cell = uvec2(ivec2(floor(TexCoord * max(cells, vec2(1.0)))) & ivec2(0xffff)) +
                                   uvec2(uint(level) << 16);
                cutoff = mix(0.5, max(cellHash(cell), 1.0 / 256.0), min(lod, 1.0));
            }
            if (texColor.a < cutoff) discard;
        }
    }
}
