#version 450 core
// imgui_add.frag, with GxBlend_AlphaKey's reference (0x0081fe90): 224/255.
layout(location = 0) out vec4 fColor;
layout(set=0, binding=0) uniform sampler2D sTexture;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;
void main()
{
    vec4 c = In.Color * texture(sTexture, In.UV.st);
    if (c.a < 224.0 / 255.0) discard;
    fColor = c;
}
