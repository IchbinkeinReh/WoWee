#pragma once

/// The layout of the irradiance probe volume (RtGiProbes), apart from anything
/// Vulkan so a test can hold it against the shaders. rt_gi.glsli mirrors every
/// constant and function here; a change to one is a change to the other.
///
/// The volume is a grid of probes per cascade that follows the camera. A probe
/// keeps its world position while the grid moves: it is stored at its absolute
/// grid coordinate modulo the probe counts, so scrolling by a plane only
/// invalidates the plane that wrapped round to the far side, and everything
/// else keeps the light it has gathered.
///
/// Each probe owns two tiles of one RGBA16F atlas: irradiance in an octahedral
/// 6x6 (8x8 with its border) and the mean and mean square distance to what its
/// rays hit in an 8x8 (10x10), for the visibility test that keeps light from
/// leaking through walls. Per cascade the atlas has a band of irradiance tiles
/// over a band of distance tiles, a tile column per (x, z) and a row per y.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace wowee::rendering::gi {

inline constexpr glm::ivec3 kProbes{32, 32, 16};  // x, y horizontal; z up
inline constexpr int kProbesPerCascade = kProbes.x * kProbes.y * kProbes.z;
inline constexpr int kCascades = 2;
inline constexpr int kTotalProbes = kProbesPerCascade * kCascades;
inline constexpr int kRaysPerProbe = 64;  // the trace's workgroup size

inline constexpr int kIrradianceTexels = 6;
inline constexpr int kIrradianceTile = kIrradianceTexels + 2;
inline constexpr int kVisibilityTexels = 8;
inline constexpr int kVisibilityTile = kVisibilityTexels + 2;
inline constexpr int kBandHeight = kProbes.y * (kIrradianceTile + kVisibilityTile);
inline constexpr int kAtlasWidth = kProbes.x * kProbes.z * kVisibilityTile;
inline constexpr int kAtlasHeight = kCascades * kBandHeight;

/// A list entry: the storage slot, the cascade, and whether the probe has just
/// come into the volume and must forget what its slot held.
inline constexpr uint32_t kListCascadeShift = 20;
inline constexpr uint32_t kListSlotMask = (1u << kListCascadeShift) - 1u;
inline constexpr uint32_t kListReset = 1u << 31;

inline constexpr uint32_t packListEntry(uint32_t slot, int cascade, bool reset) {
    return slot | (static_cast<uint32_t>(cascade) << kListCascadeShift) | (reset ? kListReset : 0u);
}

/// a mod n, never negative.
inline int floorMod(int a, int n) {
    const int m = a % n;
    return m < 0 ? m + n : m;
}

inline glm::ivec3 floorMod(const glm::ivec3& a, const glm::ivec3& n) {
    return {floorMod(a.x, n.x), floorMod(a.y, n.y), floorMod(a.z, n.z)};
}

/// Where in the volume a probe with absolute grid coordinate `abs` is stored.
inline glm::ivec3 storageOf(const glm::ivec3& abs) { return floorMod(abs, kProbes); }

inline uint32_t slotIndex(const glm::ivec3& storage) {
    return static_cast<uint32_t>(storage.x + kProbes.x * (storage.y + kProbes.y * storage.z));
}

inline glm::ivec3 storageOfSlot(uint32_t slot) {
    const int s = static_cast<int>(slot);
    return {s % kProbes.x, (s / kProbes.x) % kProbes.y, s / (kProbes.x * kProbes.y)};
}

/// The absolute grid coordinate a storage slot stands for while the grid's
/// first probe is at `gridMin`.
inline glm::ivec3 absoluteOf(const glm::ivec3& storage, const glm::ivec3& gridMin) {
    return gridMin + floorMod(storage - gridMin, kProbes);
}

inline bool inside(const glm::ivec3& abs, const glm::ivec3& gridMin) {
    return glm::all(glm::greaterThanEqual(abs, gridMin)) &&
           glm::all(glm::lessThan(abs, gridMin + kProbes));
}

/// The grid's first probe for a camera at `camera`. The grid is centred on the
/// camera, but moves only once the camera is more than a cell from the centre:
/// otherwise a camera that sways across a cell boundary resets the outermost
/// plane every other frame, and that plane flickers between a fresh estimate
/// and nothing.
inline glm::ivec3 scrolledGridMin(const glm::vec3& camera, float spacing, const glm::ivec3& current,
                                  bool haveCurrent) {
    const glm::ivec3 cell(glm::floor(camera / spacing));
    const glm::ivec3 wanted = cell - kProbes / 2;
    if (!haveCurrent) return wanted;
    glm::ivec3 out = current;
    for (int a = 0; a < 3; ++a) {
        if (std::abs(wanted[a] - current[a]) > 1) out[a] = wanted[a];
    }
    return out;
}

