#pragma once

/// What of the world is in the pointer's way, as the 3.3.5a client asks it:
/// its ray pick, 0x004f9930, shortens the ray to the first hit of the world
/// with the trace flags 0x1000124 (0x1020124 where the liquid's surface
/// counts), takes an object's model in what is left, and for a place on the
/// ground asks once more with 0x100151. The flags are CWorld::Intersect's
/// (0x0077f310 -> 0x007a3b70):
///
///   0x1        doodads by their collision triangles (0x007a2760 gives them
///              mode 3, which 0x0081df10 sends to 0x0081dd50)
///   0x2, 0x4   doodads by the triangles they draw (0x0081daf0) - with
///              0x1000000 (mode 1) only of batches that blend nothing
///   0x10       a building's collision faces, detail ones (MOPY 0x04) left out
///   0x20       a building's drawn faces: collision-only ones (0x08) left out
///   0x40       a building's faces but those flagged 0x02, the camera's
///   0x100      the terrain
///   0x30000    liquids
///   0x100000   game objects' models by their collision
///   0x1000000  no doodad the frame has culled (+0x25), and no face of a
///              building whose material blends
///
/// So the pointer stops at what is seen - terrain, a building's opaque
/// faces, a doodad's opaque triangles - and goes through what only stands in
/// the way. Pure: tests/test_ray_mesh.cpp checks it.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "rendering/ray_mesh.hpp"

namespace wowee::rendering::pointer_pick {

inline constexpr uint32_t kTraceSeen = 0x1000124;        ///< 0x004f9930's first
inline constexpr uint32_t kTraceSeenLiquid = 0x1020124;  ///< ...with pick flag 0x2
inline constexpr uint32_t kTraceGround = 0x100151;       ///< ...its second, for a place

/// 0x007ae140: the faces a trace leaves out, as MOPY flags; 0x100 and 0x200
/// besides for the faces whose material blends, or does not.
[[nodiscard]] constexpr uint32_t buildingFaceMask(uint32_t traceFlags) {
    if ((traceFlags & 0x80) != 0) return 0x2ca;
    uint32_t mask = (traceFlags & 0x10) != 0 ? 0xc6u : 0xeeu;
    if ((traceFlags & 0x20) != 0) mask &= 0xffffffdbu;
    if ((traceFlags & 0x40) == 0) mask &= 0xfffffffdu;
    if ((traceFlags & 0x4000) == 0) mask &= 0xffffffbfu;
    if ((traceFlags & 0x1000000) != 0) {
        if ((traceFlags & 0x40000000) != 0) return 0xfffffcdbu;
        mask |= 0x100;
    }
    return mask;
}

/// 0x007c6c30: whether a face is there for the trace. `blends`: its material
/// (none, 0xff, blends nothing) has a blend mode. The mask's 0x80 marks a
/// face already taken and is no flag of the file's.
[[nodiscard]] constexpr bool buildingFaceHit(uint32_t mask, uint8_t mopy, bool blends) {
    if ((mopy & mask & 0x7fu) != 0) return false;
    return (mask & (blends ? 0x100u : 0x200u)) == 0;
}

/// 0x0081daf0 with 0x007a2760's mode 1: whether a doodad's batch hides what
/// is behind it - its first layer, not flagged 0x8, shown at all, and either
/// blending nothing or of a material flagged 0x20.
[[nodiscard]] constexpr bool doodadBatchHides(uint8_t batchFlags, uint16_t materialLayer, uint16_t blendMode,
                                              uint16_t materialFlags, bool shown) {
    if (materialLayer != 0 || (batchFlags & 0x8) != 0 || !shown) return false;
    return blendMode == 0 || (materialFlags & 0x20) != 0;
}

/// The triangles of a doodad's model that hide what is behind it, and the
/// vertices they use - all that is kept of its mesh on this side.
struct DoodadMesh {
    std::vector<ray_mesh::SkinnedVertex> vertices;
    std::vector<uint16_t> indices;
    [[nodiscard]] bool empty() const { return indices.empty(); }
};

template <class Vertex>
DoodadMesh doodadMesh(const std::vector<Vertex>& vertices, const std::vector<uint16_t>& indices,
                      const std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
    DoodadMesh mesh;
    std::vector<int32_t> slot(vertices.size(), -1);
    for (const auto& [first, count] : ranges) {
        const size_t end = std::min<size_t>(size_t(first) + count, indices.size());
        for (size_t i = first; i + 2 < end; i += 3) {
            if (indices[i] >= vertices.size() || indices[i + 1] >= vertices.size() ||
                indices[i + 2] >= vertices.size()) {
                continue;
            }
            for (size_t k = 0; k < 3; ++k) {
                const uint16_t v = indices[i + k];
                if (slot[v] < 0) {
                    slot[v] = static_cast<int32_t>(mesh.vertices.size());
                    ray_mesh::SkinnedVertex kept;
                    kept.position = vertices[v].position;
                    for (int b = 0; b < 4; ++b) {
                        kept.boneWeights[b] = vertices[v].boneWeights[b];
                        kept.boneIndices[b] = vertices[v].boneIndices[b];
                    }
                    mesh.vertices.push_back(kept);
                }
                mesh.indices.push_back(static_cast<uint16_t>(slot[v]));
            }
        }
    }
    mesh.vertices.shrink_to_fit();
    mesh.indices.shrink_to_fit();
    return mesh;
}

/// Where the ray first meets it, posed by `bones` and placed by `model`
/// (0x0081df10's pass over the models the trace marked).
[[nodiscard]] inline std::optional<float> rayHitDoodad(const DoodadMesh& mesh, const std::vector<glm::mat4>& bones,
                                                       const glm::mat4& model, const glm::vec3& origin,
                                                       const glm::vec3& dir, float maxT) {
    return ray_mesh::rayHitSkinned(mesh.vertices, mesh.indices,
                                   {{0u, static_cast<uint32_t>(mesh.indices.size())}}, bones, model, origin, dir,
                                   maxT);
}

}  // namespace wowee::rendering::pointer_pick
