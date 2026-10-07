#version 450

// A rain drop as the client's `rain` vertex program draws it
// (Shaders\Vertex\rain, arbvp1; bound by 0x0078a640). One instance a drop and
// three vertices: two 0.05 yards either side of the drop across the view, and
// a tip two yards behind it. The drop is placed from its start and end times
// alone; the tip falls on to two yards past the end, so the streak runs into
// the ground. Alive only from its start until the tip lands.

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
    vec4 color;   // the Weather.dbc row's colour (c7)
    vec4 params;  // x = now, from the packet's base (c6.x)
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aVel;
layout(location = 2) in vec2 aTimes;  // start, end

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

// c12..c14, c15..c17, c18..c20.
const vec2 kUV[3] = vec2[](vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(0.5, 0.0));
const float kSide[3] = float[](-0.05, 0.05, 0.0);
const float kBack[3] = float[](0.0, 0.0, 2.0);

void main() {
    int corner = gl_VertexIndex;
    float now = push.params.x;
    float tipEnd = aTimes.y + 2.0 / -aVel.z;
    float t = (corner == 2 ? min(tipEnd, now) : min(aTimes.y, now)) - aTimes.x;
    vec3 p = aPos + aVel * t;
    vec3 toEye = normalize(viewPos.xyz - p);
    vec3 back = normalize(-aVel);
    vec3 side = normalize(cross(toEye, back));
    p += side * kSide[corner] + back * kBack[corner];
    gl_Position = projection * view * vec4(p, 1.0);
    vUV = kUV[corner];
    float alive = (aTimes.x < now && now < tipEnd) ? 1.0 : 0.0;
    vColor = vec4(push.color.rgb, alive);
}
