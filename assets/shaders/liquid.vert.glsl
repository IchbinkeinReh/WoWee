#version 450

// The client's own liquid: vsLiquidWater and vsLiquidMagma (the .bls
// programs CMaterialWater 0x008a3f70 and CMaterialMagma 0x008a4190 load; the
// variant without point lights). The vertex is what 0x007ce390 and
// 0x007a7b00 build: position, normal 0,0,1, colour, the surface coordinate
// (attribute 6) and the depth coordinate (attribute 7). The constants are the
// ones 0x008a38b0 fills: c33 the light's direction, c34 its ambient, c35 its
// diffuse, c36 its specular with the exponent 6.0 in w (0xd44ef4), c4 the fog.

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrix;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 viewPos;
    vec4 fogColor;
    vec4 fogParams;
};

layout(push_constant) uniform Push {
    vec4 animMatrix;   // the surface coordinate's 2x2 (c9, c10), column by column
    vec4 params;       // x depth v scale (c14), y light (0 world, 1 white, 2 none), z kind (0 water, 1 magma), w depth source
    vec4 scroll;       // xy the surface coordinate's translation (c12): magma's scroll
    vec4 specular;     // rgb c36's colour, a its exponent; 0 for NoSpec
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aSurfaceUV;
layout(location = 2) in vec2 aDepthUV;
layout(location = 3) in vec4 aColor;

layout(location = 0) out vec2 vSurfaceUV;
layout(location = 1) out vec2 vDepthUV;
layout(location = 2) out vec4 vPrimary;
layout(location = 3) out vec3 vSecondary;
layout(location = 4) out float vFog;

void main() {
    vec4 viewSpace = view * vec4(aPos, 1.0);
    gl_Position = projection * viewSpace;

    // texcoord[1] = attrib6 x (c9, c10) + c12; texcoord[0] = attrib7 x (c13, c14).
    vSurfaceUV = mat2(push.animMatrix.xy, push.animMatrix.zw) * aSurfaceUV + push.scroll.xy;
    vDepthUV = vec2(aDepthUV.x, aDepthUV.y * push.params.x);

    const vec3 N = vec3(0.0, 0.0, 1.0);
    vec3 L = normalize(lightDir.xyz);  // the way the light travels, as c33
    if (push.params.y < 0.5) {
        // color = (c34 + sat(N . -c33) x c35) x the vertex colour.
        float nl = clamp(dot(N, -L), 0.0, 1.0);
        vPrimary = clamp(vec4(ambientColor.rgb + nl * lightColor.rgb, 1.0) * aColor, 0.0, 1.0);
        // color.secondary = max(N . -normalize(normalize(P - eye) + c33), 0)^c36.w x c36.
        vec3 h = normalize(normalize(aPos - viewPos.xyz) + L);
        float s = max(dot(N, -h), 0.0);
        vSecondary = push.specular.a > 0.0 ? pow(s, push.specular.a) * push.specular.rgb : vec3(0.0);
    } else {
        // The interior light (0x007d4f40): white, straight down, no ambient
        // and no specular - so the vertex colour itself. Magma's program
        // passes the vertex colour through unlit.
        vPrimary = aColor;
        vSecondary = vec3(0.0);
    }

    // The fog factor, by view depth: ((end - z) / (end - start))^exponent,
    // at most 1 (c4, which 0x008a38b0 builds from the light's fog).
    float depth = -viewSpace.z;
    float f = max((fogParams.y - depth) / max(fogParams.y - fogParams.x, 1e-4), 0.0);
    vFog = min(pow(f, max(fogColor.w, 1.0)), 1.0);
}
