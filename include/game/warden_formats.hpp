#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

// Byte layouts the Warden code reads and writes: the PE headers of WoW.exe,
// the native Warden module image, the Warden packets and the check records
// inside them, and the .cr challenge/response cache.
//
// Every struct here is packed and memcpy'd to and from the bytes, never
// dereferenced in place: the bytes sit at whatever offset the packet or file
// put them, and an unaligned uint32_t* load is undefined behaviour even on x86.
// The static_asserts are the layout; a field that moves breaks the build rather
// than a login.

namespace wowee {
namespace game {

// Every one of these formats is little-endian on the wire and on disk, and the
// structs carry it in host order. A big-endian port would need a byteswap at
// each loadRecord, and this is where it would find out.
static_assert(std::endian::native == std::endian::little,
              "Warden layouts are memcpy'd straight from little-endian bytes");

/// A record copied out of a byte buffer. The caller has already checked that
/// sizeof(T) bytes are there; this only does the copy.
template <typename T>
[[nodiscard]] inline T loadRecord(const uint8_t* src) {
    static_assert(std::is_trivially_copyable_v<T>);
    T record;
    std::memcpy(&record, src, sizeof(T));
    return record;
}

/// The inverse of loadRecord, with the same contract.
template <typename T>
inline void storeRecord(uint8_t* dst, const T& record) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(dst, &record, sizeof(T));
}

#pragma pack(push, 1)

// ---------------------------------------------------------------------------
// PE/COFF (WoW.exe, read by WardenMemory)
// ---------------------------------------------------------------------------

/// IMAGE_DOS_HEADER. Only the magic and e_lfanew mean anything to a PE loader;
/// the rest is the real-mode stub's header.
struct PeDosHeader {
    uint16_t magic;              // e_magic, "MZ"
    uint8_t  realModeFields[58]; // e_cblp .. e_res2
    uint32_t peHeaderOffset;     // e_lfanew: file offset of the "PE\0\0" signature
};
static_assert(sizeof(PeDosHeader) == 64);
static_assert(offsetof(PeDosHeader, peHeaderOffset) == 0x3C);

constexpr uint16_t PE_DOS_MAGIC  = 0x5A4D;      // "MZ"
constexpr uint32_t PE_SIGNATURE  = 0x00004550;  // "PE\0\0"
constexpr uint16_t PE32_MAGIC    = 0x010B;      // IMAGE_NT_OPTIONAL_HDR32_MAGIC

/// IMAGE_FILE_HEADER, directly after the PE signature.
struct PeFileHeader {
    uint16_t machine;
    uint16_t numberOfSections;
    uint32_t timeDateStamp;
    uint32_t pointerToSymbolTable;
    uint32_t numberOfSymbols;
    uint16_t sizeOfOptionalHeader;
    uint16_t characteristics;
};
static_assert(sizeof(PeFileHeader) == 20);

/// IMAGE_OPTIONAL_HEADER32 up to, not including, the data directories.
///
/// Read field by field (see WardenMemory::parsePE) rather than whole: the file
/// header says how long this header is, and a short one is still a PE.
struct PeOptionalHeader32 {
    uint16_t magic;
    uint8_t  majorLinkerVersion;
    uint8_t  minorLinkerVersion;
    uint32_t sizeOfCode;
    uint32_t sizeOfInitializedData;
    uint32_t sizeOfUninitializedData;
    uint32_t addressOfEntryPoint;
    uint32_t baseOfCode;
    uint32_t baseOfData;
    uint32_t imageBase;
    uint32_t sectionAlignment;
    uint32_t fileAlignment;
    uint16_t majorOperatingSystemVersion;
    uint16_t minorOperatingSystemVersion;
    uint16_t majorImageVersion;
    uint16_t minorImageVersion;
    uint16_t majorSubsystemVersion;
    uint16_t minorSubsystemVersion;
    uint32_t win32VersionValue;
    uint32_t sizeOfImage;
    uint32_t sizeOfHeaders;
    uint32_t checkSum;
    uint16_t subsystem;
    uint16_t dllCharacteristics;
    uint32_t sizeOfStackReserve;
    uint32_t sizeOfStackCommit;
    uint32_t sizeOfHeapReserve;
    uint32_t sizeOfHeapCommit;
    uint32_t loaderFlags;
    uint32_t numberOfRvaAndSizes;
};
static_assert(sizeof(PeOptionalHeader32) == 96);
static_assert(offsetof(PeOptionalHeader32, imageBase) == 28);
static_assert(offsetof(PeOptionalHeader32, sizeOfImage) == 56);
static_assert(offsetof(PeOptionalHeader32, sizeOfHeaders) == 60);

/// IMAGE_SECTION_HEADER. The table follows the optional header.
struct PeSectionHeader {
    char     name[8];            // not NUL-terminated when all eight are used
    uint32_t virtualSize;
    uint32_t virtualAddress;     // RVA
    uint32_t sizeOfRawData;
    uint32_t pointerToRawData;   // file offset
    uint32_t pointerToRelocations;
    uint32_t pointerToLinenumbers;
    uint16_t numberOfRelocations;
    uint16_t numberOfLinenumbers;
    uint32_t characteristics;
};
static_assert(sizeof(PeSectionHeader) == 40);

// ---------------------------------------------------------------------------
// Native Warden module image (WardenModule::parseExecutableFormat)
// ---------------------------------------------------------------------------

/// The fixed header a native Warden image starts with. It is copied into the
/// mapped image along with everything else, and the table offsets below are
/// offsets into that image, not into the decompressed stream.
struct WardenNativeImageHeader {
    uint32_t imageSize;           // 0x00 bytes the mapped image occupies
    uint32_t unknown04;           // 0x04 not read
    uint32_t relocTableOffset;    // 0x08 relocation entries (see below)
    uint32_t relocCount;          // 0x0C
    uint32_t exportTableOffset;   // 0x10 uint32 image offsets, one per ordinal
    uint32_t exportCount;         // 0x14
    uint32_t exportBaseOrdinal;   // 0x18 ordinal of the first export slot
    uint32_t importTableOffset;   // 0x1C WardenNativeImportDescriptor[importCount]
    uint32_t importCount;         // 0x20
    uint32_t sectionCount;        // 0x24 descriptors directly after this header
};
static_assert(sizeof(WardenNativeImageHeader) == 0x28);

/// One section descriptor. These are metadata and are not copied into the
/// image; only the first one's offset is read, as where the copy stream starts.
struct WardenNativeSectionDescriptor {
    uint32_t imageOffset;
    uint32_t size;                // not read
    uint32_t protection;          // not read
};
static_assert(sizeof(WardenNativeSectionDescriptor) == 12);

/// One imported library: where its name is, and where its zero-terminated
/// thunk array is. Both are image offsets.
struct WardenNativeImportDescriptor {
    uint32_t libraryNameOffset;
    uint32_t thunkTableOffset;
};
static_assert(sizeof(WardenNativeImportDescriptor) == 8);

/// A thunk with this bit set imports by ordinal (the low 31 bits); otherwise it
/// is the image offset of the function's name.
constexpr uint32_t WARDEN_IMPORT_BY_ORDINAL_FLAG = 0x80000000u;
constexpr uint32_t WARDEN_IMPORT_ORDINAL_MASK    = 0x7FFFFFFFu;

// Native relocation entries are one of two forms, told apart by the top bit of
// the first byte. Clear: a two-byte big-endian delta added to the running
// offset. Set: a four-byte big-endian absolute offset, flag bit masked off
// (wardenAbsoluteRelocTarget). Either way the dword at the resulting offset
// gets the image base added.
constexpr uint8_t WARDEN_RELOC_ABSOLUTE_FLAG       = 0x80;
constexpr size_t  WARDEN_RELOC_DELTA_ENTRY_SIZE    = 2;
constexpr size_t  WARDEN_RELOC_ABSOLUTE_ENTRY_SIZE = 4;

// ---------------------------------------------------------------------------
// Warden packets (decrypted SMSG/CMSG_WARDEN_DATA payloads)
// ---------------------------------------------------------------------------

/// SMSG MODULE_USE: which module to run, and the RC4 key it is encrypted with.
struct WardenModuleUseRequest {
    uint8_t  opcode;
    uint8_t  moduleHash[16];      // MD5 of the encrypted module; names the cache file
    uint8_t  moduleKey[16];       // RC4 key for the module body
    uint32_t moduleSize;          // bytes the MODULE_CACHE chunks add up to
};
static_assert(sizeof(WardenModuleUseRequest) == 37);

/// SMSG MODULE_CACHE: one chunk of the module, chunkSize bytes follow.
struct WardenModuleCacheHeader {
    uint8_t  opcode;
    uint16_t chunkSize;
};
static_assert(sizeof(WardenModuleCacheHeader) == 3);

/// SMSG HASH_REQUEST: the seed the module's hash answer is keyed with.
struct WardenHashRequest {
    uint8_t opcode;
    uint8_t seed[16];
};
static_assert(sizeof(WardenHashRequest) == 17);

/// CMSG CHEAT_CHECKS_RESULT, ahead of the per-check results.
struct WardenCheatChecksResultHeader {
    uint8_t  opcode;
    uint16_t resultLength;        // bytes of results that follow
    uint32_t checksum;            // the five SHA1 dwords of the results, XORed
};
static_assert(sizeof(WardenCheatChecksResultHeader) == 7);

// Check records inside CHEAT_CHECKS_REQUEST, each after its one-byte check type.
// String indices are one-based into the packet's string table; zero is none.

/// READ_MEMORY: [1 stringIdx][4 offset][1 length].
struct WardenMemCheckRequest {
    uint8_t  moduleStringIndex;
    uint32_t address;
    uint8_t  length;
};
static_assert(sizeof(WardenMemCheckRequest) == 6);

/// FIND_MEM_IMAGE_CODE_BY_HASH / FIND_CODE_BY_HASH, short form: [4 seed][20 sha1].
struct WardenPageCheckShortRequest {
    uint8_t seed[4];
    uint8_t sha1[20];             // HMAC-SHA1(seed, pattern)
};
static_assert(sizeof(WardenPageCheckShortRequest) == 24);

/// The long form: [4 seed][20 sha1][4 offset][1 length].
struct WardenPageCheckRequest {
    uint8_t  seed[4];
    uint8_t  sha1[20];            // HMAC-SHA1(seed, pattern)
    uint32_t offset;              // where the server expects the pattern
    uint8_t  length;              // pattern length
};
static_assert(sizeof(WardenPageCheckRequest) == 29);

/// FIND_MODULE_BY_NAME: [4 seed][20 sha1] of the upper-case module name.
struct WardenModuleCheckRequest {
    uint8_t seed[4];
    uint8_t sha1[20];
};
static_assert(sizeof(WardenModuleCheckRequest) == 24);

/// FIND_DRIVER_BY_NAME: [4 seed][20 sha1][1 stringIdx].
struct WardenDriverCheckRequest {
    uint8_t seed[4];
    uint8_t sha1[20];
    uint8_t driverStringIndex;
};
static_assert(sizeof(WardenDriverCheckRequest) == 25);

/// API_CHECK: [4 seed][20 sha1][1 stringIdx][1 stringIdx2][4 offset].
struct WardenApiCheckRequest {
    uint8_t  seed[4];
    uint8_t  sha1[20];
    uint8_t  moduleStringIndex;
    uint8_t  functionStringIndex;
    uint32_t offset;
};
static_assert(sizeof(WardenApiCheckRequest) == 30);

// ---------------------------------------------------------------------------
// .cr challenge/response cache file
// ---------------------------------------------------------------------------

/// The 17 bytes a .cr file opens with. The two dwords are kept by the tools
/// that write these files; only the check opcodes are used here.
struct WardenCRFileHeader {
    uint32_t memoryRead;
    uint32_t pageScanCheck;
    uint8_t  checkOpcodes[9];     // indexed by WardenScanType
};
static_assert(sizeof(WardenCRFileHeader) == 17);

#pragma pack(pop)

/// Index into a module's nine check opcodes, in CMaNGOS WindowsScanType order.
/// Each module picks its own byte for each scan; the .cr file says which.
enum WardenScanType : uint8_t {
    WARDEN_SCAN_READ_MEMORY                 = 0,
    WARDEN_SCAN_FIND_MODULE_BY_NAME         = 1,
    WARDEN_SCAN_FIND_MEM_IMAGE_CODE_BY_HASH = 2,  // PAGE_A
    WARDEN_SCAN_FIND_CODE_BY_HASH           = 3,  // PAGE_B
    WARDEN_SCAN_HASH_CLIENT_FILE            = 4,  // MPQ
    WARDEN_SCAN_GET_LUA_VARIABLE            = 5,
    WARDEN_SCAN_API_CHECK                   = 6,  // PROC
    WARDEN_SCAN_FIND_DRIVER_BY_NAME         = 7,
    WARDEN_SCAN_CHECK_TIMING_VALUES         = 8,
    WARDEN_SCAN_TYPE_COUNT                  = 9
};

} // namespace game
} // namespace wowee
