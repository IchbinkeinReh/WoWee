#include "pipeline/wdt_loader.hpp"
#include "pipeline/map_placement_disk.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cstring>
#include <span>

namespace wowee {
namespace pipeline {

namespace {

// Chunk header: four-character magic, then the size of the data after it.
struct ChunkHeaderDisk {
    uint32_t magic;
    uint32_t size;
};
static_assert(sizeof(ChunkHeaderDisk) == 8,
              "ChunkHeaderDisk is read straight from the file: 8 bytes, no padding");

// A disk struct copied out of a span the caller has checked is long enough.
template <typename T>
T readDisk(std::span<const uint8_t> data) {
    T value;
    std::memcpy(&value, data.data(), sizeof(T));
    return value;
}

// Chunk magic constants (big-endian ASCII, same as ADTLoader)
constexpr uint32_t MVER = 0x4D564552; // "MVER"
constexpr uint32_t MPHD = 0x4D504844; // "MPHD"
constexpr uint32_t MWMO = 0x4D574D4F; // "MWMO"
constexpr uint32_t MODF = 0x4D4F4446; // "MODF"

} // anonymous namespace

WDTInfo parseWDT(const std::vector<uint8_t>& data) {
    WDTInfo info;

    if (data.size() < sizeof(ChunkHeaderDisk)) {
        LOG_WARNING("WDT data too small (", data.size(), " bytes)");
        return info;
    }

    const std::span<const uint8_t> file(data);
    size_t offset = 0;

    while (offset + sizeof(ChunkHeaderDisk) <= file.size()) {
        const auto header = readDisk<ChunkHeaderDisk>(file.subspan(offset));
        const uint32_t magic = header.magic;
        const uint32_t chunkSize = header.size;

        if (offset + sizeof(ChunkHeaderDisk) + chunkSize > file.size()) {
            LOG_WARNING("WDT chunk extends beyond file at offset ", offset);
            break;
        }

        const std::span<const uint8_t> chunkData =
            file.subspan(offset + sizeof(ChunkHeaderDisk), chunkSize);

        if (magic == MVER) {
            if (chunkSize >= sizeof(uint32_t)) {
                uint32_t version = readDisk<uint32_t>(chunkData);
                LOG_DEBUG("WDT version: ", version);
            }
        } else if (magic == MPHD) {
            if (chunkSize >= sizeof(uint32_t)) {
                info.mphdFlags = readDisk<uint32_t>(chunkData);  // the first of MPHD's fields
                LOG_DEBUG("WDT MPHD flags: 0x", std::hex, info.mphdFlags, std::dec);
            }
        } else if (magic == MWMO) {
            // Null-terminated WMO path string(s)
            if (chunkSize > 0) {
                const char* str = reinterpret_cast<const char*>(chunkData.data());
                // Bound scan to chunkSize to avoid OOB read on truncated files
                // (strlen has no upper bound if the data lacks a null terminator).
                size_t len = strnlen(str, chunkSize);
                if (len > 0) {
                    info.rootWMOPath = std::string(str, len);
                    LOG_DEBUG("WDT root WMO: ", info.rootWMOPath);
                }
            }
        } else if (magic == MODF) {
            // The one placement of a WMO-only map, laid out as an ADT's MODF
            if (chunkSize >= sizeof(ModfEntryDisk)) {
                const auto entry = readDisk<ModfEntryDisk>(chunkData);
                // nameId is unused for WDT - the path comes from MWMO
                std::copy_n(entry.position, 3, info.position);
                std::copy_n(entry.rotation, 3, info.rotation);
                info.flags = entry.flags;
                info.doodadSet = entry.doodadSet;
                LOG_DEBUG("WDT MODF placement: pos=(", info.position[0], ", ",
                         info.position[1], ", ", info.position[2], ") rot=(",
                         info.rotation[0], ", ", info.rotation[1], ", ",
                         info.rotation[2], ") doodadSet=", info.doodadSet);
            }
        }

        offset += sizeof(ChunkHeaderDisk) + chunkSize;
    }

    LOG_DEBUG("WDT parse result: mphdFlags=0x", std::hex, info.mphdFlags, std::dec,
             " isWMOOnly=", info.isWMOOnly(),
             " rootWMO='", info.rootWMOPath, "'");

    return info;
}

} // namespace pipeline
} // namespace wowee
