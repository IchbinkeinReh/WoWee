#include "pipeline/adt_loader.hpp"
#include "pipeline/map_placement_disk.hpp"

#include <span>
#include "core/logger.hpp"
#include "core/profiler.hpp"
#include "core/frame_profiler.hpp"
#include <cstddef>
#include <cstring>
#include <string>
#include <type_traits>
#include <cmath>
#include <algorithm>
#include <limits>

namespace wowee {
namespace pipeline {

// MCVT height grid: 9 outer + 8 inner vertices per row, 9 rows = 145 total.
// Each row is 17 entries: 9 outer corner vertices then 8 inner midpoints.
static constexpr int kMCVTVertexCount = 145;
static constexpr int kMCVTRowStride   = 17;  // 9 outer + 8 inner per row

namespace {

// Sizes as uint32, so the bounds checks they take part in add up in the same
// width as the file's own uint32 offsets.
constexpr uint32_t kChunkHeaderSize = 8;                     // magic + size
constexpr uint32_t kMcvtBytes = kMCVTVertexCount * sizeof(float);
constexpr uint32_t kMcnrBytes = kMCVTVertexCount * 3 + 13;   // 3 signed bytes a normal, 13 padding
constexpr uint32_t kMcshBytes = 64 * 64 / 8;                 // one bit a texel
constexpr uint32_t kMccvBytes = kMCVTVertexCount * 4;        // BGRA a vertex

// MCNK header (128 bytes). The ofs* fields locate the sub-chunks.
struct McnkHeaderDisk {
    uint32_t flags;               // 0x00
    uint32_t indexX;              // 0x04
    uint32_t indexY;              // 0x08
    uint32_t nLayers;             // 0x0C
    uint32_t nDoodadRefs;         // 0x10
    uint32_t ofsHeight;           // 0x14 - MCVT
    uint32_t ofsNormal;           // 0x18 - MCNR
    uint32_t ofsLayer;            // 0x1C - MCLY
    uint32_t ofsRefs;             // 0x20 - MCRF
    uint32_t ofsAlpha;            // 0x24 - MCAL
    uint32_t sizeAlpha;           // 0x28
    uint32_t ofsShadow;           // 0x2C - MCSH
    uint32_t sizeShadow;          // 0x30
    uint32_t areaId;              // 0x34
    uint32_t nMapObjRefs;         // 0x38
    uint16_t holes;               // 0x3C - a bit per 2x2 block of quads
    uint16_t unknown;             // 0x3E
    uint8_t doodadMapping[16];    // 0x40 - two bits per quad: its ground-effect layer
    uint32_t noEffectDoodad[2];   // 0x50 - one bit per quad, low half then high
    uint32_t ofsSoundEmitters;    // 0x58
    uint32_t nSoundEmitters;      // 0x5C
    uint32_t ofsLiquid;           // 0x60 - MCLQ
    uint32_t sizeLiquid;          // 0x64
    float position[3];            // 0x68 - wowY, wowX, wowZ (the height base)
    uint32_t ofsMCCV;             // 0x74
    uint32_t ofsMCLV;             // 0x78
    uint32_t unused;              // 0x7C
};
static_assert(sizeof(McnkHeaderDisk) == 128,
              "McnkHeaderDisk is read straight from the file: 128 bytes, no padding");
static_assert(offsetof(McnkHeaderDisk, holes) == 0x3C &&
              offsetof(McnkHeaderDisk, doodadMapping) == 0x40 &&
              offsetof(McnkHeaderDisk, noEffectDoodad) == 0x50 &&
              offsetof(McnkHeaderDisk, ofsLiquid) == 0x60 &&
              offsetof(McnkHeaderDisk, position) == 0x68 &&
              offsetof(McnkHeaderDisk, ofsMCCV) == 0x74,
              "McnkHeaderDisk fields must sit where the client reads them");

// MCLY entry: one texture layer.
struct MclyEntryDisk {
    uint32_t textureId;         // index into MTEX
    uint32_t flags;
    uint32_t offsetMCAL;        // where its alpha map starts in MCAL
    uint32_t effectId;          // GroundEffectTexture
};
static_assert(sizeof(MclyEntryDisk) == 16,
              "MclyEntryDisk is read straight from the file: 16 bytes, no padding");
constexpr uint32_t kMclyEntrySize = sizeof(MclyEntryDisk);

// MCLQ (vanilla/TBC liquid inside MCNK). Each vertex is four bytes whose
// meaning depends on the liquid, then its height.
struct MclqWaterVertexDisk {
    uint8_t depth;
    uint8_t flow0;
    uint8_t flow1;
    uint8_t filler;
    float height;
};
struct MclqMagmaVertexDisk {
    uint16_t s;
    uint16_t t;
    float height;
};
static_assert(sizeof(MclqWaterVertexDisk) == 8 && sizeof(MclqMagmaVertexDisk) == 8,
              "an MCLQ vertex is 8 bytes either way");
struct MclqDisk {
    float minHeight;
    float maxHeight;
    uint8_t vertices[9 * 9][8];  // MclqWaterVertexDisk or MclqMagmaVertexDisk
    uint8_t tiles[8 * 8];        // low nibble the liquid, 0x0F none; 0x80 hidden
};
static_assert(sizeof(MclqDisk) == 720,
              "MclqDisk is read straight from the file: 720 bytes, no padding");

// MH2O: 256 of these, one per map chunk. Offsets are from the start of the
// MH2O chunk's data.
struct Mh2oChunkHeaderDisk {
    uint32_t offsetInstances;   // -> Mh2oInstanceDisk[layerCount]
    uint32_t layerCount;
    uint32_t offsetAttributes;  // not read
};
static_assert(sizeof(Mh2oChunkHeaderDisk) == 12,
              "Mh2oChunkHeaderDisk is read straight from the file: 12 bytes, no padding");

// SMLiquidInstance: one liquid layer of a chunk.
struct Mh2oInstanceDisk {
    uint16_t liquidType;        // LiquidType.dbc
    uint16_t liquidObject;      // LVF: the vertex format
    float minHeight;
    float maxHeight;
    uint8_t x;                  // the layer's sub-rectangle of the 8x8 tiles
    uint8_t y;
    uint8_t width;
    uint8_t height;
    uint32_t offsetExistsBitmap;
    uint32_t offsetVertexData;
};
static_assert(sizeof(Mh2oInstanceDisk) == 24,
              "Mh2oInstanceDisk is read straight from the file: 24 bytes, no padding");

// A disk struct copied out of the buffer; zeroed when it would run past the
// end, as readUInt32 and its kin read past the end as zero.
template <typename T>
T readDisk(std::span<const uint8_t> data, size_t offset) {
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    if (offset + sizeof(T) <= data.size()) {
        std::memcpy(&value, data.data() + offset, sizeof(T));
    }
    return value;
}

// The NUL-terminated names MTEX, MMDX and MWMO hold, up to the first empty
// one. Bounded by the chunk: a last name without its terminator ends there
// rather than reading on into the next chunk.
void readNameList(std::span<const uint8_t> data, std::vector<std::string>& names) {
    size_t offset = 0;
    while (offset < data.size()) {
        const char* name = reinterpret_cast<const char*>(data.data() + offset);
        const size_t nameLen = strnlen(name, data.size() - offset);
        if (nameLen == 0) break;
        names.emplace_back(name, nameLen);
        offset += nameLen + 1;  // +1 for the terminator
    }
}

}  // namespace

// HeightMap implementation
float HeightMap::getHeight(int x, int y) const {
    if (x < 0 || x > 8 || y < 0 || y > 8) {
        return 0.0f;
    }

    // Outer vertex (x, y) in the interleaved grid
    int index = y * kMCVTRowStride + x;
    if (index < 0 || index >= kMCVTVertexCount) return 0.0f;

    return heights[index];
}

// ADTLoader implementation
ADTTerrain ADTLoader::load(const std::vector<uint8_t>& adtData) {
    ZoneScopedN("ADTLoader::load");
    // On the streaming workers, off the frame's path; here so a hitch that
    // lines up with a tile arriving can be told from one that does not.
    WOWEE_PROFILE_SCOPE("terrain: parse ADT", Background);
    ADTTerrain terrain;

    if (adtData.empty()) {
        LOG_ERROR("Empty ADT data");
        return terrain;
    }

    LOG_DEBUG("Loading ADT terrain (", adtData.size(), " bytes)");

    size_t offset = 0;
    int chunkIndex = 0;

    // Parse chunks
    while (offset < adtData.size()) {
        ChunkHeader header;
        if (!readChunkHeader(adtData, offset, header)) {
            break;
        }

        const size_t chunkSize = header.size;
        const std::span<const uint8_t> chunkData =
            std::span<const uint8_t>(adtData).subspan(offset + kChunkHeaderSize, chunkSize);

        // Parse based on chunk type
        if (header.magic == MVER) {
            parseMVER(chunkData, terrain);
        }
        else if (header.magic == MTEX) {
            parseMTEX(chunkData, terrain);
        }
        else if (header.magic == MMDX) {
            parseMMDX(chunkData, terrain);
        }
        else if (header.magic == MWMO) {
            parseMWMO(chunkData, terrain);
        }
        else if (header.magic == MDDF) {
            parseMDDF(chunkData, terrain);
        }
        else if (header.magic == MODF) {
            parseMODF(chunkData, terrain);
        }
        else if (header.magic == MH2O) {
            LOG_DEBUG("Found MH2O chunk (", chunkSize, " bytes)");
            parseMH2O(chunkData, terrain);
        }
        else if (header.magic == MCNK) {
            parseMCNK(chunkData, chunkIndex++, terrain);
        }

        // Move to next chunk
        offset += kChunkHeaderSize + chunkSize;
    }

    terrain.loaded = true;

    return terrain;
}

bool ADTLoader::readChunkHeader(std::span<const uint8_t> data, size_t offset, ChunkHeader& header) {
    static_assert(sizeof(ChunkHeader) == kChunkHeaderSize,
                  "ChunkHeader is read straight from the file: magic and size");
    if (offset + sizeof(ChunkHeader) > data.size()) {
        return false;
    }

    header = readDisk<ChunkHeader>(data, offset);

    // Validate chunk size
    if (offset + sizeof(ChunkHeader) + header.size > data.size()) {
        LOG_WARNING("Chunk extends beyond file: magic=0x", std::hex, header.magic,
                    ", size=", std::dec, header.size);
        return false;
    }

    return true;
}

uint32_t ADTLoader::readUInt32(std::span<const uint8_t> data, size_t offset) {
    if (offset + sizeof(uint32_t) > data.size()) {
        return uint32_t{};
    }
    uint32_t value;
    std::memcpy(&value, data.data() + offset, sizeof(uint32_t));
    return value;
}

float ADTLoader::readFloat(std::span<const uint8_t> data, size_t offset) {
    if (offset + sizeof(float) > data.size()) {
        return float{};
    }
    float value;
    std::memcpy(&value, data.data() + offset, sizeof(float));
    return value;
}

uint16_t ADTLoader::readUInt16(std::span<const uint8_t> data, size_t offset) {
    if (offset + sizeof(uint16_t) > data.size()) {
        return uint16_t{};
    }
    uint16_t value;
    std::memcpy(&value, data.data() + offset, sizeof(uint16_t));
    return value;
}

void ADTLoader::parseMVER(std::span<const uint8_t> data, ADTTerrain& terrain) {
    if (data.size() < 4) {
        LOG_WARNING("MVER chunk too small");
        return;
    }

    terrain.version = readUInt32(data, 0);
    LOG_DEBUG("ADT version: ", terrain.version);
}

void ADTLoader::parseMTEX(std::span<const uint8_t> data, ADTTerrain& terrain) {
    // MTEX contains null-terminated texture filenames.
    readNameList(data, terrain.textures);
    LOG_DEBUG("Loaded ", terrain.textures.size(), " texture names");
}

void ADTLoader::parseMMDX(std::span<const uint8_t> data, ADTTerrain& terrain) {
    // MMDX contains null-terminated M2 model filenames
    readNameList(data, terrain.doodadNames);
    LOG_DEBUG("Loaded ", terrain.doodadNames.size(), " doodad names");
}

void ADTLoader::parseMWMO(std::span<const uint8_t> data, ADTTerrain& terrain) {
    // MWMO contains null-terminated WMO filenames
    readNameList(data, terrain.wmoNames);
    LOG_DEBUG("Loaded ", terrain.wmoNames.size(), " WMO names from MWMO chunk");
}

void ADTLoader::parseMDDF(std::span<const uint8_t> data, ADTTerrain& terrain) {
    // MDDF contains doodad placements (36 bytes each)
    const size_t count = data.size() / sizeof(MddfEntryDisk);

    for (size_t i = 0; i < count; i++) {
        const auto entry = readDisk<MddfEntryDisk>(data, i * sizeof(MddfEntryDisk));

        ADTTerrain::DoodadPlacement placement;
        placement.nameId = entry.nameId;
        placement.uniqueId = entry.uniqueId;
        std::copy_n(entry.position, 3, placement.position);
        std::copy_n(entry.rotation, 3, placement.rotation);
        placement.scale = entry.scale;
        placement.flags = entry.flags;
        // Sanitize NaN/inf - corrupted MDDF entries would propagate bad
        // floats into the WMO/M2 instance transform and crash render.
        for (int k = 0; k < 3; k++) {
            if (!std::isfinite(placement.position[k])) placement.position[k] = 0.0f;
            if (!std::isfinite(placement.rotation[k])) placement.rotation[k] = 0.0f;
        }

        terrain.doodadPlacements.push_back(placement);
    }

    LOG_DEBUG("Loaded ", terrain.doodadPlacements.size(), " doodad placements");
}

void ADTLoader::parseMODF(std::span<const uint8_t> data, ADTTerrain& terrain) {
    // MODF contains WMO placements (64 bytes each)
    const size_t count = data.size() / sizeof(ModfEntryDisk);

    for (size_t i = 0; i < count; i++) {
        const auto entry = readDisk<ModfEntryDisk>(data, i * sizeof(ModfEntryDisk));

        ADTTerrain::WMOPlacement placement;
        placement.nameId = entry.nameId;
        placement.uniqueId = entry.uniqueId;
        std::copy_n(entry.position, 3, placement.position);
        std::copy_n(entry.rotation, 3, placement.rotation);
        std::copy_n(entry.extentLower, 3, placement.extentLower);
        std::copy_n(entry.extentUpper, 3, placement.extentUpper);
        placement.flags = entry.flags;
        placement.doodadSet = entry.doodadSet;
        // WotLK MODF entries include trailing nameSet + scale (4 bytes); older
        // expansions left them as padding, which reads as no scale.
        placement.nameSet = entry.nameSet;
        placement.scale = entry.scale;
        if (placement.scale == 0) placement.scale = 1024;
        // Same NaN scrub as MDDF entries - corrupted MODF would crash WMO
        // instance transform.
        for (int k = 0; k < 3; k++) {
            if (!std::isfinite(placement.position[k])) placement.position[k] = 0.0f;
            if (!std::isfinite(placement.rotation[k])) placement.rotation[k] = 0.0f;
            if (!std::isfinite(placement.extentLower[k])) placement.extentLower[k] = 0.0f;
            if (!std::isfinite(placement.extentUpper[k])) placement.extentUpper[k] = 0.0f;
        }

        terrain.wmoPlacements.push_back(placement);
    }

    LOG_DEBUG("Loaded ", terrain.wmoPlacements.size(), " WMO placements");
}

void ADTLoader::parseMCNK(std::span<const uint8_t> data, int chunkIndex, ADTTerrain& terrain) {
    if (chunkIndex < 0 || chunkIndex >= 256) {
        LOG_WARNING("Invalid chunk index: ", chunkIndex);
        return;
    }

    MapChunk& chunk = terrain.chunks[chunkIndex];

    // Read MCNK header (128 bytes)
    if (data.size() < sizeof(McnkHeaderDisk)) {
        LOG_WARNING("MCNK chunk too small");
        return;
    }
    const auto mcnk = readDisk<McnkHeaderDisk>(data, 0);

    chunk.flags = mcnk.flags;
    chunk.indexX = mcnk.indexX;
    chunk.indexY = mcnk.indexY;
    chunk.areaId = mcnk.areaId;

    // Holes mask: each bit represents a 2x2 block of the 8x8 quad grid
    chunk.holes = mcnk.holes;

    // doodadMapping: two bits per quad of the 8x8 grid,
    // naming which of the four texture layers the quad takes its ground
    // effect from. Paint blends per texel but growth follows the quad's
    // dominant layer, so a few faint grassy texels bleeding over dirt do not
    // seed the dirt.
    static_assert(sizeof(mcnk.doodadMapping) == std::tuple_size_v<decltype(chunk.doodadMapping)>,
                  "MapChunk::doodadMapping holds MCNK's mapping as it is");
    std::copy_n(mcnk.doodadMapping, sizeof(mcnk.doodadMapping), chunk.doodadMapping.begin());

    // noEffectDoodad: one bit per quad of the 8x8 grid,
    // set where the map forbids ground effect doodads. Tilled farm rows,
    // building footprints and WMO interior floors are painted with textures
    // whose effects otherwise grow, and this mask is the only thing in the
    // data that says nothing should.
    chunk.noEffectDoodad = static_cast<uint64_t>(mcnk.noEffectDoodad[0]) |
                           (static_cast<uint64_t>(mcnk.noEffectDoodad[1]) << 32);

    // Read layer count and offsets from MCNK header
    const uint32_t nLayers = mcnk.nLayers;
    const uint32_t ofsHeight = mcnk.ofsHeight;   // MCVT offset
    const uint32_t ofsNormal = mcnk.ofsNormal;   // MCNR offset
    const uint32_t ofsLayer = mcnk.ofsLayer;     // MCLY offset
    const uint32_t ofsAlpha = mcnk.ofsAlpha;     // MCAL offset
    const uint32_t sizeAlpha = mcnk.sizeAlpha;

    // Debug first chunk only
    if (chunkIndex == 0) {
        LOG_DEBUG("MCNK[0] offsets: nLayers=", nLayers,
                 " height=", ofsHeight, " normal=", ofsNormal,
                 " layer=", ofsLayer, " alpha=", ofsAlpha,
                 " sizeAlpha=", sizeAlpha, " size=", data.size(),
                 " holes=0x", std::hex, chunk.holes, std::dec);
    }

    // MCNK position is in canonical WoW coordinates (NOT ADT placement space):
    //   [0] wowY (west axis, horizontal - unused, XY computed from tile indices)
    //   [1] wowX (north axis, horizontal - unused, XY computed from tile indices)
    //   [2] wowZ = HEIGHT BASE (MCVT heights are relative to this)
    std::copy_n(mcnk.position, 3, chunk.position);


    // Parse sub-chunks using offsets from MCNK header
    // WoW ADT sub-chunks may have their own 8-byte headers (magic+size)
    // Check by inspecting the first 4 bytes at the offset

    // Height map (MCVT) - 145 floats = 580 bytes.
    // Guard must include the potential 8-byte sub-chunk header, otherwise the
    // parser reads up to 8 bytes past the validated range.
    if (ofsHeight > 0 && ofsHeight + kMcvtBytes + kChunkHeaderSize <= data.size()) {
        uint32_t possibleMagic = readUInt32(data, ofsHeight);
        uint32_t headerSkip = 0;
        if (possibleMagic == MCVT) {
            headerSkip = kChunkHeaderSize;
            if (chunkIndex == 0) {
                LOG_DEBUG("MCNK sub-chunks have headers (MCVT magic found at offset ", ofsHeight, ")");
            }
        }
        parseMCVT(data.subspan(ofsHeight + headerSkip, kMcvtBytes), chunk);
    }

    // Normals (MCNR) - 145 normals (3 bytes each) + 13 padding = 448 bytes.
    if (ofsNormal > 0 && ofsNormal + kMcnrBytes + kChunkHeaderSize <= data.size()) {
        uint32_t possibleMagic = readUInt32(data, ofsNormal);
        uint32_t skip = (possibleMagic == MCNR) ? kChunkHeaderSize : 0;
        parseMCNR(data.subspan(ofsNormal + skip, kMcnrBytes), chunk);
    }

    // Texture layers (MCLY) - 16 bytes per layer
    if (ofsLayer > 0 && nLayers > 0) {
        size_t layerSize = nLayers * kMclyEntrySize;
        uint32_t possibleMagic = readUInt32(data, ofsLayer);
        uint32_t skip = (possibleMagic == MCLY) ? kChunkHeaderSize : 0;
        if (ofsLayer + skip + layerSize <= data.size()) {
            parseMCLY(data.subspan(ofsLayer + skip, layerSize), chunk);
        }
    }

    // Alpha maps (MCAL) - variable size from header.
    // sizeAlpha is only known to be >= 1 here, so a four-byte chunk whose
    // contents happen to spell MCAL would take skip to 8 and wrap
    // sizeAlpha - skip to ~1.8e19. Drop the sub-chunk rather than hand a
    // parser a length longer than the address space.
    if (ofsAlpha > 0 && sizeAlpha > 0 && ofsAlpha + sizeAlpha <= data.size()) {
        uint32_t possibleMagic = readUInt32(data, ofsAlpha);
        uint32_t skip = (possibleMagic == MCAL) ? kChunkHeaderSize : 0;
        if (sizeAlpha > skip) {
            parseMCAL(data.subspan(ofsAlpha + skip, sizeAlpha - skip), chunk);
        }
    }

    // Baked shadow (MCSH): MCNK offsets 0x2C and 0x30, present with flag 0x1.
    // With its own sub-chunk header, the size is the header's.
    const uint32_t ofsShadow = mcnk.ofsShadow;
    const uint32_t sizeShadow = mcnk.sizeShadow;
    if ((chunk.flags & 0x1) && ofsShadow > 0 && ofsShadow + kChunkHeaderSize <= data.size()) {
        const ChunkHeader shadowHeader = readDisk<ChunkHeader>(data, ofsShadow);
        const uint32_t skip = (shadowHeader.magic == MCSH) ? kChunkHeaderSize : 0;
        const uint32_t size = skip ? shadowHeader.size : sizeShadow;
        if (size >= kMcshBytes && ofsShadow + skip + kMcshBytes <= data.size()) {
            parseMCSH(data.subspan(ofsShadow + skip, kMcshBytes), (chunk.flags & 0x8000) == 0, chunk);
        }
    }

    // Vertex shading (MCCV): MCNK offset 0x74, 145 BGRA colours.
    const uint32_t ofsMCCV = mcnk.ofsMCCV;
    if (ofsMCCV > 0 && ofsMCCV + kChunkHeaderSize + kMccvBytes <= data.size()) {
        const uint32_t possibleMagic = readUInt32(data, ofsMCCV);
        const uint32_t skip = (possibleMagic == MCCV) ? kChunkHeaderSize : 0;
        std::copy_n(data.begin() + ofsMCCV + skip, kMccvBytes, chunk.vertexShading.begin());
        chunk.hasVertexShading = true;
    }

    // Liquid (MCLQ) - vanilla/TBC per-chunk water (no MH2O in these expansions)
    // ofsLiquid at MCNK header offset 0x60, sizeLiquid at 0x64
    uint32_t ofsLiquid = mcnk.ofsLiquid;
    uint32_t sizeLiquid = mcnk.sizeLiquid;
    if (ofsLiquid > 0 && sizeLiquid > 8 && ofsLiquid + sizeLiquid <= data.size()) {
        uint32_t possibleMagic = readUInt32(data, ofsLiquid);
        uint32_t skip = (possibleMagic == MCLQ) ? 8 : 0;
        if (sizeLiquid > skip) {
            parseMCLQ(data.subspan(ofsLiquid + skip, sizeLiquid - skip),
                      chunkIndex, chunk.flags, terrain);
        }
    }
}

void ADTLoader::parseMCSH(std::span<const uint8_t> data, bool fixEdges, MapChunk& chunk) {
    if (data.size() < 512) return;
    chunk.shadowMap.assign(64 * 64, 0);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const uint8_t byte = data[static_cast<size_t>(y * 8 + x / 8)];
            chunk.shadowMap[static_cast<size_t>(y * 64 + x)] = (byte >> (x % 8)) & 1u;
        }
    }
    if (fixEdges) {
        for (int i = 0; i < 64; ++i) {
            chunk.shadowMap[static_cast<size_t>(i * 64 + 63)] = chunk.shadowMap[static_cast<size_t>(i * 64 + 62)];
            chunk.shadowMap[static_cast<size_t>(63 * 64 + i)] = chunk.shadowMap[static_cast<size_t>(62 * 64 + i)];
        }
    }
}

