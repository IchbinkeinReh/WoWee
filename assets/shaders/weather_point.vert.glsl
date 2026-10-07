#version 450

// Snow and sand, one point a particle, as the client's `snowpoint` and `sand`
// vertex programs draw them (Shaders\Vertex, arbvp1; bound by 0x0078aee0 and
// 0x0078bee0).
//
// Snow: the flake falls from its start to its end and stays there; 14 pixels
// across, less by a fiftieth a yard away, never under one; fading in over its
// first second and out over the quarter second after it lands. Its colour is
// the Weather.dbc row's.
//
// Sand: the grain blows from its start to its end; fading in over its first
// fifth of a second and out over its last; its size the viewport's width over
// 400, full to 10 yards away and gone by 30. Its colour is the zone's fog.

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
    vec4 color;   // snow: the row's colour (c8); sand reads the fog colour (c0)
    vec4 params;  // x = now; y = 0 snow, 1 sand; z = sand's size (c9.z)
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aVel;
layout(location = 2) in vec2 aTimes;  // start, end

layout(location = 0) out vec4 vColor;

void main() {
    float now = push.params.x;
    if (push.params.y < 0.5) {
        float t = min(aTimes.y, now) - aTimes.x;
        vec3 p = aPos + aVel * t;
        float dist = length(p - viewPos.xyz);
        // c7 = (-0.02, 1, 14); the point size held to 24 (render state 0x52).
        gl_PointSize = clamp(clamp(dist * -0.02 + 1.0, 0.0, 1.0) * 14.0, 1.0, 24.0);
        float fade = clamp(max(now - aTimes.y, 0.0) * -4.0 + 1.0, 0.0, 1.0);
        float a = fade < 1.0 ? fade : t;
        vColor = vec4(push.color.rgb, clamp(a, 0.0, 1.0));
        gl_Position = projection * view * vec4(p, 1.0);
    } else {
        float life = aTimes.y - aTimes.x;
        float t = max(min(life, now - aTimes.x), 0.0);
        vec3 p = aPos + aVel * t;
        float dist = length(p - viewPos.xyz);
        // c9 = (-0.05, 1.5, width * 0.0025).
        float size = clamp(dist * -0.05 + 1.5, 0.0, 1.0);
        float fadeIn = 1.0 - clamp(t * -5.0 + 1.0, 0.0, 1.0);
        float fadeOut = clamp(life * 5.0 + t * -5.0, 0.0, 1.0);
        vColor = vec4(1.0, 1.0, 1.0, fadeIn * fadeOut) * vec4(fogColor.rgb, 1.0);
        gl_PointSize = max(size * push.params.z, 1.0);
        gl_Position = projection * view * vec4(p, 1.0);
    }
}
