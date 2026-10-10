#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace wowee {
namespace pipeline {

/**
 * WMO (World Model Object) Format
 *
 * WMO files contain buildings, dungeons, and large structures.
 * Structure:
 * - Root WMO file: Contains groups, materials, doodad sets
 * - Group WMO files: Individual rooms/sections (_XXX.wmo)
 *
 * Reference: https://wowdev.wiki/WMO
 */

// WMO Material
struct WMOMaterial {
    uint32_t flags;
    uint32_t shader;
    uint32_t blendMode;
    uint32_t texture1;          // Diffuse texture index
    uint32_t color1;
    uint32_t texture2;          // Environment/detail texture
    uint32_t color2;
    uint32_t texture3;
    uint32_t color3;
    float runtime[4];           // Runtime data
};

// WMO Group Info
struct WMOGroupInfo {
    uint32_t flags;
    glm::vec3 boundingBoxMin;
    glm::vec3 boundingBoxMax;
    int32_t nameOffset;         // Group name in MOGN chunk
};

// WMO Light
struct WMOLight {
    uint32_t type;              // 0=omni, 1=spot, 2=directional, 3=ambient
    uint8_t useAttenuation;
    uint8_t pad[3];
    glm::vec4 color;
    glm::vec3 position;
    float intensity;
    float attenuationStart;
    float attenuationEnd;
    float unknown[4];
};

// WMO Doodad Set (collection of M2 models placed in WMO)
struct WMODoodadSet {
    char name[20];
    uint32_t startIndex;        // First doodad in MODD
    uint32_t count;             // Number of doodads
    uint32_t padding;
};

// WMO Doodad Instance
struct WMODoodad {
    uint32_t nameIndex;         // Index into MODN (doodad names)
    glm::vec3 position;
    glm::quat rotation;         // Quaternion rotation
    float scale;
    glm::vec4 color;           // BGRA color
};

// WMO Fog (MFOG, 48 bytes an entry). Entry 0 is the model's own fog; the
// others are spheres a group names in its MOGP fog indices. The client reads
// the whole record (Wow.exe 3.3.5a 0x007a1150) and uses the first fog for
// air and the second for liquid.
struct WMOFog {
    uint32_t flags = 0;          // 0x01: radius ignored (not a sphere)
    glm::vec3 position{0.0f};    // model space
    float smallRadius = 0.0f;    // full weight inside this
    float largeRadius = 0.0f;    // none outside this
    float endDist = 0.0f;        // fog end, yards
    float startFactor = 0.0f;    // fog start as a fraction of the end
    glm::vec4 color1{0.0f};      // fog colour, rgb 0..1
    float endDist2 = 0.0f;       // the same under liquid
    float startFactor2 = 0.0f;
    glm::vec4 color2{0.0f};
};

// WMO Portal
struct WMOPortal {
    uint16_t startVertex;
    uint16_t vertexCount;
    uint16_t planeIndex;
    uint16_t padding;
};

// WMO Portal Plane
struct WMOPortalPlane {
    glm::vec3 normal;
    float distance;
};

// WMO Portal Reference (MOPR chunk) - links portals to groups
struct WMOPortalRef {
    uint16_t portalIndex;   // Index into portals array
    uint16_t groupIndex;    // Group on other side of portal
    int16_t side;           // Which side of portal plane (-1 or 1)
    uint16_t padding;
};

// WMO Liquid (MLIQ chunk data)
/// A WMO group's LiquidType (the group's +0x144, set by 0x007d82e0). MOGP's
/// liquid (+0x34 in its header) is a LiquidType id where the root's MOHD
/// flags have 4; otherwise it is the old numbering, one less, with 15 for
/// none. The basic types at or below 20 then become the WMO ones
/// (0x007d7310): water 13 - 14 in a group flagged 0x80000 - ocean 14,
/// magma 19, slime 20.
inline uint32_t wmoGroupLiquidType(uint32_t mogpLiquid, uint32_t mogpFlags, uint32_t mohdFlags) {
    uint32_t t = mogpLiquid;
    if ((mohdFlags & 4) == 0) t = mogpLiquid == 15 ? 0 : mogpLiquid + 1;
    if (t != 0 && t < 0x15) {
        switch ((t - 1) & 3) {
            case 0: return (mogpFlags & 0x80000) != 0 ? 14u : 13u;
            case 1: return 14u;
            case 2: return 19u;
            default: return 20u;
        }
    }
    return t;
}

struct WMOLiquid {
    uint32_t xVerts = 0;        // Vertices in X direction
    uint32_t yVerts = 0;        // Vertices in Y direction
    uint32_t xTiles = 0;        // Tiles in X (= xVerts - 1)
    uint32_t yTiles = 0;        // Tiles in Y (= yVerts - 1)
    glm::vec3 basePosition;     // Corner position in model space
    uint16_t materialId = 0;    // MLIQ's material id (an index into MOMT)
    /// The group's LiquidType (0x007d82e0, 0x007d7310), from MOGP's liquid
    /// and the root's MOHD flags; see wmoGroupLiquidType.
    uint32_t liquidType = 0;
    /// The root's MOGI flags for this group; 0x48 clear there also draws its
    /// liquid the interior way (0x007bde50, 0x00793d20).
    uint32_t groupInfoFlags = 0;
    std::vector<float> heights; // Height per vertex (xVerts * yVerts)
    std::vector<uint8_t> flags; // Flags per tile (xTiles * yTiles)
    /// Each vertex's first four bytes, ahead of its height: a water's depth
    /// in the low byte, or a magma's two int16 texture coordinates. The client
    /// draws with both (0x007a7b00).
    std::vector<uint32_t> vertexInfo;
    /// MLIQ's own material id: the MOMT entry whose diffuse colour an
    /// interior liquid is drawn in (the group's +0x130, 0x007a6d70).
    uint16_t momtIndex = 0;
    /// The group's MOGP flags; 0x48 decides whether its liquid is drawn the
    /// interior way (0x00793d20).
    uint32_t groupFlags = 0;

