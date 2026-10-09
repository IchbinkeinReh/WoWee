#pragma once

/// The placement records ADT and WDT files share, as they lie on disk.
///
/// MDDF places an M2 and MODF a WMO. A WMO-only map's WDT carries one MODF
/// record of the same layout as an ADT's. Read with memcpy, never through a
/// cast pointer: the records sit at whatever offset the chunk puts them.

#include <cstdint>

namespace wowee::pipeline {

/// MDDF entry: one M2 placement.
struct MddfEntryDisk {
    uint32_t nameId;            ///< index into MMDX
    uint32_t uniqueId;
    float position[3];
    float rotation[3];          ///< degrees
    uint16_t scale;             ///< 1024 = 1.0
    uint16_t flags;
};
static_assert(sizeof(MddfEntryDisk) == 36,
              "MddfEntryDisk is read straight from the file: 36 bytes, no padding");

/// MODF entry: one WMO placement.
struct ModfEntryDisk {
    uint32_t nameId;            ///< index into MWMO
    uint32_t uniqueId;
    float position[3];
    float rotation[3];          ///< degrees
    float extentLower[3];
    float extentUpper[3];
    uint16_t flags;
    uint16_t doodadSet;
    uint16_t nameSet;           ///< WotLK; padding in older expansions
    uint16_t scale;             ///< WotLK, 1024 = 1.0; padding in older expansions
};
static_assert(sizeof(ModfEntryDisk) == 64,
              "ModfEntryDisk is read straight from the file: 64 bytes, no padding");

}  // namespace wowee::pipeline
