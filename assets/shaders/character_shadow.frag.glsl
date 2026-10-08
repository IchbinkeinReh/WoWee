#version 450

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(set = 0, binding = 1) uniform ShadowParams {
    int alphaTest;
    int unused0;   // was the black colour key: the client never keys by colour
};

layout(location = 0) in vec2 TexCoord;

void main() {
    vec4 texColor = texture(uTexture, TexCoord);
    // The client's alpha reference, 224/255, as character.frag cuts the
    // batch on screen; at 0.5 the cast silhouette was fatter than the drawn
    // one, hair cards most of all.
    if (alphaTest != 0 && texColor.a < 0.8784314) discard;
}