void ADTLoader::parseMCVT(std::span<const uint8_t> data, MapChunk& chunk) {
    if (data.size() < kMCVTVertexCount * sizeof(float)) {
        LOG_WARNING("MCVT chunk too small: ", data.size(), " bytes");
        return;
    }

    float minHeight = std::numeric_limits<float>::max();
    float maxHeight = std::numeric_limits<float>::lowest();

    for (int i = 0; i < kMCVTVertexCount; i++) {
        float height = readFloat(data, i * sizeof(float));
        chunk.heightMap.heights[i] = height;

        if (height < minHeight) minHeight = height;
        if (height > maxHeight) maxHeight = height;
    }
    chunk.heightMap.loaded = true;

    // Log height range for first chunk only
    static bool logged = false;
    if (!logged) {
        LOG_INFO("MCVT height range: [", minHeight, ", ", maxHeight, "]",
                 " (heights[0]=", chunk.heightMap.heights[0],
                 " heights[8]=", chunk.heightMap.heights[8],
                 " heights[136]=", chunk.heightMap.heights[136],
                 " heights[144]=", chunk.heightMap.heights[144], ")");
        logged = true;
    }
}

void ADTLoader::parseMCNR(std::span<const uint8_t> data, MapChunk& chunk) {
    // MCNR: one signed XYZ normal per vertex (3 bytes each)
    if (data.size() < kMCVTVertexCount * 3) {
        LOG_WARNING("MCNR chunk too small: ", data.size(), " bytes");
        return;
    }

    for (int i = 0; i < kMCVTVertexCount * 3; i++) {
        chunk.normals[i] = static_cast<int8_t>(data[i]);
    }
}