/// Append this frame's probes of one cascade to `list`: every probe that has
/// just come into the grid, and of the rest the slots whose turn it is - one in
/// `period`, so each is traced every `period` frames.
inline void appendUpdateList(std::vector<uint32_t>& list, int cascade, const glm::ivec3& gridMin,
                             const glm::ivec3& prevGridMin, bool havePrev, uint32_t period,
                             uint64_t frame) {
    period = std::max(period, 1u);
    const uint32_t phase = static_cast<uint32_t>(frame % period);
    if (havePrev && gridMin == prevGridMin) {
        // Most frames: nothing came in.
        for (uint32_t slot = phase; slot < static_cast<uint32_t>(kProbesPerCascade); slot += period) {
            list.push_back(packListEntry(slot, cascade, false));
        }
        return;
    }
    for (uint32_t slot = 0; slot < static_cast<uint32_t>(kProbesPerCascade); ++slot) {
        const glm::ivec3 abs = absoluteOf(storageOfSlot(slot), gridMin);
        const bool fresh = !havePrev || !inside(abs, prevGridMin);
        if (fresh || slot % period == phase) list.push_back(packListEntry(slot, cascade, fresh));
    }
}

/// The corner of a probe's tiles in the atlas, in texels (the border's corner).
inline glm::ivec2 irradianceTileOrigin(int cascade, const glm::ivec3& storage) {
    return {(storage.x + kProbes.x * storage.z) * kIrradianceTile,
            cascade * kBandHeight + storage.y * kIrradianceTile};
}

inline glm::ivec2 visibilityTileOrigin(int cascade, const glm::ivec3& storage) {
    return {(storage.x + kProbes.x * storage.z) * kVisibilityTile,
            cascade * kBandHeight + kProbes.y * kIrradianceTile + storage.y * kVisibilityTile};
}

/// Octahedral mapping, the z hemisphere folded (rt_common.glsli's rtOctEncode).
inline glm::vec2 octEncode(glm::vec3 n) {
    n /= std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
    glm::vec2 e(n.x, n.y);
    if (n.z < 0.0f) {
        e = (1.0f - glm::abs(glm::vec2(n.y, n.x))) *
            glm::vec2(e.x >= 0.0f ? 1.0f : -1.0f, e.y >= 0.0f ? 1.0f : -1.0f);
    }
    return e;
}

inline glm::vec3 octDecode(const glm::vec2& e) {
    glm::vec3 n(e.x, e.y, 1.0f - std::abs(e.x) - std::abs(e.y));
    if (n.z < 0.0f) {
        const glm::vec2 xy = (1.0f - glm::abs(glm::vec2(n.y, n.x))) *
                             glm::vec2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
        n.x = xy.x;
        n.y = xy.y;
    }
    return glm::normalize(n);
}

/// The direction an interior texel of a tile `texels` wide stands for.
inline glm::vec3 texelDirection(const glm::ivec2& texel, int texels) {
    return octDecode((glm::vec2(texel) + 0.5f) / static_cast<float>(texels) * 2.0f - 1.0f);
}

/// The interior texel a tile texel copies, `t` counted from the first interior
/// texel, so the border is at -1 and `texels`. Across an edge the octahedron
/// continues mirrored, so the border texel copies its neighbour inside with the
/// other coordinate reversed; a corner copies the opposite corner. Bilinear
/// filtering at an edge then blends directions that are neighbours on the
/// sphere, not across the map.
inline glm::ivec2 borderSource(const glm::ivec2& t, int texels) {
    const bool loX = t.x < 0, hiX = t.x >= texels, loY = t.y < 0, hiY = t.y >= texels;
    if ((loX || hiX) && (loY || hiY)) return {loX ? texels - 1 : 0, loY ? texels - 1 : 0};
    if (loX || hiX) return {loX ? 0 : texels - 1, texels - 1 - t.y};
    if (loY || hiY) return {texels - 1 - t.x, loY ? 0 : texels - 1};
    return t;
}

/// Ray `i` of `n` over the sphere, before the frame's random rotation.
inline glm::vec3 fibonacciDirection(int i, int n) {
    const float z = 1.0f - (2.0f * static_cast<float>(i) + 1.0f) / static_cast<float>(n);
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    const float phi = static_cast<float>(i) * 2.39996323f;  // the golden angle
    return {r * std::cos(phi), r * std::sin(phi), z};
}

}  // namespace wowee::rendering::gi
