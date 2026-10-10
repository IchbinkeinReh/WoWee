#include "pipeline/wmo_loader.hpp"
#include "core/logger.hpp"
#include <cstddef>
#include <cstring>
#include <glm/gtc/quaternion.hpp>

namespace wowee {
namespace pipeline {

namespace {

// WMO chunk identifiers
constexpr uint32_t MVER = 0x4D564552;  // Version
constexpr uint32_t MOHD = 0x4D4F4844;  // Header
constexpr uint32_t MOTX = 0x4D4F5458;  // Textures
constexpr uint32_t MOMT = 0x4D4F4D54;  // Materials
constexpr uint32_t MOGN = 0x4D4F474E;  // Group names
constexpr uint32_t MOGI = 0x4D4F4749;  // Group info
constexpr uint32_t MOLT = 0x4D4F4C54;  // Lights
constexpr uint32_t MODN = 0x4D4F444E;  // Doodad names
constexpr uint32_t MODD = 0x4D4F4444;  // Doodad definitions
constexpr uint32_t MODS = 0x4D4F4453;  // Doodad sets
constexpr uint32_t MOPV = 0x4D4F5056;  // Portal vertices
constexpr uint32_t MOPT = 0x4D4F5054;  // Portal info
constexpr uint32_t MOPR = 0x4D4F5052;  // Portal references
constexpr uint32_t MFOG = 0x4D464F47;  // Fog

// WMO group chunk identifiers
constexpr uint32_t MOGP = 0x4D4F4750;  // Group header
constexpr uint32_t MOVI = 0x4D4F5649;  // Indices
constexpr uint32_t MOBA = 0x4D4F4241;  // Batches
constexpr uint32_t MOCV = 0x4D4F4356;  // Vertex colors
constexpr uint32_t MONR = 0x4D4F4E52;  // Normals
constexpr uint32_t MOTV = 0x4D4F5456;  // Texture coords
constexpr uint32_t MLIQ = 0x4D4C4951;  // Liquid
constexpr uint32_t MODR = 0x4D4F4452;  // Doodad references
constexpr uint32_t MOVT = 0x4D4F5654;  // Vertices
constexpr uint32_t MOPY = 0x4D4F5059;  // Triangle material info

// SMOMaterial (MOMT entry, 64 bytes).
struct MomtEntryDisk {
    uint32_t flags;
    uint32_t shader;
    uint32_t blendMode;
    uint32_t texture1;          // MOTX offset
    uint32_t sidnColor;         // emissive
    uint32_t frameSidnColor;    // not read
    uint32_t texture2;          // MOTX offset
    uint32_t diffColor;
    uint32_t groundType;        // not read
    uint32_t texture3;          // MOTX offset
    uint32_t color2;
    uint32_t flags2;            // not read
    uint32_t runtimeData[4];    // not read
};
static_assert(sizeof(MomtEntryDisk) == 64,
              "MomtEntryDisk is read straight from the file: 64 bytes, no padding");

// SMODoodadSet (MODS entry, 32 bytes).
struct ModsEntryDisk {
    char name[20];
    uint32_t startIndex;        // first doodad in MODD
    uint32_t count;
    uint32_t padding;
};
static_assert(sizeof(ModsEntryDisk) == 32,
              "ModsEntryDisk is read straight from the file: 32 bytes, no padding");

// MOGP header (68 bytes), ahead of the group's sub-chunks.
struct MogpHeaderDisk {
    uint32_t groupName;             // MOGN offset - not read
    uint32_t descriptiveGroupName;  // MOGN offset - not read
    uint32_t flags;
    float boundingBoxMin[3];
    float boundingBoxMax[3];
    uint16_t portalStart;
    uint16_t portalCount;
    uint16_t transBatchCount;
    uint16_t intBatchCount;
    uint16_t extBatchCount;         // not read
    uint16_t padding;
    uint8_t fogIndices[4];          // 4 x uint8, NOT 4 x uint32
    uint32_t liquidType;
    int32_t areaGroupId;            // WMOAreaTable's WMOGroupID (the client's group +0x180)
    uint32_t flags2;                // not read
    uint32_t unused;
};
static_assert(sizeof(MogpHeaderDisk) == 68,
              "MogpHeaderDisk is read straight from the file: 68 bytes, no padding");
static_assert(offsetof(MogpHeaderDisk, flags) == 8 &&
              offsetof(MogpHeaderDisk, fogIndices) == 48 &&
              offsetof(MogpHeaderDisk, areaGroupId) == 56,
              "MogpHeaderDisk fields must sit where the client reads them");

// SMOBatch (MOBA entry, 24 bytes).
struct MobaEntryDisk {
    int16_t boundingBox[6];     // not read
    uint32_t startIndex;
    uint16_t count;
    uint16_t minIndex;
    uint16_t maxIndex;
    uint8_t flags;
    uint8_t materialId;
};
static_assert(sizeof(MobaEntryDisk) == 24,
              "MobaEntryDisk is read straight from the file: 24 bytes, no padding");

// MLIQ header (30 bytes, unaligned fields - hence packed), then per-vertex
// data and per-tile flags.
#pragma pack(push, 1)
struct MliqHeaderDisk {
    uint32_t xVerts;
    uint32_t yVerts;
    uint32_t xTiles;
    uint32_t yTiles;
    float basePosition[3];
    uint16_t materialId;
};
#pragma pack(pop)
static_assert(sizeof(MliqHeaderDisk) == 30,
              "MliqHeaderDisk is read straight from the file: 30 bytes, no padding");

// Read utilities
template<typename T>
T read(const std::vector<uint8_t>& data, uint32_t& offset) {
    if (offset + sizeof(T) > data.size()) {
        return T{};
    }
    T value;
    std::memcpy(&value, &data[offset], sizeof(T));
    offset += sizeof(T);
    return value;
}

template<typename T>
std::vector<T> readArray(const std::vector<uint8_t>& data, uint32_t offset, uint32_t count) {
    std::vector<T> result;
    // Use 64-bit arithmetic to prevent uint32 overflow on crafted count values.
    // A large count (e.g., 0x20000001 with sizeof(T)=8) would wrap to a small
    // value in 32-bit, pass the bounds check, then cause a multi-GB allocation.
    uint64_t totalBytes = static_cast<uint64_t>(count) * sizeof(T);
    constexpr uint64_t kMaxReadBytes = 64u * 1024u * 1024u;  // 64MB sanity cap
    if (totalBytes > kMaxReadBytes || static_cast<uint64_t>(offset) + totalBytes > data.size()) {
        return result;
    }
    result.resize(count);
    std::memcpy(result.data(), &data[offset], static_cast<size_t>(totalBytes));
    return result;
}

std::string readString(const std::vector<uint8_t>& data, uint32_t offset) {
    std::string result;
    while (offset < data.size() && data[offset] != 0) {
        result += static_cast<char>(data[offset++]);
    }
    return result;
}

} // anonymous namespace

WMOModel WMOLoader::load(const std::vector<uint8_t>& wmoData) {
    WMOModel model;

    if (wmoData.size() < 8) {
        core::Logger::getInstance().error("WMO data too small");
        return model;
    }

    // WMO loader logs disabled

    uint32_t offset = 0;

    // Parse chunks
    while (offset + 8 <= wmoData.size()) {
        uint32_t chunkId = read<uint32_t>(wmoData, offset);
        uint32_t chunkSize = read<uint32_t>(wmoData, offset);

        // 64-bit, for the reason readArray gives above: offset and chunkSize
        // are both uint32 and both come from the file, so the sum wraps and a
        // crafted chunkSize walks the loop past the end of the buffer.
        if (static_cast<uint64_t>(offset) + chunkSize > wmoData.size()) {
            core::Logger::getInstance().warning("Chunk extends beyond file");
            break;
        }

        uint32_t chunkStart = offset;
        uint32_t chunkEnd = offset + chunkSize;

        switch (chunkId) {
            case MVER: {
                model.version = read<uint32_t>(wmoData, offset);
                // WMO version log disabled
                break;
            }

            case MOHD: {
                // Header - SMOHeader structure (WotLK 3.3.5a)
                model.nTextures = read<uint32_t>(wmoData, offset);   // Was missing!
                model.nGroups = read<uint32_t>(wmoData, offset);
                model.nPortals = read<uint32_t>(wmoData, offset);
                model.nLights = read<uint32_t>(wmoData, offset);
                model.nDoodadNames = read<uint32_t>(wmoData, offset);
                model.nDoodadDefs = read<uint32_t>(wmoData, offset);
                model.nDoodadSets = read<uint32_t>(wmoData, offset);

                uint32_t ambColor = read<uint32_t>(wmoData, offset);  // Ambient color (BGRA)
                // Unpack BGRA bytes to normalized [0,1] RGB
                model.ambientColor.r = ((ambColor >> 16) & 0xFF) / 255.0f;
                model.ambientColor.g = ((ambColor >>  8) & 0xFF) / 255.0f;
                model.ambientColor.b = ((ambColor >>  0) & 0xFF) / 255.0f;
                model.wmoId = read<uint32_t>(wmoData, offset);  // WMOAreaTable's WMOID

                model.boundingBoxMin.x = read<float>(wmoData, offset);
                model.boundingBoxMin.y = read<float>(wmoData, offset);
                model.boundingBoxMin.z = read<float>(wmoData, offset);

                model.boundingBoxMax.x = read<float>(wmoData, offset);
                model.boundingBoxMax.y = read<float>(wmoData, offset);
                model.boundingBoxMax.z = read<float>(wmoData, offset);

                // flags and numLod (uint16 each). Flag 0x2 adds the ambient
                // to the floor colour a unit is lit by (0x007c7fe0).
                model.flags = read<uint16_t>(wmoData, offset);
                offset += 2;

                core::Logger::getInstance().debug("WMO header: nTextures=", model.nTextures, " nGroups=", model.nGroups);
                break;
            }

            case MOTX: {
                // Textures - raw block of null-terminated strings
                // Material texture1/texture2/texture3 are byte offsets into this chunk.
                // We must map every offset to its texture index.
                uint32_t texOffset = chunkStart;
                uint32_t texIndex = 0;
                core::Logger::getInstance().debug("MOTX chunk: ", chunkSize, " bytes");
                while (texOffset < chunkEnd) {
                    uint32_t relativeOffset = texOffset - chunkStart;

                    std::string texName = readString(wmoData, texOffset);
                    if (texName.empty()) {
                        // Skip null bytes (empty entries or padding)
                        texOffset++;
                        continue;
                    }

                    // Store mapping from byte offset to texture index
                    model.textureOffsetToIndex[relativeOffset] = texIndex;
                    model.textures.push_back(texName);
                    // MOTX texture log disabled
                    texOffset += texName.length() + 1;
                    texIndex++;
                }
                // WMO textures log disabled
                break;
            }

            case MOMT: {
                // Materials, SMOMaterial (wowdev.wiki): see MomtEntryDisk
                uint32_t nMaterials = chunkSize / sizeof(MomtEntryDisk);
                for (uint32_t i = 0; i < nMaterials; i++) {
                    const auto entry = read<MomtEntryDisk>(wmoData, offset);

                    WMOMaterial mat;
                    mat.flags = entry.flags;
                    mat.shader = entry.shader;
                    mat.blendMode = entry.blendMode;
                    mat.texture1 = entry.texture1;
                    mat.color1 = entry.sidnColor;
                    mat.texture2 = entry.texture2;
                    mat.color2 = entry.diffColor;
                    mat.texture3 = entry.texture3;
                    mat.color3 = entry.color2;

                    model.materials.push_back(mat);
                }
                core::Logger::getInstance().debug("WMO materials: ", model.materials.size());
                break;
            }

            case MOGN: {
                // Group names - store raw chunk for offset-based lookup (MOGI nameOffset)
                if (chunkSize > 0 && chunkEnd <= wmoData.size()) {
                    model.groupNameRaw.assign(wmoData.begin() + chunkStart, wmoData.begin() + chunkEnd);
                }
                uint32_t nameOffset = chunkStart;
                while (nameOffset < chunkEnd) {
                    std::string name = readString(wmoData, nameOffset);
                    if (name.empty()) break;
                    model.groupNames.push_back(name);
                    nameOffset += name.length() + 1;
                }
                // WMO group names log disabled
                break;
            }

            case MOGI: {
                // Group info
                uint32_t nGroupInfo = chunkSize / 32;  // Each group info is 32 bytes
                for (uint32_t i = 0; i < nGroupInfo; i++) {
                    WMOGroupInfo info;
                    info.flags = read<uint32_t>(wmoData, offset);
                    info.boundingBoxMin.x = read<float>(wmoData, offset);
                    info.boundingBoxMin.y = read<float>(wmoData, offset);
                    info.boundingBoxMin.z = read<float>(wmoData, offset);
                    info.boundingBoxMax.x = read<float>(wmoData, offset);
                    info.boundingBoxMax.y = read<float>(wmoData, offset);
                    info.boundingBoxMax.z = read<float>(wmoData, offset);
                    info.nameOffset = read<int32_t>(wmoData, offset);

                    model.groupInfo.push_back(info);
                }
                core::Logger::getInstance().debug("WMO group info: ", model.groupInfo.size());
                break;
            }

            case MOLT: {
                // Lights
                uint32_t nLights = chunkSize / 48;  // Approximate size
                for (uint32_t i = 0; i < nLights && offset < chunkEnd; i++) {
                    WMOLight light;
                    light.type = read<uint32_t>(wmoData, offset);
                    light.useAttenuation = read<uint8_t>(wmoData, offset);
                    light.pad[0] = read<uint8_t>(wmoData, offset);
                    light.pad[1] = read<uint8_t>(wmoData, offset);
                    light.pad[2] = read<uint8_t>(wmoData, offset);

                    light.color.r = read<float>(wmoData, offset);
                    light.color.g = read<float>(wmoData, offset);
                    light.color.b = read<float>(wmoData, offset);
                    light.color.a = read<float>(wmoData, offset);

                    light.position.x = read<float>(wmoData, offset);
                    light.position.y = read<float>(wmoData, offset);
                    light.position.z = read<float>(wmoData, offset);

                    light.intensity = read<float>(wmoData, offset);
                    light.attenuationStart = read<float>(wmoData, offset);
                    light.attenuationEnd = read<float>(wmoData, offset);

                    for (float& value : light.unknown) {
                        value = read<float>(wmoData, offset);
                    }

                    model.lights.push_back(light);
                }
                core::Logger::getInstance().debug("WMO lights: ", model.lights.size());
                break;
            }

            case MODN: {
                // Doodad names - stored by byte offset into the MODN chunk
                // (MODD nameIndex is a byte offset, not a vector index)
                uint32_t nameOffset = 0;  // Offset relative to chunk start
                while (chunkStart + nameOffset < chunkEnd) {
                    std::string name = readString(wmoData, chunkStart + nameOffset);
                    if (!name.empty()) {
                        model.doodadNames[nameOffset] = name;
                    }
                    nameOffset += name.length() + 1;
                }
                core::Logger::getInstance().debug("Loaded ", model.doodadNames.size(), " doodad names");
                break;
            }

            case MODD: {
                // Doodad definitions
                uint32_t nDoodads = chunkSize / 40;  // Each doodad is 40 bytes
                for (uint32_t i = 0; i < nDoodads; i++) {
                    WMODoodad doodad;

                    // WMO doodad placement: name index packed in lower 24 bits, flags in upper 8.
                    // The name index is an offset into the MODN string table (doodad names).
                    constexpr uint32_t kDoodadNameIndexMask = 0x00FFFFFF;
                    uint32_t nameAndFlags = read<uint32_t>(wmoData, offset);
                    doodad.nameIndex = nameAndFlags & kDoodadNameIndexMask;

                    doodad.position.x = read<float>(wmoData, offset);
                    doodad.position.y = read<float>(wmoData, offset);
                    doodad.position.z = read<float>(wmoData, offset);

                    // C4Quaternion in file: x, y, z, w
                    doodad.rotation.x = read<float>(wmoData, offset);
                    doodad.rotation.y = read<float>(wmoData, offset);
                    doodad.rotation.z = read<float>(wmoData, offset);
                    doodad.rotation.w = read<float>(wmoData, offset);

                    doodad.scale = read<float>(wmoData, offset);

                    uint32_t color = read<uint32_t>(wmoData, offset);
                    doodad.color.b = ((color >> 0) & 0xFF) / 255.0f;
                    doodad.color.g = ((color >> 8) & 0xFF) / 255.0f;
                    doodad.color.r = ((color >> 16) & 0xFF) / 255.0f;
                    doodad.color.a = ((color >> 24) & 0xFF) / 255.0f;

                    model.doodads.push_back(doodad);
                }
                core::Logger::getInstance().debug("WMO doodads: ", model.doodads.size());
                break;
            }

            case MODS: {
                // Doodad sets: 20-byte name + 3×uint32 = 32 bytes each.
                // Through the bounds-checked read<T>: the chunk lies inside
                // the file, so every whole entry does too.
                uint32_t nSets = chunkSize / sizeof(ModsEntryDisk);
                for (uint32_t i = 0; i < nSets; i++) {
                    const auto entry = read<ModsEntryDisk>(wmoData, offset);
                    WMODoodadSet set;
                    static_assert(sizeof(set.name) == sizeof(entry.name), "MODS names are 20 bytes");
                    std::memcpy(set.name, entry.name, sizeof(set.name));
                    set.startIndex = entry.startIndex;
                    set.count = entry.count;
                    set.padding = entry.padding;

                    model.doodadSets.push_back(set);
                }
                core::Logger::getInstance().debug("WMO doodad sets: ", model.doodadSets.size());
                break;
            }

            case MOPV: {
                // Portal vertices
                uint32_t nVerts = chunkSize / 12;  // Each vertex is 3 floats
                for (uint32_t i = 0; i < nVerts; i++) {
                    glm::vec3 vert;
                    vert.x = read<float>(wmoData, offset);
                    vert.y = read<float>(wmoData, offset);
                    vert.z = read<float>(wmoData, offset);
                    model.portalVertices.push_back(vert);
                }
                break;
            }

            case MOPT: {
                // Portal info
                uint32_t nPortals = chunkSize / 20;  // Each portal reference is 20 bytes
                for (uint32_t i = 0; i < nPortals; i++) {
                    WMOPortal portal;
                    portal.startVertex = read<uint16_t>(wmoData, offset);
                    portal.vertexCount = read<uint16_t>(wmoData, offset);
                    portal.planeIndex = read<uint16_t>(wmoData, offset);
                    portal.padding = read<uint16_t>(wmoData, offset);

                    // Skip additional data (12 bytes)
                    offset += 12;

                    model.portals.push_back(portal);
                }
                core::Logger::getInstance().debug("WMO portals: ", model.portals.size());
                break;
            }

            case MOPR: {
                // Portal references - links groups via portals
                uint32_t nRefs = chunkSize / 8;  // Each reference is 8 bytes
                for (uint32_t i = 0; i < nRefs; i++) {
                    WMOPortalRef ref;
                    ref.portalIndex = read<uint16_t>(wmoData, offset);
                    ref.groupIndex = read<uint16_t>(wmoData, offset);
                    ref.side = read<int16_t>(wmoData, offset);
                    ref.padding = read<uint16_t>(wmoData, offset);
                    model.portalRefs.push_back(ref);
                }
                core::Logger::getInstance().debug("WMO portal refs: ", model.portalRefs.size());
                break;
            }

            case MFOG: {
                // Fog spheres, 48 bytes each (0x007a1150 reads them as twelve
                // dwords). Colours are BGRA, red in the third byte.
                const auto bgra = [](uint32_t c) {
                    return glm::vec4(static_cast<float>((c >> 16) & 0xFF) / 255.0f,
                                     static_cast<float>((c >> 8) & 0xFF) / 255.0f,
                                     static_cast<float>(c & 0xFF) / 255.0f,
                                     static_cast<float>((c >> 24) & 0xFF) / 255.0f);
                };
                const uint32_t nFogs = chunkSize / 48;
                for (uint32_t i = 0; i < nFogs; i++) {
                    WMOFog fog;
                    fog.flags = read<uint32_t>(wmoData, offset);
                    fog.position.x = read<float>(wmoData, offset);
                    fog.position.y = read<float>(wmoData, offset);
                    fog.position.z = read<float>(wmoData, offset);
                    fog.smallRadius = read<float>(wmoData, offset);
                    fog.largeRadius = read<float>(wmoData, offset);
                    fog.endDist = read<float>(wmoData, offset);
                    fog.startFactor = read<float>(wmoData, offset);
                    fog.color1 = bgra(read<uint32_t>(wmoData, offset));
                    fog.endDist2 = read<float>(wmoData, offset);
                    fog.startFactor2 = read<float>(wmoData, offset);
                    fog.color2 = bgra(read<uint32_t>(wmoData, offset));
                    model.fogs.push_back(fog);
                }
                break;
            }

            default:
                // Unknown chunk, skip it
                break;
        }

        offset = chunkEnd;
    }

    // Initialize groups array. Cap at a sanity limit so a hostile or
    // corrupted WMO header can't trigger a multi-gigabyte allocation -
    // Blizzard's largest real WMOs cap out around a few hundred groups.
    constexpr uint32_t kMaxWMOGroups = 4096;
    if (model.nGroups > kMaxWMOGroups) {
        core::Logger::getInstance().warning(
            "WMO: nGroups=", model.nGroups,
            " exceeds sanity cap ", kMaxWMOGroups, ", clamping");
        model.nGroups = kMaxWMOGroups;
    }
    model.groups.resize(model.nGroups);

    // WMO loaded log disabled
    return model;
}

bool WMOLoader::loadGroup(const std::vector<uint8_t>& groupData,
                          WMOModel& model,
                          uint32_t groupIndex) {
    if (groupIndex >= model.groups.size()) {
        core::Logger::getInstance().error("Invalid group index: ", groupIndex);
        return false;
    }

    if (groupData.size() < 20) {
        core::Logger::getInstance().error("WMO group file too small");
        return false;
    }

    auto& group = model.groups[groupIndex];
    group.groupId = groupIndex;

    uint32_t offset = 0;

    // Parse chunks in group file
    while (offset + 8 < groupData.size()) {
        uint32_t chunkId = read<uint32_t>(groupData, offset);
        uint32_t chunkSize = read<uint32_t>(groupData, offset);
        uint32_t chunkEnd = offset + chunkSize;

        if (chunkEnd > groupData.size()) {
            break;
        }

        if (chunkId == MVER) {
            // Version - skip
        }
        else if (chunkId == MOGP) {
            // Group header - parse sub-chunks
            // MOGP header is 68 bytes, followed by sub-chunks
            if (chunkSize < sizeof(MogpHeaderDisk)) {
                offset = chunkEnd;
                continue;
            }

            // Read MOGP header
            uint32_t mogpOffset = offset;
            const auto mogp = read<MogpHeaderDisk>(groupData, mogpOffset);
            group.flags = mogp.flags;
            bool isInterior = (group.flags & 0x2000) != 0;
            core::Logger::getInstance().debug("  Group flags: 0x", std::hex, group.flags, std::dec,
                                              (isInterior ? " (INTERIOR)" : " (exterior)"));
            group.boundingBoxMin = glm::vec3(mogp.boundingBoxMin[0], mogp.boundingBoxMin[1], mogp.boundingBoxMin[2]);
            group.boundingBoxMax = glm::vec3(mogp.boundingBoxMax[0], mogp.boundingBoxMax[1], mogp.boundingBoxMax[2]);
            group.portalStart = mogp.portalStart;
            group.portalCount = mogp.portalCount;
            group.transBatchCount = mogp.transBatchCount;
            group.intBatchCount = mogp.intBatchCount;
            // fogIndices: 4 × uint8 (4 bytes total, NOT 4 × uint32)
            for (int i = 0; i < 4; i++) group.fogIndices[i] = mogp.fogIndices[i];
            group.liquidType = mogp.liquidType;
            // WMOAreaTable's WMOGroupID (the client's group +0x180).
            group.areaGroupId = mogp.areaGroupId;
            // The sub-chunks follow the 68-byte header
            mogpOffset = offset + static_cast<uint32_t>(sizeof(MogpHeaderDisk));

            // Parse sub-chunks within MOGP
            int groupLogCount = 0;
            int motvSeen = 0;
            int mocvSeen = 0;
            while (mogpOffset + 8 < chunkEnd) {
                uint32_t subChunkId = read<uint32_t>(groupData, mogpOffset);
                uint32_t subChunkSize = read<uint32_t>(groupData, mogpOffset);
                // Widened for the same reason as the root chunk loop: both
                // operands are uint32 read from the file, and a wrapped
                // subChunkEnd is smaller than mogpOffset, which every
                // "parseOffset + n <= subChunkEnd" test below then reads as
                // "no room" or, worse, as room that is not there.
                const uint64_t subChunkEnd64 =
                    static_cast<uint64_t>(mogpOffset) + subChunkSize;
                if (subChunkEnd64 > chunkEnd) {
                    break;
                }
                // Past the guard it is <= chunkEnd, so this cannot truncate.
                const uint32_t subChunkEnd = static_cast<uint32_t>(subChunkEnd64);

                // Debug: log chunk magic as string
                char magic[5] = {0};
                magic[0] = (subChunkId >> 0) & 0xFF;
                magic[1] = (subChunkId >> 8) & 0xFF;
                magic[2] = (subChunkId >> 16) & 0xFF;
                magic[3] = (subChunkId >> 24) & 0xFF;
                // Not static - previously this throttle was per-process, silencing
                // all WMO group logging after the first 30 sub-chunks globally.
                if (groupLogCount < 30) {
                    core::Logger::getInstance().debug("  WMO sub-chunk: ", magic, " (0x", std::hex, subChunkId, std::dec, ") size=", subChunkSize);
                    groupLogCount++;
                }

                if (subChunkId == MOVT) { // Vertices
                    uint32_t vertexCount = subChunkSize / 12; // 3 floats per vertex
                    for (uint32_t i = 0; i < vertexCount; i++) {
                        WMOVertex vertex;
                        // Keep vertices in WoW model-local coords - coordinate swap done in model matrix
                        vertex.position.x = read<float>(groupData, mogpOffset);
                        vertex.position.y = read<float>(groupData, mogpOffset);
                        vertex.position.z = read<float>(groupData, mogpOffset);
                        vertex.normal = glm::vec3(0, 0, 1);
                        vertex.texCoord = glm::vec2(0, 0);
                        vertex.color = glm::vec4(1, 1, 1, 1);
                        group.vertices.push_back(vertex);
                    }
                }
                else if (subChunkId == MOVI) { // Indices
                    uint32_t indexCount = subChunkSize / 2; // uint16_t per index
                    for (uint32_t i = 0; i < indexCount; i++) {
                        group.indices.push_back(read<uint16_t>(groupData, mogpOffset));
                    }
                }
                else if (subChunkId == MOPY) { // Triangle material info
                    // 2 bytes per triangle: flags (uint8) + materialId (uint8)
                    // flag 0x04 = detail/decorative geometry (no collision)
                    uint32_t triCount = subChunkSize / 2;
                    group.triFlags.resize(triCount);
                    for (uint32_t i = 0; i < triCount; i++) {
                        group.triFlags[i] = read<uint8_t>(groupData, mogpOffset);
                        read<uint8_t>(groupData, mogpOffset); // materialId (skip)
                    }
                }
                else if (subChunkId == MONR) { // Normals
                    uint32_t normalCount = subChunkSize / 12;
                    core::Logger::getInstance().debug("  MONR: ", normalCount, " normals for ", group.vertices.size(), " vertices");
                    for (uint32_t i = 0; i < normalCount && i < group.vertices.size(); i++) {
                        group.vertices[i].normal.x = read<float>(groupData, mogpOffset);
                        group.vertices[i].normal.y = read<float>(groupData, mogpOffset);
                        group.vertices[i].normal.z = read<float>(groupData, mogpOffset);
                    }
                    if (normalCount > 0 && !group.vertices.empty()) {
                        const auto& n = group.vertices[0].normal;
                        core::Logger::getInstance().debug("    First normal: (", n.x, ", ", n.y, ", ", n.z, ")");
                    }
                }
                else if (subChunkId == MOTV) { // Texture coords
                    // The first MOTV is the UVs; a second one (MOGP 0x2000000)
                    // is the second layer's, which used to overwrite them.
                    const bool second = motvSeen++ > 0;
                    uint32_t texCoordCount = subChunkSize / 8;
                    core::Logger::getInstance().debug("  MOTV: ", texCoordCount, " tex coords for ", group.vertices.size(), " vertices");
                    for (uint32_t i = 0; i < texCoordCount && i < group.vertices.size(); i++) {
                        glm::vec2& uv = second ? group.vertices[i].texCoord2 : group.vertices[i].texCoord;
                        uv.x = read<float>(groupData, mogpOffset);
                        uv.y = read<float>(groupData, mogpOffset);
                    }
                    if (texCoordCount > 0 && !group.vertices.empty()) {
                        core::Logger::getInstance().debug("    First UV: (", group.vertices[0].texCoord.x, ", ", group.vertices[0].texCoord.y, ")");
                    }
                }
                else if (subChunkId == MOCV) { // Vertex colors
                    // Update vertex colors
                    // The first MOCV is the colours; a second one (MOGP
                    // 0x1000000) is MapObjComposite's blend.
                    const bool second = mocvSeen++ > 0;
                    uint32_t colorCount = subChunkSize / 4;
                    core::Logger::getInstance().debug("  MOCV: ", colorCount, " vertex colors for ", group.vertices.size(), " vertices");
                    for (uint32_t i = 0; i < colorCount && i < group.vertices.size(); i++) {
                        uint8_t b = read<uint8_t>(groupData, mogpOffset);
                        uint8_t g = read<uint8_t>(groupData, mogpOffset);
                        uint8_t r = read<uint8_t>(groupData, mogpOffset);
                        uint8_t a = read<uint8_t>(groupData, mogpOffset);
                        (second ? group.vertices[i].color2 : group.vertices[i].color) =
                            glm::vec4(r/255.0f, g/255.0f, b/255.0f, a/255.0f);
                    }
                    if (colorCount > 0 && !group.vertices.empty()) {
                        const auto& c = group.vertices[0].color;
                        core::Logger::getInstance().debug("    First color: (", c.r, ", ", c.g, ", ", c.b, ", ", c.a, ")");
                    }
                }
                else if (subChunkId == MODR) { // Doodad references
                    // The MODD entries this group owns. The client makes a
                    // group's doodads from this list and lights them by the
                    // group's interior or exterior flags (0x007bf740).
                    const uint32_t refCount = subChunkSize / 2;
                    group.doodadRefs.reserve(refCount);
                    for (uint32_t i = 0; i < refCount; i++) {
                        group.doodadRefs.push_back(read<uint16_t>(groupData, mogpOffset));
                    }
                }
                else if (subChunkId == MOBA) { // Batches
                    // SMOBatch structure (24 bytes): see MobaEntryDisk
                    uint32_t batchCount = subChunkSize / sizeof(MobaEntryDisk);
                    for (uint32_t i = 0; i < batchCount; i++) {
                        const auto entry = read<MobaEntryDisk>(groupData, mogpOffset);
                        WMOBatch batch;
                        batch.startIndex = entry.startIndex;
                        batch.indexCount = entry.count;
                        batch.startVertex = entry.minIndex;
                        batch.lastVertex = entry.maxIndex;
                        batch.flags = entry.flags;
                        batch.materialId = entry.materialId;
                        group.batches.push_back(batch);

                        // Non-static so each group gets its own throttle window.
                        if (static_cast<int>(i) < 15) {
                            core::Logger::getInstance().debug("  Batch[", i, "]: start=", batch.startIndex,
                                " count=", batch.indexCount, " verts=[", batch.startVertex, "-",
                                batch.lastVertex, "] mat=", static_cast<int>(batch.materialId), " flags=", static_cast<int>(batch.flags));
                        }
                    }
                }
                else if (subChunkId == MLIQ) { // MLIQ - WMO liquid data
                    // Basic WotLK layout: MliqHeaderDisk, then
                    // (optional pad/unknown bytes)
                    // followed by vertex/tile payload
                    uint32_t parseOffset = mogpOffset;
                    if (parseOffset + sizeof(MliqHeaderDisk) <= subChunkEnd) {
                        const auto mliq = read<MliqHeaderDisk>(groupData, parseOffset);
                        group.liquid.xVerts = mliq.xVerts;
                        group.liquid.yVerts = mliq.yVerts;
                        group.liquid.xTiles = mliq.xTiles;
                        group.liquid.yTiles = mliq.yTiles;
                        group.liquid.basePosition =
                            glm::vec3(mliq.basePosition[0], mliq.basePosition[1], mliq.basePosition[2]);
                        group.liquid.materialId = mliq.materialId;
                        group.liquid.momtIndex = group.liquid.materialId;
                        group.liquid.groupFlags = group.flags;
                        group.liquid.liquidType =
                            wmoGroupLiquidType(group.liquidType, group.flags, model.flags);
                        group.liquid.groupInfoFlags =
                            groupIndex < model.groupInfo.size() ? model.groupInfo[groupIndex].flags : 0u;

                        // Keep parser resilient across minor format variants:
                        // prefer explicit per-vertex floats, otherwise fall back to flat.
                        const size_t vertexCount =
                            static_cast<size_t>(group.liquid.xVerts) * static_cast<size_t>(group.liquid.yVerts);
                        const size_t tileCount =
                            static_cast<size_t>(group.liquid.xTiles) * static_cast<size_t>(group.liquid.yTiles);
                        const size_t bytesRemaining = (subChunkEnd > parseOffset) ? (subChunkEnd - parseOffset) : 0;

                        group.liquid.heights.clear();
                        group.liquid.flags.clear();

                        // MLIQ vertex data: each vertex is 8 bytes -
                        // 4 bytes flow/unknown data + 4 bytes float height.
                        const size_t VERTEX_STRIDE = 8; // bytes per vertex
                        if (vertexCount > 0 && bytesRemaining >= vertexCount * VERTEX_STRIDE) {
                            group.liquid.heights.resize(vertexCount);
                            group.liquid.vertexInfo.resize(vertexCount);
                            for (size_t i = 0; i < vertexCount; i++) {
                                group.liquid.vertexInfo[i] = read<uint32_t>(groupData, parseOffset);
                                group.liquid.heights[i] = read<float>(groupData, parseOffset);
                            }
                        } else if (vertexCount > 0 && bytesRemaining >= vertexCount * sizeof(float)) {
                            // Fallback: try reading as plain floats if stride doesn't fit
                            group.liquid.heights.resize(vertexCount);
                            for (size_t i = 0; i < vertexCount; i++) {
                                group.liquid.heights[i] = read<float>(groupData, parseOffset);
                            }
                        } else if (vertexCount > 0) {
                            group.liquid.heights.resize(vertexCount, group.liquid.basePosition.z);
                        }

                        if (tileCount > 0 && parseOffset + tileCount <= subChunkEnd) {
                            group.liquid.flags.resize(tileCount);
                            std::memcpy(group.liquid.flags.data(), &groupData[parseOffset], tileCount);
                        } else if (tileCount > 0) {
                            group.liquid.flags.resize(tileCount, 0);
                        }
                    }
                }

                mogpOffset = subChunkEnd;
            }
        }

        offset = chunkEnd;
    }

    // Create a default batch if none were loaded
    if (group.batches.empty() && !group.indices.empty()) {
        WMOBatch batch;
        batch.startIndex = 0;
        batch.indexCount = static_cast<uint16_t>(group.indices.size());
        batch.materialId = 0;
        group.batches.push_back(batch);
    }

    core::Logger::getInstance().debug("WMO group ", groupIndex, " loaded: ",
                                      group.vertices.size(), " vertices, ",
                                      group.indices.size(), " indices, ",
                                      group.batches.size(), " batches");
    return !group.vertices.empty() && !group.indices.empty();
}

} // namespace pipeline
} // namespace wowee