void ADTLoader::parseMCLY(std::span<const uint8_t> data, MapChunk& chunk) {
    // MCLY contains texture layer definitions (16 bytes each)
    size_t layerCount = data.size() / sizeof(MclyEntryDisk);

    if (layerCount > 4) {
        LOG_WARNING("More than 4 texture layers: ", layerCount);
        layerCount = 4;
    }

    static int layerLogCount = 0;
    for (size_t i = 0; i < layerCount; i++) {
        const auto entry = readDisk<MclyEntryDisk>(data, i * sizeof(MclyEntryDisk));
        TextureLayer layer;

        layer.textureId = entry.textureId;
        layer.flags = entry.flags;
        layer.offsetMCAL = entry.offsetMCAL;
        layer.effectId = entry.effectId;

        if (layerLogCount < 10) {
            LOG_DEBUG("  MCLY[", i, "]: texId=", layer.textureId,
                     " flags=0x", std::hex, layer.flags, std::dec,
                     " alphaOfs=", layer.offsetMCAL,
                     " useAlpha=", layer.useAlpha(),
                     " compressed=", layer.compressedAlpha());
            layerLogCount++;
        }

        chunk.layers.push_back(layer);
    }
}

void ADTLoader::parseMCAL(std::span<const uint8_t> data, MapChunk& chunk) {
    // MCAL contains alpha maps for texture layers
    // Store raw data; decompression happens per-layer during mesh generation
    chunk.alphaMap.resize(data.size());
    std::memcpy(chunk.alphaMap.data(), data.data(), data.size());
}

