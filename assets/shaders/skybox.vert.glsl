#version 450

// Fullscreen triangle sky - no vertex buffer, no mesh.
// Draws 3 vertices covering the entire screen, depth forced to 1.0 (far plane).

layout(location = 0) out vec2 TexCoord;

void main() {
    // Produces triangle covering NDC [-1,1]² on the far plane: depth 0,
    // depth being reversed (depth_convention.hpp).
    TexCoord = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(TexCoord * 2.0 - 1.0, 0.0, 1.0);
}
