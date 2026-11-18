#pragma once
// The units' own tile of the sun's shadow atlas (Renderer::computeLightSpaceMatrix,
// shadow_csm.glsli's csmUnitShadow).
//
// The cascades cover the shadow distance - up to 500 yards - and with one
// cascade, or a coarse first one, a texel of the map is a quarter of a yard:
// a character, two yards tall and half a yard across, is a handful of texels,
// and its shadow on the ground came out in blocks (Stonebull Lake at 14:00,
// one cascade of 4096 over 500 yards, user180). With the ray tracing on it is
// still the map's: the units' sun shadows are left to the map wherever it
// reaches (rt_trace.comp), as a traced body comes a frame late and is rigid
// pieces, while the plants beside them took the traced shadows' smooth edges.
//
// So the units near the player are drawn once more, alone, into a small tile
// of their own a few hundredths of a yard a texel, and the cascades leave out
// every unit that tile holds whole. A surface takes the cascades' shadow
// times the tile's. A unit only partly in the tile is in both, and casts its
// coarse shadow as before.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

namespace wowee::rendering::shadow_unit_tile {

/// Yards from the tile's centre (the player) to a side: the units a camera at
/// its farthest still shows large enough for a shadow's edge to matter.
constexpr float kHalfExtent = 24.0f;
/// The tile's side in texels at most: 48 yards over 2048 is 0.023 yards a texel,
/// a tenth of what one cascade over 500 yards gives a 4096 map.
constexpr uint32_t kMaxSide = 2048;
/// A finest cascade with texels this fine or finer already draws a unit's shadow
/// smoothly; then there is no tile (the default two cascades at 4096 are 0.025).
constexpr float kWantedAboveTexel = 0.06f;

/// The tile's side for a cascade tile of `cascadeTile` texels: as large as that,
/// up to kMaxSide.
inline uint32_t side(uint32_t cascadeTile) {
    return std::min(cascadeTile, kMaxSide);
}

/// Yards a texel of the unit tile.
inline float texelWorld(uint32_t unitSide) {
    return unitSide > 0 ? 2.0f * kHalfExtent / static_cast<float>(unitSide) : 0.0f;
}

/// Whether the units want a tile of their own: the finest cascade, of half-extent
/// `finestHalfExtent` yards over `cascadeTile` texels, is coarser than
/// kWantedAboveTexel and coarser than the tile would be.
inline bool wanted(float finestHalfExtent, uint32_t cascadeTile) {
    if (cascadeTile == 0 || finestHalfExtent <= 0.0f) return false;
    const float finest = 2.0f * finestHalfExtent / static_cast<float>(cascadeTile);
    return finest > kWantedAboveTexel && finest > texelWorld(side(cascadeTile));
}

/// Where the unit tile goes in the atlas, and the atlas it makes.
struct Placement {
    bool placed = false;   ///< false: no room for it, the atlas is the cascades' alone
    uint32_t x = 0, y = 0; ///< its top-left texel
    uint32_t atlasW = 0, atlasH = 0;
};

/// Places a tile of `unitSide` beside the cascades' block of `blockW` x `blockH`
/// (`tile` a side each): in the free quarter three cascades in two rows leave,
/// else to the right of the block, else under it, as the device's widest and
/// tallest drawable image (`maxW`, `maxH`) allow.
inline Placement place(uint32_t blockW, uint32_t blockH, uint32_t tile, int cascades, bool inRow,
                       uint32_t unitSide, uint32_t maxW, uint32_t maxH) {
    Placement p;
    p.atlasW = blockW;
    p.atlasH = blockH;
    if (unitSide == 0) return p;
    if (cascades == 3 && !inRow && unitSide <= tile) {
        p.placed = true;
        p.x = tile;
        p.y = tile;
        return p;
    }
    if (static_cast<uint64_t>(blockW) + unitSide <= maxW && std::max(blockH, unitSide) <= maxH) {
        p.placed = true;
        p.x = blockW;
        p.y = 0;
        p.atlasW = blockW + unitSide;
        p.atlasH = std::max(blockH, unitSide);
        return p;
    }
    if (static_cast<uint64_t>(blockH) + unitSide <= maxH && std::max(blockW, unitSide) <= maxW) {
        p.placed = true;
        p.x = 0;
        p.y = blockH;
        p.atlasW = std::max(blockW, unitSide);
        p.atlasH = blockH + unitSide;
        return p;
    }
    return p;
}

/// How a unit stands to the tile. `lightSpace` is the tile's matrix (world to
/// clip, x and y -1..1 across it, orthographic), `at` the unit's origin and
/// `reach` the most any of it lies from there, in yards.
enum class Cover { Outside, Partly, Whole };
inline Cover cover(const glm::mat4& lightSpace, const glm::vec3& at, float reach, float halfExtent) {
    if (halfExtent <= 0.0f) return Cover::Outside;
    const glm::vec4 c = lightSpace * glm::vec4(at, 1.0f);
    const float r = reach / halfExtent;
    const float m = std::max(std::abs(c.x), std::abs(c.y));
    if (m + r < 1.0f) return Cover::Whole;
    if (m - r < 1.0f) return Cover::Partly;
    return Cover::Outside;
}

/// The most any part of a model lies from its origin, in yards, from its
/// bounding box and the instance's scale.
inline float reach(const glm::vec3& boundMin, const glm::vec3& boundMax, float scale) {
    const glm::vec3 corner = glm::max(glm::abs(boundMin), glm::abs(boundMax));
    return glm::length(corner) * std::max(scale, 0.0f);
}

}  // namespace wowee::rendering::shadow_unit_tile
