#pragma once

/// What the 3.3.5a client gives a detail doodad (ground clutter) to be lit by
/// when it places one on a chunk (0x007d3390): the terrain's own vertex
/// colour there, the baked shadow there, and the face normal of the terrain
/// triangle it stands on. DetailDoodad.bls then lights every vertex of the
/// doodad by that one normal, multiplies by the colour and takes the shadow
/// from the colour's alpha (0x007b1b50 copies all three into each vertex).

#include "pipeline/adt_loader.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wowee::pipeline {

struct DetailDoodadShade {
    /// MCCV at the point, doubled and capped at 1 - 0x7f reads as 1. White
    /// when the chunk has none or the doodad's GroundEffectDoodad flag 0x2
    /// asks for none.
    glm::vec3 color{1.0f};
    /// The vertex alpha: 0 where MCSH marks the point shadowed, else 1.
    float lit = 1.0f;
    /// The face normal of the terrain triangle under it, render space.
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
};

/// The client's colour, shadow and normal for a doodad at (fracX, fracY) - in
/// quads, 0 to 8, across the chunk's columns and rows - as 0x007d3390 works
/// them out. `colored` is GroundEffectDoodad flag 0x2 clear.
inline DetailDoodadShade detailDoodadShade(const MapChunk& chunk, float fracX, float fracY,
                                           float unitSize, bool colored) {
    DetailDoodadShade s;
    const int qx = std::clamp(static_cast<int>(std::floor(fracX)), 0, 7);
    const int qy = std::clamp(static_cast<int>(std::floor(fracY)), 0, 7);
    const float u = std::clamp(fracX - static_cast<float>(qx), 0.0f, 1.0f);
    const float v = std::clamp(fracY - static_cast<float>(qy), 0.0f, 1.0f);

    // The wedge of the quad's fan the point is in: the centre and two corners,
    // as TerrainMeshGenerator::chunkSurfacePoint picks them.
    int ax, ay, bx, by;
    float wA, wB;
    if (u > v) {
        if (u + v < 1.0f) { ax = qx; ay = qy; bx = qx + 1; by = qy; wA = 1.0f - u - v; wB = u - v; }
        else { ax = qx + 1; ay = qy; bx = qx + 1; by = qy + 1; wA = u - v; wB = u + v - 1.0f; }
    } else {
        if (u + v < 1.0f) { ax = qx; ay = qy; bx = qx; by = qy + 1; wA = 1.0f - u - v; wB = v - u; }
        else { ax = qx; ay = qy + 1; bx = qx + 1; by = qy + 1; wA = v - u; wB = u + v - 1.0f; }
    }
    const float wC = 1.0f - wA - wB;
    const int iA = ay * 17 + ax;
    const int iB = by * 17 + bx;
    const int iC = 9 + qy * 17 + qx;

    if (colored && chunk.hasVertexShading) {
        // Each channel of the three vertices doubled and interpolated, capped
        // at 255 (0x007d3390 doubles the base and both edges).
        auto channel = [&](int c) {
            const float a = chunk.vertexShading[static_cast<size_t>(iA) * 4 + c];
            const float b = chunk.vertexShading[static_cast<size_t>(iB) * 4 + c];
            const float m = chunk.vertexShading[static_cast<size_t>(iC) * 4 + c];
            return std::min((a * wA + b * wB + m * wC) * 2.0f, 255.0f) / 255.0f;
        };
        // BGRA bytes.
        s.color = glm::vec3(channel(2), channel(1), channel(0));
    }

    if (chunk.shadowMap.size() >= 64 * 64) {
        // The MCSH texel under it: 1.92 texels a yard, round(t - 0.5) - the
        // texel it is in (0x007d3390).
        const int tx = std::clamp(static_cast<int>(std::floor(fracX * unitSize * 1.92f)), 0, 63);
        const int ty = std::clamp(static_cast<int>(std::floor(fracY * unitSize * 1.92f)), 0, 63);
        if (chunk.shadowMap[static_cast<size_t>(ty) * 64 + tx] != 0) s.lit = 0.0f;
    }

    // The triangle's face normal. World X runs against grid rows and world Y
    // against grid columns (chunkSurfacePoint).
    auto vertex = [&](float gx, float gy, float h) {
        return glm::vec3(-gy * unitSize, -gx * unitSize, h);
    };
    const glm::vec3 pA = vertex(static_cast<float>(ax), static_cast<float>(ay), chunk.heightMap.getHeight(ax, ay));
    const glm::vec3 pB = vertex(static_cast<float>(bx), static_cast<float>(by), chunk.heightMap.getHeight(bx, by));
    const float hC = iC < static_cast<int>(chunk.heightMap.heights.size()) ? chunk.heightMap.heights[iC] : 0.0f;
    const glm::vec3 pC = vertex(static_cast<float>(qx) + 0.5f, static_cast<float>(qy) + 0.5f, hC);
    glm::vec3 n = glm::cross(pB - pA, pC - pA);
    const float len = glm::length(n);
    if (len > 1e-6f) {
        n /= len;
        if (n.z < 0.0f) n = -n;
        s.normal = n;
    }
    return s;
}

}  // namespace wowee::pipeline
