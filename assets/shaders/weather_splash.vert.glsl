#version 450

// A rain drop's splash as the client's `patter` vertex program draws it
// (Shaders\Vertex\patter, arbvp1; bound by 0x0078a030). A small triangle a
// sixth of a yard high facing the camera, sunk toward the ground the more the
// camera looks down on it, its picture one of RainDropSplash01's 4 x 4 cells:
// the column steps through four frames over its quarter second, the row is
// chosen by how steeply it is seen.

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
    vec4 shadowParams;
};

layout(push_constant) uniform Push {
    vec4 color;   // c7
    vec4 params;  // x = now (c6.x)
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTimes;  // landing, landing + 0.25

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

// c11..c13.
const vec2 kUV[3] = vec2[](vec2(0.0, 0.25), vec2(0.125, 0.04296875), vec2(0.25, 0.25));

void main() {
    int corner = gl_VertexIndex;
    float now = push.params.x;
    vec3 right = vec3(view[0][0], view[1][0], view[2][0]);
    vec3 up = vec3(view[0][1], view[1][1], view[2][1]);
    // c8..c10: a twelfth of a yard to either side, a sixth up.
    vec3 corners[3] = vec3[](-right / 12.0, up / 6.0, right / 12.0);
    vec3 p = aPos + corners[corner] - viewPos.xyz;
    float down = max(-normalize(p).z, 0.0);
    p -= (up / 6.0) * down * 0.5;
    p.z += down * 0.083333336;
    float column = floor((now - aTimes.x) * 15.96);
    float row = floor((1.0 - down) * 3.99);
    gl_Position = projection * view * vec4(p + viewPos.xyz, 1.0);
    vUV = kUV[corner] + vec2(column, row) * 0.25;
    float alive = (aTimes.x < now && now < aTimes.y) ? 1.0 : 0.0;
    vColor = vec4(push.color.rgb, alive);
}
