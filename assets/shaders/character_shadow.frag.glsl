#version 450

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(set = 0, binding = 1) uniform ShadowParams {
    int alphaTest;
    int unused0;   // was the black colour key: the client never keys by colour
};

layout(location = 0) in vec2 TexCoord;

void main() {
    vec4 texColor = texture(uTexture, TexCoord);
    if (alphaTest != 0 && texColor.a < 0.5) discard;
}
