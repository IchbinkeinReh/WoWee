#pragma once

/// Where a ray first meets a skinned model's triangles, on the CPU: what the
/// 3.3.5a client asks of every model under the pointer before anything else
/// (0x004f9550 hands the tracked models to the M2 scene, whose first pass,
/// 0x0081df10 with 0x0081daf0, walks the triangles of the batches that draw
/// and keeps the nearest hit). Of two units standing in one another the one
/// whose body is in front at the pointer is the one pointed at; a bounding
/// sphere ranked by its centre always gave the same one wherever the pointer
/// was on them.
///
/// Pure: tests/test_ray_mesh.cpp checks it.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::rendering::ray_mesh {

/// Möller-Trumbore, both faces: the distance along `dir` - in its own units,
/// so an unnormalised direction keeps the caller's parameter - at which the
/// ray meets triangle (a, b, c), if it does in front of the origin.
inline std::optional<float> rayTriangle(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& a,
                                        const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 e1 = b - a;
    const glm::vec3 e2 = c - a;
    const glm::vec3 p = glm::cross(dir, e2);
    const float det = glm::dot(e1, p);
    if (std::abs(det) < 1e-12f) return std::nullopt;
    const float inv = 1.0f / det;
    const glm::vec3 s = origin - a;
    const float u = glm::dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return std::nullopt;
    const glm::vec3 q = glm::cross(s, e1);
    const float v = glm::dot(dir, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return std::nullopt;
    const float t = glm::dot(e2, q) * inv;
    if (t < 0.0f) return std::nullopt;
    return t;
}

/// A vertex as the vertex shader skins it (character.vert): the bones'
/// matrices weighted by bytes out of 255, an index past the bones taken as
/// the last one. Any type with `position`, `boneIndices[4]` and
/// `boneWeights[4]` (pipeline::M2Vertex).
struct SkinnedVertex {
    glm::vec3 position{0.0f};
    uint8_t boneWeights[4] = {0, 0, 0, 0};
    uint8_t boneIndices[4] = {0, 0, 0, 0};
};

template <class Vertex>
glm::vec3 skin(const Vertex& v, const std::vector<glm::mat4>& bones) {
    if (bones.empty()) return v.position;
    glm::mat4 m(0.0f);
    float total = 0.0f;
    for (int k = 0; k < 4; ++k) {
        if (v.boneWeights[k] == 0) continue;
        const float w = static_cast<float>(v.boneWeights[k]) / 255.0f;
        m += bones[std::min<size_t>(v.boneIndices[k], bones.size() - 1)] * w;
        total += w;
    }
    if (total <= 0.0f) return v.position;
    return glm::vec3(m * glm::vec4(v.position, 1.0f));
}

/// The nearest hit of a world ray on the triangles of `indices` within
/// `ranges` (first index, count), the model posed by `bones` and placed by
/// `model`. The ray is taken into the model's space rather than every vertex
/// into the world's, so the distance is the world ray's own. None past
/// `maxT`, or where it meets nothing.
template <class Vertex>
std::optional<float> rayHitSkinned(const std::vector<Vertex>& vertices,
                                          const std::vector<uint16_t>& indices,
                                          const std::vector<std::pair<uint32_t, uint32_t>>& ranges,
                                          const std::vector<glm::mat4>& bones, const glm::mat4& model,
                                          const glm::vec3& origin, const glm::vec3& dir, float maxT) {
    const glm::mat4 toModel = glm::inverse(model);
    const glm::vec3 o = glm::vec3(toModel * glm::vec4(origin, 1.0f));
    const glm::vec3 d = glm::vec3(toModel * glm::vec4(dir, 0.0f));
    // Each vertex skinned once, as a range first needs it.
    std::vector<glm::vec3> posed(vertices.size());
    std::vector<uint8_t> done(vertices.size(), 0);
    auto at = [&](uint16_t i) -> const glm::vec3& {
        if (!done[i]) {
            posed[i] = skin(vertices[i], bones);
            done[i] = 1;
        }
        return posed[i];
    };
    std::optional<float> best;
    float limit = maxT;
    for (const auto& [first, count] : ranges) {
        const size_t end = std::min<size_t>(size_t(first) + count, indices.size());
        for (size_t i = first; i + 2 < end; i += 3) {
            const uint16_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
            if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()) continue;
            const auto t = rayTriangle(o, d, at(a), at(b), at(c));
            if (t && *t <= limit) {
                limit = *t;
                best = *t;
            }
        }
    }
    return best;
}

/// Of the units a ray's spheres met, the one it meets first on their
/// triangles: `hits` as (precise distance or none, index). The index of the
/// nearest precise hit, or none when the ray met no triangle of any - which
/// the caller answers by the spheres, as before.
inline std::optional<size_t> nearestPrecise(const std::vector<std::optional<float>>& hits) {
    std::optional<size_t> best;
    for (size_t i = 0; i < hits.size(); ++i) {
        if (!hits[i]) continue;
        if (!best || *hits[i] < *hits[*best]) best = i;
    }
    return best;
}

}  // namespace wowee::rendering::ray_mesh
