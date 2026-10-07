#pragma once

#include "core/coordinates.hpp"
#include "pipeline/adt_loader.hpp"

#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

namespace wowee {
namespace pipeline {

/**
 * Vertex format for terrain rendering
 */
struct TerrainVertex {
    float position[3];     // X, Y, Z
    float normal[3];       // Normal vector
    float texCoord[2];     // Base texture coordinates
    float layerUV[2];      // Layer texture coordinates
    /// MCCV's colour for this vertex, 1 neutral: the textures are multiplied
    /// by it. White where the chunk has none.
    float shading[3];
    uint8_t chunkIndex = 0;    // Which chunk this vertex belongs to

    TerrainVertex()  {
        position[0] = position[1] = position[2] = 0.0f;
        normal[0] = normal[1] = normal[2] = 0.0f;
        texCoord[0] = texCoord[1] = 0.0f;
        layerUV[0] = layerUV[1] = 0.0f;
        shading[0] = shading[1] = shading[2] = 1.0f;
    }
};

/// How the 3.3.5a client maps a chunk's textures (0x007d0760 builds the
/// texture matrices, 0x007d06b0): the layers' textures once every 1/8 of a
/// chunk - a quad, 4.1667 yards, the scale being 1 / 0x00d254a8 - and the
/// alpha maps (and MCSH) exactly once over the chunk, 0 at its corner to 1 at
/// the far one, with no half texel inset.
inline constexpr float kTerrainTextureRepeatsPerChunk = 8.0f;
inline float terrainLayerUV(float vertexOffset) { return vertexOffset / 8.0f; }

/// MCLY flags the client draws by (0x007d0760, 0x007d0d70): 0x40 the layer's
/// texture scrolls, 0x80 the layer is drawn with the lighting off.
inline constexpr uint32_t kTerrainLayerAnimated = 0x40;
inline constexpr uint32_t kTerrainLayerUnlit = 0x80;

/// A scrolling layer's texture offset after `seconds` (MCLY flag 0x40). The
/// client keeps one offset a direction (flags & 7, 45 degrees apart: the
/// table at 0x00adee78), adds the direction times the frame time to it each
/// frame and sets a component that reaches 64 back to 0 (0x0077ee80's caller,
/// 0x00cd77f8). The texture is moved by that over the speed's divisor
/// ((flags >> 3) & 7 picks 64, 48, 32, 16, 8, 4, 2 or 1, 0x00af14f8) in
/// texture repeats: the offset's y moves u and its x moves v, since u runs
/// along the world's Y (0x007d06b0 swaps the rows), both against the
/// direction's sign. The terrain shader does the same with the frame time.
inline glm::vec2 terrainLayerAnimOffset(uint32_t flags, float seconds) {
    static constexpr float kDir[8][2] = {{-1, 0}, {-1, 1}, {0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}};
    static constexpr float kDivisor[8] = {64, 48, 32, 16, 8, 4, 2, 1};
    if ((flags & kTerrainLayerAnimated) == 0) return glm::vec2(0.0f);
    const float* dir = kDir[flags & 7u];
    glm::vec2 acc(dir[0] * seconds, dir[1] * seconds);
    for (int c = 0; c < 2; ++c) {
        if (dir[c] > 0.0f) acc[c] = acc[c] - 64.0f * static_cast<float>(static_cast<int>(acc[c] / 64.0f));
    }
    const float divisor = kDivisor[(flags >> 3) & 7u];
    return glm::vec2(-acc.y, -acc.x) / divisor;
}

/**
 * Triangle index (3 vertices)
 */
using TerrainIndex = uint32_t;

/**
 * Renderable terrain mesh for a single map chunk
 */
struct ChunkMesh {
    std::vector<TerrainVertex> vertices;
    std::vector<TerrainIndex> indices;

    // Chunk position in world space
    float worldX;
    float worldY;
    float worldZ;

    // Chunk grid coordinates
    int chunkX;
    int chunkY;

    // Texture layer info
    struct LayerInfo {
        uint32_t textureId;
        uint32_t flags;
        std::vector<uint8_t> alphaData;  // 64x64 alpha map
    };
    std::vector<LayerInfo> layers;

    /// MCSH, 64x64, one byte a texel: 0 shadowed, 255 lit. Empty for none.
    std::vector<uint8_t> shadowMap;

    /// The map's MPHD flag 0x4 (0x00cf08d0): Terrain1's weighted variants
    /// (0x0079e5c0 picks them) lay the layers over the base by their summed
    /// alpha instead of one over another.
    bool weightedLayers = false;