    [[nodiscard]] bool hasLiquid() const { return xVerts > 0 && yVerts > 0; }
};

// WMO Group Vertex
struct WMOVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 texCoord;
    glm::vec4 color;           // Vertex color
    /// The second MOTV and MOCV (MOGP flags 0x2000000 and 0x1000000), which
    /// MapObjComposite blends its two textures by: the second layer's UVs,
    /// and the colour whose alpha is the blend (0x007a9380 hands them to the
    /// program as attributes 7 and 5).
    glm::vec2 texCoord2{0.0f};
    glm::vec4 color2{0.0f};
};

// WMO Batch (render batch)
struct WMOBatch {
    uint32_t startIndex;   // First index (this is uint32 in file format)
    uint16_t indexCount;   // Number of indices
    uint16_t startVertex;
    uint16_t lastVertex;
    uint8_t flags;
    uint8_t materialId;
};

// WMO Group (individual room/section)
struct WMOGroup {
    uint32_t flags;
    glm::vec3 boundingBoxMin;
    glm::vec3 boundingBoxMax;
    uint16_t portalStart;
    uint16_t portalCount;
    /// MOGP's first batch count: the transition batches, which come first
    /// in MOBA. The client keeps their vertex colours' alpha (0x007d7380).
    uint16_t transBatchCount = 0;
    /// The second: the interior batches, which follow them. 0x007ac9f0 draws
    /// these unlit in a group with vertex colours.
    uint16_t intBatchCount = 0;
    uint32_t fogIndices[4];     // Fog references
    uint32_t liquidType;
    uint32_t groupId;
    /// MOGP's WMOAreaTable group id (+0x38).
    int32_t areaGroupId = 0;

    // Geometry
    std::vector<WMOVertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<WMOBatch> batches;
    std::vector<uint8_t> triFlags;  // Per-triangle MOPY flags (0x04 = detail/no-collide)

    // Portals
    std::vector<WMOPortal> portals;
    std::vector<glm::vec3> portalVertices;

    // BSP tree (for collision - optional)
    std::vector<uint8_t> bspNodes;

    // Liquid data (MLIQ chunk)
    WMOLiquid liquid;

    // MODR: indices into the model's MODD doodads that belong to this group
    std::vector<uint16_t> doodadRefs;

    std::string name;
    std::string description;
};

// Complete WMO Model
struct WMOModel {
    // Runtime source path when known. The binary format does not store its root
    // filename, but a few rendering classifications need the owning WMO family.
    std::string sourcePath;

    // Root WMO data (from MOHD chunk)
    uint32_t version = 0;
    uint32_t nTextures = 0;  // Added - was missing, caused offset issues
    uint32_t nGroups = 0;
    uint32_t nPortals = 0;
    uint32_t nLights = 0;
    uint32_t nDoodadNames = 0;
    uint32_t nDoodadDefs = 0;
    uint32_t nDoodadSets = 0;
    /// MOHD's WMOAreaTable WMOID.
    uint32_t wmoId = 0;

    glm::vec3 ambientColor;     // MOHD ambient color (used for interior group lighting)
    uint32_t flags = 0;         // MOHD flags
    glm::vec3 boundingBoxMin;
    glm::vec3 boundingBoxMax;

    // Materials and textures
    std::vector<WMOMaterial> materials;
    std::vector<std::string> textures;
    std::unordered_map<uint32_t, uint32_t> textureOffsetToIndex;  // MOTX offset -> texture array index

    // Groups (rooms/sections)
    std::vector<WMOGroupInfo> groupInfo;
    std::vector<WMOGroup> groups;

    // Portals (visibility culling)
    std::vector<WMOPortal> portals;
    std::vector<WMOPortalPlane> portalPlanes;
    std::vector<glm::vec3> portalVertices;
    std::vector<WMOPortalRef> portalRefs;  // MOPR chunk - portal-to-group links

    // Lights
    std::vector<WMOLight> lights;

    // Doodads (M2 models placed in WMO)
    // Keyed by byte offset into MODN chunk (nameIndex in MODD references these offsets)
    std::unordered_map<uint32_t, std::string> doodadNames;
    std::vector<WMODoodad> doodads;
    std::vector<WMODoodadSet> doodadSets;

    // Fog
    std::vector<WMOFog> fogs;

    // Group names
    std::vector<std::string> groupNames;
    std::vector<uint8_t> groupNameRaw;  // Raw MOGN chunk for offset-based name lookup

    [[nodiscard]] bool isValid() const {
        return nGroups > 0 && !groups.empty();
    }
};

class WMOLoader {
public:
    /**
     * Load root WMO file
     *
     * @param wmoData Raw WMO file bytes
     * @return Parsed WMO model (without group geometry)
     */
    static WMOModel load(const std::vector<uint8_t>& wmoData);

    /**
     * Load WMO group file
     *
     * @param groupData Raw WMO group file bytes
     * @param model Model to populate with group data
     * @param groupIndex Group index to load
     * @return True if successful
     */
    static bool loadGroup(const std::vector<uint8_t>& groupData,
                         WMOModel& model,
                         uint32_t groupIndex);
};

} // namespace pipeline
} // namespace wowee
