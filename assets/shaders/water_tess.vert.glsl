#version 450
// The tessellated water's vertex stage (WaterRenderer::buildWaterPipeline):
// the liquid grid's corner as it is, in the world. The waves and the
// interaction move the finer surface the tessellator makes of it (water.tese),
// which then does what water.vert does for the untessellated water.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec2 vTexCoord;

void main() {
    vWorldPos = aPos;
    vTexCoord = aTexCoord;
}
