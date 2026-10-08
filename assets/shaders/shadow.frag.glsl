#version 450

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform Push {
    mat4 lightSpaceModel;
    vec4 sway;
    ivec4 flags;            // x useTexture, y alphaTest, z unused
    vec4 wind;
} push;

layout(location = 0) in vec2 TexCoord;

void main() {
    if (push.flags.x != 0) {
        // Mipmapped: read at the base level, a leaf sheet a few texels wide
        // in the map pulled its whole top level through the texture cache,
        // and that was most of the shadow pass (7 of 10 ms at three 8192
        // cascades). A mip's averaged alpha thins a cutout, so it is scaled
        // back up by the level, which keeps the canopy about as dense as the
        // base level drew it.
        vec4 texColor = texture(uTexture, TexCoord);
        float lod = max(textureQueryLod(uTexture, TexCoord).x, 0.0);
        if (push.flags.y != 0 && texColor.a * (1.0 + 0.25 * lod) < 0.5) discard;
    }
}