    [[nodiscard]] bool isValid() const { return !vertices.empty() && !indices.empty(); }
    [[nodiscard]] size_t getVertexCount() const { return vertices.size(); }
    [[nodiscard]] size_t getTriangleCount() const { return indices.size() / 3; }
};

/**
 * Complete terrain tile mesh (16x16 chunks)
 */
struct TerrainMesh {
    std::array<ChunkMesh, 256> chunks;  // 16x16 grid
    std::vector<std::string> textures;   // Texture filenames

    int validChunkCount = 0;

    [[nodiscard]] const ChunkMesh& getChunk(int x, int y) const { return chunks[y * 16 + x]; }
    ChunkMesh& getChunk(int x, int y) { return chunks[y * 16 + x]; }
};

/**
 * Terrain mesh generator
 *
 * Converts ADT heightmap data into renderable triangle meshes
 */
class TerrainMeshGenerator {
public:
    /**
     * Generate terrain mesh from ADT data
     * @param terrain Loaded ADT terrain data
     * @return Generated mesh (check validChunkCount)
     */
    static TerrainMesh generate(const ADTTerrain& terrain);

    /**
     * Where a point inside a chunk sits in the world.
     *
     * `fracX` and `fracY` are cell coordinates within the chunk, 0 to 8, and
     * need not be whole: the height between the four surrounding grid points
     * is interpolated bilinearly. This is what the ground-clutter and doodad
     * scatterers ask when they drop something at a random spot on a chunk,
     * and both had their own copy of it.
     *
     * The axes cross on purpose. A chunk's world X runs against the grid's
     * Y and its world Y against the grid's X, which is the terrain axis
     * pairing this codebase uses throughout; swapping them back lays every
     * scattered object out mirrored, which reads as the doodad data being
     * wrong rather than the sampling.
     *
     * Out-of-range coordinates clamp to the chunk rather than reading past
     * it, so a caller that rounds slightly past 8 gets the edge height
     * instead of a zero that would bury the object.
     */
    static glm::vec3 chunkSurfacePoint(const float chunkPosition[3],
                                       const HeightMap& heightMap,
                                       float fracX, float fracY, float unitSize);

    /**
     * Where a world position falls inside a chunk, in the 0..8 grid fractions
     * chunkSurfacePoint and isHole take. False if the position is outside it.
     *
     * The axes cross here too: world X gives fracY and world Y gives fracX.
     *
     * Separated so a caller walking many points can test the chunk it already
     * has before searching for another. The search is a tile lookup and a 3x3
     * probe, and running it per sample dominated grass generation - a hundred
     * thousand candidates in a row nearly all land in the chunk the last one
     * did.
     */
    static bool chunkFractionsAt(const float chunkPosition[3], float glX, float glY,
                                 float unitSize, float& fracX, float& fracY);

private:
    /**
     * Generate mesh for a single map chunk
     */
    static ChunkMesh generateChunkMesh(const MapChunk& chunk, int chunkX, int chunkY, int tileX, int tileY);

    /**
     * Generate vertices from heightmap
     * WoW heightmap layout: 9x9 outer + 8x8 inner vertices (145 total)
     */
    static std::vector<TerrainVertex> generateVertices(const MapChunk& chunk, int chunkX, int chunkY, int tileX, int tileY);

    /**
     * Generate triangle indices
     * Creates triangles that connect the heightmap vertices
     * Skips quads that are marked as holes in the chunk
     */
    static std::vector<TerrainIndex> generateIndices(const MapChunk& chunk);


    /**
     * Convert WoW's compressed normals to float
     */
    static void decompressNormal(const int8_t* compressedNormal, float* normal);

    /**
     * Get height at grid position from WoW's 9x9+8x8 layout
     */
    static float getHeightAt(const HeightMap& heightMap, int x, int y);



    // Terrain constants
    // WoW terrain: 64x64 tiles, each tile = 533.33 yards, each chunk = 33.33 yards
    // One ADT tile, from the coordinate header rather than spelled again:
    // the value is a truncation of 1600/3, and a second spelling of a
    // truncation is a second answer to where a tile boundary is.
    static constexpr float TILE_SIZE = core::coords::TILE_SIZE;
    static constexpr float CHUNK_SIZE = TILE_SIZE / 16.0f; // One chunk = 33.33 yards (16 chunks per tile)
    static constexpr float GRID_STEP = CHUNK_SIZE / 8.0f;  // 8 quads per chunk = 4.17 yards per vertex
};

} // namespace pipeline
} // namespace wowee
