#pragma once

// The `.cr` file: a module's Warden challenge/response constants, cached.
//
// A Warden module answers the server's HASH_REQUEST with four fixed values -
// the server's seed, the 20-byte reply, and the two 16-byte RC4 keys the
// exchange switches to - and tags each cheat-check with nine module-specific
// opcodes. All of that is constant for a given module, so it is cached per
// module MD5 rather than recomputed. The format is small and fixed:
//
//   [4 memoryRead][4 pageScanCheck][9 checkOpcodes]   = 17-byte header
//   ( [16 seed][20 reply][16 clientKey][16 serverKey] ) * N  = 68 bytes each
//
// The first eight header bytes are historical and unread; the nine opcodes and
// the records are what matter.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace wowee {
namespace game {

/// One module's HASH_REQUEST answer: the seed it is asked with, the reply it
/// gives, and the RC4 keys the exchange then switches to. clientKey encrypts
/// client->server, serverKey decrypts server->client.
struct WardenCRRecord {
    uint8_t seed[16] = {};
    uint8_t reply[20] = {};
    uint8_t clientKey[16] = {};
    uint8_t serverKey[16] = {};
};

inline constexpr size_t kWardenCRHeaderSize = 17;
inline constexpr size_t kWardenCRRecordSize = 68;  // 16 + 20 + 16 + 16

/// Where the Warden cache lives on this platform. The one definition, so the
/// reader, the writer and the module cache all look in the same place.
inline std::string wardenCacheDir() {
#ifdef _WIN32
    if (const char* h = std::getenv("APPDATA")) return std::string(h) + "\\wowee\\warden_cache";
    return ".\\warden_cache";
#else
    if (const char* h = std::getenv("HOME")) return std::string(h) + "/.local/share/wowee/warden_cache";
    return "./warden_cache";
#endif
}

inline std::string wardenCRPath(const std::string& moduleHashHex) {
    return wardenCacheDir() + "/" + moduleHashHex + ".cr";
}

/// Read a `.cr` file. Fills the nine opcodes and the records; returns false if
/// the file is absent or too small to hold a header and at least one record.
inline bool readWardenCR(const std::string& path, uint8_t outOpcodes[9],
                         std::vector<WardenCRRecord>& outRecords) {
    outRecords.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size < static_cast<std::streamoff>(kWardenCRHeaderSize + kWardenCRRecordSize)) return false;

    in.seekg(8);  // skip memoryRead + pageScanCheck
    in.read(reinterpret_cast<char*>(outOpcodes), 9);

    const size_t count =
        (static_cast<size_t>(size) - kWardenCRHeaderSize) / kWardenCRRecordSize;
    outRecords.resize(count);
    for (auto& r : outRecords) {
        in.read(reinterpret_cast<char*>(r.seed), 16);
        in.read(reinterpret_cast<char*>(r.reply), 20);
        in.read(reinterpret_cast<char*>(r.clientKey), 16);
        in.read(reinterpret_cast<char*>(r.serverKey), 16);
    }
    return in.good() && !outRecords.empty();
}

/// The module every TrinityCore and AzerothCore realm sends to a 3.3.5a client
/// (MD5 79C0768D657977D697E10BAD956CCED1). Those servers ask it one fixed seed
/// and expect SHA1(clientKey) back, so its whole `.cr` is public: the constants
/// are AzerothCore's WardenModuleWin.h, the opcodes its WardenCheckType.
/// Without it a realm that sends this module goes unanswered unless someone has
/// hand-placed a `.cr` file, and every check decodes against zeroed opcodes.
inline bool knownWardenCR(const std::string& moduleHashHex, uint8_t outOpcodes[9],
                          std::vector<WardenCRRecord>& outRecords) {
    outRecords.clear();
    if (moduleHashHex != "79c0768d657977d697e10bad956cced1") return false;

    // MEM, MODULE, PAGE_A, PAGE_B, MPQ, LUA, PROC, DRIVER, TIMING
    static constexpr uint8_t kOpcodes[9] = {0xF3, 0xD9, 0xB2, 0xBF, 0x98, 0x8B, 0x7E, 0x71, 0x57};
    static constexpr WardenCRRecord kRecord = {
        {0x4D, 0x80, 0x8D, 0x2C, 0x77, 0xD9, 0x05, 0xC4, 0x1A, 0x63, 0x80, 0xEC, 0x08, 0x58, 0x6A, 0xFE},
        {0x56, 0x8C, 0x05, 0x4C, 0x78, 0x1A, 0x97, 0x2A, 0x60, 0x37,
         0xA2, 0x29, 0x0C, 0x22, 0xB5, 0x25, 0x71, 0xA0, 0x6F, 0x4E},
        {0x7F, 0x96, 0xEE, 0xFD, 0xA5, 0xB6, 0x3D, 0x20, 0xA4, 0xDF, 0x8E, 0x00, 0xCB, 0xF4, 0x83, 0x04},
        {0xC2, 0xB7, 0xAD, 0xED, 0xFC, 0xCC, 0xA9, 0xC2, 0xBF, 0xB3, 0xF8, 0x56, 0x02, 0xBA, 0x80, 0x9B},
    };
    std::memcpy(outOpcodes, kOpcodes, 9);
    outRecords.push_back(kRecord);
    return true;
}

}  // namespace game
}  // namespace wowee
