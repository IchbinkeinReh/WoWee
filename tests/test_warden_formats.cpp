#include <catch_amalgamated.hpp>

#include "game/warden_constants.hpp"
#include "game/warden_formats.hpp"

#include <cstdint>
#include <vector>

using namespace wowee::game;

// The Warden structs replace hand-counted offsets. Each test lays the bytes out
// the way the wire or file does, at an odd offset so nothing is aligned, and
// checks that the struct reads every field from where the old offset said.

namespace {

std::vector<uint8_t> sequence(size_t count, uint8_t first) {
    std::vector<uint8_t> bytes(count);
    for (size_t i = 0; i < count; ++i) bytes[i] = static_cast<uint8_t>(first + i);
    return bytes;
}

uint32_t le32(const std::vector<uint8_t>& b, size_t at) {
    return b[at] | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) | (uint32_t(b[at + 3]) << 24);
}

} // namespace

TEST_CASE("A READ_MEMORY record reads its index, address and length", "[warden]") {
    // [1 stringIdx][4 offset][1 length], one byte past an aligned start
    std::vector<uint8_t> packet = {0xEE, 0x03, 0xC8, 0x0B, 0xCF, 0x00, 0x04};
    const auto req = loadRecord<WardenMemCheckRequest>(packet.data() + 1);
    CHECK(req.moduleStringIndex == 0x03);
    CHECK(req.address == WARDEN_TICKCOUNT_ADDRESS);
    CHECK(req.length == 4);
}

TEST_CASE("A long PAGE_A record reads seed, hash, offset and length", "[warden]") {
    const auto bytes = sequence(1 + sizeof(WardenPageCheckRequest), 0x10);
    const uint8_t* p = bytes.data() + 1;
    const auto req = loadRecord<WardenPageCheckRequest>(p);
    for (size_t i = 0; i < 4; ++i) CHECK(req.seed[i] == p[i]);
    for (size_t i = 0; i < 20; ++i) CHECK(req.sha1[i] == p[4 + i]);
    CHECK(req.offset == le32(bytes, 1 + 24));
    CHECK(req.length == p[28]);
}

TEST_CASE("MODULE, DRIVER and API_CHECK records keep the seed then the hash", "[warden]") {
    const auto bytes = sequence(1 + sizeof(WardenApiCheckRequest), 0x40);
    const uint8_t* p = bytes.data() + 1;

    const auto module = loadRecord<WardenModuleCheckRequest>(p);
    CHECK(module.seed[3] == p[3]);
    CHECK(module.sha1[0] == p[4]);

    const auto driver = loadRecord<WardenDriverCheckRequest>(p);
    CHECK(driver.driverStringIndex == p[24]);

    const auto api = loadRecord<WardenApiCheckRequest>(p);
    CHECK(api.moduleStringIndex == p[24]);
    CHECK(api.functionStringIndex == p[25]);
    CHECK(api.offset == le32(bytes, 1 + 26));
}

TEST_CASE("MODULE_USE, MODULE_CACHE and HASH_REQUEST read the packet as laid out", "[warden]") {
    const auto use = sequence(sizeof(WardenModuleUseRequest), 0x00);
    const auto moduleUse = loadRecord<WardenModuleUseRequest>(use.data());
    CHECK(moduleUse.opcode == use[0]);
    CHECK(moduleUse.moduleHash[0] == use[1]);
    CHECK(moduleUse.moduleHash[15] == use[16]);
    CHECK(moduleUse.moduleKey[0] == use[17]);
    CHECK(moduleUse.moduleKey[15] == use[32]);
    CHECK(moduleUse.moduleSize == le32(use, 33));

    const std::vector<uint8_t> cache = {WARDEN_SMSG_MODULE_CACHE, 0x34, 0x12};
    CHECK(loadRecord<WardenModuleCacheHeader>(cache.data()).chunkSize == 0x1234);

    const auto hash = sequence(sizeof(WardenHashRequest), 0x80);
    const auto hashRequest = loadRecord<WardenHashRequest>(hash.data());
    CHECK(hashRequest.seed[0] == hash[1]);
    CHECK(hashRequest.seed[15] == hash[16]);
}