void ADTLoader::parseMCLQ(std::span<const uint8_t> data, int chunkIndex,
                          uint32_t mcnkFlags, ADTTerrain& terrain) {
    // MCLQ: Vanilla/TBC per-chunk liquid data (inside MCNK). See MclqDisk:
    //   float minHeight, maxHeight  (8 bytes)
    //   SLiquidVertex[9*9]          (81 * 8 = 648 bytes)
    //     water: uint8 depth, flow0, flow1, filler, float height
    //     magma: uint16 s, uint16 t, float height
    //   uint8 tiles[8*8]            (64 bytes)
    // Total minimum: 720 bytes

    if (data.size() < sizeof(MclqDisk)) {
        return;  // Not enough data for a valid MCLQ
    }
    const auto mclq = readDisk<MclqDisk>(data, 0);

    float minHeight = mclq.minHeight;
    float maxHeight = mclq.maxHeight;

    // Determine liquid type from MCNK flags
    // 0x04 = has liquid (river/lake), 0x08 = ocean, 0x10 = magma, 0x20 = slime
    uint16_t liquidType = 0;  // water
    if (mcnkFlags & 0x08) liquidType = 1;       // ocean
    else if (mcnkFlags & 0x10) liquidType = 2;  // magma
    else if (mcnkFlags & 0x20) liquidType = 3;  // slime

    // Read 9x9 vertices: the float height at +4, and ahead of it a water's
    // depth byte or a magma's two texture coordinates.
    const bool magmaVerts = liquidType >= 2;
    std::vector<float> heights(81);
    std::vector<uint8_t> depths;
    std::vector<uint16_t> uvs;
    if (magmaVerts) uvs.resize(162); else depths.resize(81);
    for (int i = 0; i < 81; i++) {
        if (magmaVerts) {
            MclqMagmaVertexDisk v;
            std::memcpy(&v, mclq.vertices[i], sizeof(v));
            heights[i] = v.height;
            uvs[i * 2] = v.s;
            uvs[i * 2 + 1] = v.t;
        } else {
            MclqWaterVertexDisk v;
            std::memcpy(&v, mclq.vertices[i], sizeof(v));
            heights[i] = v.height;
            depths[i] = v.depth;
        }
    }

    // Read 8x8 tile flags
    std::vector<uint8_t> tileMask(64);
    bool anyVisible = false;
    for (int i = 0; i < 64; i++) {
        uint8_t tileFlag = mclq.tiles[i];
        // The low nibble stores the liquid type; 0x0F is the MCLQ sentinel
        // for a tile with no liquid.  Some files also mark dry tiles with
        // the legacy 0x80 hidden bit.
        bool hidden = (tileFlag & 0x0F) == 0x0F || (tileFlag & 0x80) != 0;
        tileMask[i] = hidden ? 0 : 1;
        if (!hidden) anyVisible = true;
    }

    if (!anyVisible) {
        return;  // All tiles hidden, no visible water
    }

    // Validate heights - if all heights are 0 or unreasonable, skip
    bool validHeights = false;
    for (float h : heights) {
        if (h != 0.0f && std::isfinite(h)) {
            validHeights = true;
            break;
        }
    }
    // If heights are all zero, use maxHeight as flat water level
    if (!validHeights) {
        for (float& h : heights) h = maxHeight;
    }

    // Build a WaterLayer matching the MH2O format
    ADTTerrain::WaterLayer layer;
    layer.liquidType = liquidType;
    layer.flags = 0;
    layer.minHeight = minHeight;
    layer.maxHeight = maxHeight;
    layer.x = 0;
    layer.y = 0;
    layer.width = 8;   // 8 tiles = 9 vertices per axis
    layer.height = 8;
    layer.heights = std::move(heights);
    layer.vertexFormat = magmaVerts ? 1 : 0;
    layer.fromMCLQ = true;
    layer.depths = std::move(depths);
    layer.uvs = std::move(uvs);
    layer.mask.resize(8);  // 8 bytes = 64 bits for 8x8 tiles
    for (int row = 0; row < 8; row++) {
        uint8_t rowBits = 0;
        for (int col = 0; col < 8; col++) {
            if (tileMask[row * 8 + col]) {
                rowBits |= (1 << col);
            }
        }
        layer.mask[row] = rowBits;
    }

    terrain.waterData[chunkIndex].layers.push_back(std::move(layer));

    static int mclqLogCount = 0;
    if (mclqLogCount < 5) {
        LOG_INFO("MCLQ[", chunkIndex, "]: type=", liquidType,
                 " height=[", minHeight, ",", maxHeight, "]");
        mclqLogCount++;
    }
}

