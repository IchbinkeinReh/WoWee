#pragma once

/// A skinned model as the ray tracer can afford to move it: cut into rigid
/// pieces, one per bone, each placed every frame by that bone's matrix.
///
/// Skinning the vertices and rebuilding a bottom level per character per
/// frame costs, on the CPU, a few hundred microseconds a character, and on
/// the GPU a skinning pass plus a BLAS rebuild - and the software tracer would
/// need its hierarchy refitted on the CPU besides. Cut into pieces instead,
/// the geometry never changes: a piece is built once per model, and a pose is
/// only a transform per piece, which the dynamic top level takes as it takes
/// any instance. The joints part a little where a triangle spans two bones;
/// in a shadow on the ground that does not show.
///
/// Each triangle goes with the bone that carries most of its three vertices'
/// weight. Bones that would carry only a sliver - a finger, an eyelid - hand
/// their triangles to the nearest ancestor that carries enough, so a hand is
/// one piece and a character some fifteen to thirty.
///
/// Pure: tests/test_rt_rigid_parts.cpp checks it.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace wowee::rendering::rt_parts {

struct Part {
    uint32_t bone = 0;                  // the bone whose matrix places it
    std::vector<uint32_t> triangles;    // indices into the input triangles
};

/// `boneIndices` and `boneWeights`: four per vertex. `triangles`: three
/// vertex indices per triangle. `parents`: one per bone, -1 for a root.
/// Parts come out ordered by bone.
inline std::vector<Part> splitByBone(std::span<const std::array<uint8_t, 4>> boneIndices,
                                     std::span<const std::array<uint8_t, 4>> boneWeights,
                                     std::span<const uint32_t> triangles,
                                     std::span<const int16_t> parents, uint32_t minTriangles) {
    const size_t boneCount = parents.size();
    const size_t triCount = triangles.size() / 3;
    std::vector<uint32_t> triBone(triCount, 0);
    std::vector<uint32_t> perBone(std::max<size_t>(boneCount, 1), 0);
    for (size_t t = 0; t < triCount; ++t) {
        // The weight each bone has across the triangle's corners; four
        // bones a vertex, so twelve at most.
        std::array<uint32_t, 12> bones{};
        std::array<uint32_t, 12> weight{};
        size_t n = 0;
        for (int c = 0; c < 3; ++c) {
            const uint32_t v = triangles[t * 3 + c];
            if (v >= boneIndices.size() || v >= boneWeights.size()) continue;
            for (int k = 0; k < 4; ++k) {
                const uint32_t b = boneIndices[v][k];
                const uint32_t w = boneWeights[v][k];
                if (w == 0 || b >= boneCount) continue;
                size_t j = 0;
                while (j < n && bones[j] != b) ++j;
                if (j == n) {
                    bones[n] = b;
                    weight[n] = 0;
                    ++n;
                }
                weight[j] += w;
            }
        }
        uint32_t best = 0, bestWeight = 0;
        for (size_t j = 0; j < n; ++j) {
            // Ties to the lower bone, which is nearer the root.
            if (weight[j] > bestWeight || (weight[j] == bestWeight && bones[j] < best)) {
                best = bones[j];
                bestWeight = weight[j];
            }
        }
        triBone[t] = best;
        if (best < perBone.size()) ++perBone[best];
    }

    // Small pieces up the hierarchy, judged by what each bone had to begin
    // with so the result does not depend on the order of the walk.
    std::vector<uint32_t> target(perBone.size());
    for (size_t b = 0; b < perBone.size(); ++b) {
        uint32_t cur = static_cast<uint32_t>(b);
        for (size_t guard = 0; guard < boneCount && perBone[cur] < minTriangles; ++guard) {
            const int16_t p = cur < parents.size() ? parents[cur] : int16_t(-1);
            if (p < 0 || static_cast<size_t>(p) >= boneCount) break;
            cur = static_cast<uint32_t>(p);
        }
        target[b] = cur;
    }

    std::vector<int32_t> partOf(perBone.size(), -1);
    std::vector<Part> parts;
    for (size_t b = 0; b < perBone.size(); ++b) {
        if (perBone[b] == 0) continue;
        const uint32_t to = target[b];
        if (partOf[to] < 0) {
            partOf[to] = 0;  // placeholder: numbered below, in bone order
        }
    }
    for (size_t b = 0; b < partOf.size(); ++b) {
        if (partOf[b] < 0) continue;
        partOf[b] = static_cast<int32_t>(parts.size());
        Part p;
        p.bone = static_cast<uint32_t>(b);
        parts.push_back(std::move(p));
    }
    for (size_t t = 0; t < triCount; ++t) {
        const uint32_t to = target[triBone[t]];
        parts[static_cast<size_t>(partOf[to])].triangles.push_back(static_cast<uint32_t>(t));
    }
    return parts;
}

}  // namespace wowee::rendering::rt_parts