TEST_CASE("The CHEAT_CHECKS_RESULT header writes opcode, length, checksum", "[warden]") {
    const WardenCheatChecksResultHeader header = {
        .opcode = WARDEN_CMSG_CHEAT_CHECKS_RESULT,
        .resultLength = 0x0102,
        .checksum = 0xA1B2C3D4,
    };
    uint8_t bytes[sizeof(header)];
    storeRecord(bytes, header);
    const uint8_t expected[] = {0x02, 0x02, 0x01, 0xD4, 0xC3, 0xB2, 0xA1};
    for (size_t i = 0; i < sizeof(expected); ++i) CHECK(bytes[i] == expected[i]);
}

TEST_CASE("A .cr header puts the check opcodes after two dwords", "[warden]") {
    const auto bytes = sequence(sizeof(WardenCRFileHeader), 0x20);
    const auto header = loadRecord<WardenCRFileHeader>(bytes.data());
    CHECK(header.memoryRead == le32(bytes, 0));
    CHECK(header.pageScanCheck == le32(bytes, 4));
    CHECK(header.checkOpcodes[WARDEN_SCAN_READ_MEMORY] == bytes[8]);
    CHECK(header.checkOpcodes[WARDEN_SCAN_CHECK_TIMING_VALUES] == bytes[16]);
}

TEST_CASE("A native Warden image header reads its tables in order", "[warden]") {
    std::vector<uint8_t> bytes(1 + sizeof(WardenNativeImageHeader) + sizeof(WardenNativeSectionDescriptor));
    for (size_t field = 0; field < 13; ++field) {
        const uint32_t value = 0x1000u * static_cast<uint32_t>(field + 1) + 7u;
        storeRecord(bytes.data() + 1 + field * 4, value);
    }
    const auto header = loadRecord<WardenNativeImageHeader>(bytes.data() + 1);
    CHECK(header.imageSize == 0x1007);
    CHECK(header.relocTableOffset == 0x3007);
    CHECK(header.relocCount == 0x4007);
    CHECK(header.exportTableOffset == 0x5007);
    CHECK(header.exportCount == 0x6007);
    CHECK(header.exportBaseOrdinal == 0x7007);
    CHECK(header.importTableOffset == 0x8007);
    CHECK(header.importCount == 0x9007);
    CHECK(header.sectionCount == 0xA007);
    const auto section = loadRecord<WardenNativeSectionDescriptor>(bytes.data() + 1 + 0x28);
    CHECK(section.imageOffset == 0xB007);
}

TEST_CASE("PE headers sit where the PE/COFF specification puts them", "[warden]") {
    STATIC_CHECK(offsetof(PeDosHeader, peHeaderOffset) == 0x3C);
    STATIC_CHECK(sizeof(PeFileHeader) == 20);
    STATIC_CHECK(offsetof(PeFileHeader, numberOfSections) == 2);
    STATIC_CHECK(offsetof(PeFileHeader, sizeOfOptionalHeader) == 16);
    STATIC_CHECK(offsetof(PeOptionalHeader32, imageBase) == 28);
    STATIC_CHECK(offsetof(PeOptionalHeader32, sizeOfImage) == 56);
    STATIC_CHECK(offsetof(PeOptionalHeader32, sizeOfHeaders) == 60);
    STATIC_CHECK(offsetof(PeSectionHeader, virtualSize) == 8);
    STATIC_CHECK(offsetof(PeSectionHeader, virtualAddress) == 12);
    STATIC_CHECK(offsetof(PeSectionHeader, sizeOfRawData) == 16);
    STATIC_CHECK(offsetof(PeSectionHeader, pointerToRawData) == 20);
    STATIC_CHECK(sizeof(PeSectionHeader) == 40);
}