void ADTLoader::parseMH2O(std::span<const uint8_t> data, ADTTerrain& terrain) {
    // MH2O contains water/liquid data for all 256 map chunks
    // Structure: 256 SMLiquidChunk headers followed by instance data

    // Each SMLiquidChunk header is 12 bytes (WotLK 3.3.5a), Mh2oChunkHeaderDisk.

    const size_t totalHeaderSize = 256 * sizeof(Mh2oChunkHeaderDisk);

    if (data.size() < totalHeaderSize) {
        LOG_WARNING("MH2O chunk too small for headers: ", data.size(), " bytes");
        return;
    }

    int totalLayers = 0;

    for (int chunkIdx = 0; chunkIdx < 256; chunkIdx++) {
        const auto chunkHeader =
            readDisk<Mh2oChunkHeaderDisk>(data, chunkIdx * sizeof(Mh2oChunkHeaderDisk));
        uint32_t offsetInstances = chunkHeader.offsetInstances;
        uint32_t layerCount = chunkHeader.layerCount;

        if (layerCount == 0 || offsetInstances == 0) {
            continue;  // No water in this chunk
        }

        // Sanity checks
        if (offsetInstances >= data.size()) {
            continue;
        }
        if (layerCount > 16) {
            // Sanity check - max 16 layers per chunk is reasonable
            LOG_WARNING("MH2O: Invalid layer count ", layerCount, " for chunk ", chunkIdx);
            continue;
        }

        // Parse each liquid layer (SMLiquidInstance - 24 bytes)
        for (uint32_t layerIdx = 0; layerIdx < layerCount; layerIdx++) {
            size_t instanceOffset = offsetInstances + layerIdx * sizeof(Mh2oInstanceDisk);

            if (instanceOffset + sizeof(Mh2oInstanceDisk) > data.size()) {
                break;
            }
            const auto instance = readDisk<Mh2oInstanceDisk>(data, instanceOffset);

            ADTTerrain::WaterLayer layer;
            layer.liquidType = instance.liquidType;
            uint16_t liquidObject = instance.liquidObject;  // LVF format flags
            layer.minHeight = instance.minHeight;
            layer.maxHeight = instance.maxHeight;
            layer.x = instance.x;
            layer.y = instance.y;
            layer.width = instance.width;
            layer.height = instance.height;
            uint32_t offsetExistsBitmap = instance.offsetExistsBitmap;
            uint32_t offsetVertexData = instance.offsetVertexData;

            // Skip invalid layers
            if (layer.width == 0 || layer.height == 0) {
                continue;
            }
            // An origin outside the 8x8 chunk would wrap the uint8 width clamp
            // below and index past layer.mask; only corrupt files have one.
            if (layer.x >= 8 || layer.y >= 8) {
                continue;
            }

            // Clamp dimensions to valid range
            if (layer.width > 8) layer.width = 8;
            if (layer.height > 8) layer.height = 8;
            if (layer.x + layer.width > 8) layer.width = 8 - layer.x;
            if (layer.y + layer.height > 8) layer.height = 8 - layer.y;

            // Read exists bitmap (which tiles have water).
            // The bitmap holds one bit per tile of the layer's width×height
            // sub-rectangle - row-major, LSB-first, packed into
            // (width*height+7)/8 bytes. It is NOT a chunk-wide 8-byte block.
            // Normalize into a canonical chunk-wide 8x8 mask
            // (bit = (y+row)*8 + (x+col), LSB-first) so MCLQ and MH2O
            // masks share one format downstream.
            // Note: offsets in SMLiquidInstance are relative to MH2O chunk start
            layer.mask.assign(8, 0x00);
            bool anyTile = false;
            size_t packedBytes = (static_cast<size_t>(layer.width) * layer.height + 7) / 8;
            bool haveBitmap = offsetExistsBitmap > 0 &&
                              offsetExistsBitmap + packedBytes <= data.size();
            const std::span<const uint8_t> bits =
                haveBitmap ? data.subspan(offsetExistsBitmap, packedBytes) : std::span<const uint8_t>{};
            int bitPos = 0;
            for (int row = 0; row < layer.height; row++) {
                for (int col = 0; col < layer.width; col++, bitPos++) {
                    // No bitmap (or out-of-range offset) means every tile in
                    // the sub-rect exists.
                    bool exists = !haveBitmap || (bits[bitPos / 8] & (1 << (bitPos % 8))) != 0;
                    if (exists) {
                        int tileIdx = (layer.y + row) * 8 + (layer.x + col);
                        layer.mask[tileIdx / 8] |= static_cast<uint8_t>(1 << (tileIdx % 8));
                        anyTile = true;
                    }
                }
            }
            if (!anyTile) {
                continue;  // Bitmap masks out every tile - nothing to render
            }

            // Read vertex heights
            // Number of vertices is (width+1) * (height+1)
            size_t numVertices = (layer.width + 1) * (layer.height + 1);

            // Check liquid object flags (LVF) to determine vertex format
            bool hasHeightData = (liquidObject != 2);  // LVF_height_depth or LVF_height_texcoord
            layer.vertexFormat = liquidObject;

            // What follows the heights: LVF 0 a depth byte a vertex, 1 two
            // ushort texture coordinates, 2 depth alone, 3 coordinates then
            // depth. The client draws with both (0x007ce390).
            if (offsetVertexData > 0 && liquidObject <= 3) {
                size_t at = offsetVertexData + (hasHeightData ? numVertices * sizeof(float) : 0);
                if (liquidObject == 1 || liquidObject == 3) {
                    if (at + numVertices * 4 <= data.size()) {
                        layer.uvs.resize(numVertices * 2);
                        for (size_t i = 0; i < numVertices * 2; i++) {
                            layer.uvs[i] = readUInt16(data, at + i * 2);
                        }
                    }
                    at += numVertices * 4;
                }
                if (liquidObject != 1 && at + numVertices <= data.size()) {
                    layer.depths.assign(data.begin() + static_cast<std::ptrdiff_t>(at),
                                        data.begin() + static_cast<std::ptrdiff_t>(at + numVertices));
                }
            }

            if (hasHeightData && offsetVertexData > 0) {
                size_t vertexOffset = offsetVertexData;
                size_t vertexDataSize = numVertices * sizeof(float);

                if (vertexOffset + vertexDataSize <= data.size()) {
                    layer.heights.resize(numVertices);
                    for (size_t i = 0; i < numVertices; i++) {
                        layer.heights[i] = readFloat(data, vertexOffset + i * sizeof(float));
                    }
                } else {
                    // Offset out of bounds - use flat water
                    layer.heights.resize(numVertices, layer.minHeight);
                }
            } else {
                // No height data - use flat surface at minHeight
                layer.heights.resize(numVertices, layer.minHeight);
            }

            // Default flags
            layer.flags = 0;

            terrain.waterData[chunkIdx].layers.push_back(layer);
            totalLayers++;
        }
    }

    LOG_DEBUG("Loaded MH2O water data: ", totalLayers, " liquid layers across ", data.size(), " bytes");
}

} // namespace pipeline
} // namespace wowee
