#include "game/warden_check_responder.hpp"
#include "game/warden_constants.hpp"
#include "game/warden_memory.hpp"
#include "auth/crypto.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <openssl/hmac.h>
#include <openssl/sha.h>

namespace wowee {
namespace game {

namespace {

enum CheckType { CT_MEM = 0, CT_PAGE_A, CT_PAGE_B, CT_MPQ, CT_LUA,
                 CT_DRIVER, CT_TIMING, CT_PROC, CT_MODULE, CT_UNKNOWN };
const char* const kCheckTypeNames[] = {"MEM", "PAGE_A", "PAGE_B", "MPQ", "LUA",
                                       "DRIVER", "TIMING", "PROC", "MODULE", "UNKNOWN"};

std::string hex8(uint32_t v) { char s[12]; std::snprintf(s, sizeof(s), "%08x", v); return s; }
std::string hex2(uint8_t v) { char s[4]; std::snprintf(s, sizeof(s), "%02x", v); return s; }

uint32_t readLE32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool hmacSha1Matches(const uint8_t seed[4], const uint8_t* text, size_t textLen, const uint8_t expected[20]) {
    uint8_t out[SHA_DIGEST_LENGTH];
    unsigned int outLen = 0;
    HMAC(EVP_sha1(), seed, 4, text, textLen, out, &outLen);
    return outLen == SHA_DIGEST_LENGTH && std::memcmp(out, expected, SHA_DIGEST_LENGTH) == 0;
}

bool hmacSha1Matches(const uint8_t seed[4], const std::string& text, const uint8_t expected[20]) {
    return hmacSha1Matches(seed, reinterpret_cast<const uint8_t*>(text.data()), text.size(), expected);
}

// Code scans a server expects to be found: its own sanity checks that the
// client's Warden code is present.
bool isKnownWantedCodeScan(const uint8_t seed[4], const uint8_t hash[20], uint32_t offset, uint8_t length) {
    // DB sanity check: "Warden packet process code search sanity check" (id=85)
    static constexpr uint8_t kPacketProcessSanityPattern[] = {
        0x33, 0xD2, 0x33, 0xC9, 0xE8, 0x87, 0x07, 0x1B, 0x00, 0xE8
    };
    if (offset == 13856 && length == sizeof(kPacketProcessSanityPattern) &&
        hmacSha1Matches(seed, kPacketProcessSanityPattern, sizeof(kPacketProcessSanityPattern), hash)) {
        return true;
    }
    // Scripted sanity check: "Warden Memory Read check" in wardenwin.cpp
    static constexpr uint8_t kWardenMemoryReadPattern[] = {
        0x56, 0x57, 0xFC, 0x8B, 0x54, 0x24, 0x14, 0x8B,
        0x74, 0x24, 0x10, 0x8B, 0x44, 0x24, 0x0C, 0x8B,
        0xCA, 0x8B, 0xF8, 0xC1, 0xE9, 0x02, 0x74, 0x02,
        0xF3, 0xA5, 0xB1, 0x03, 0x23, 0xCA, 0x74, 0x02,
        0xF3, 0xA4, 0x5F, 0x5E, 0xC3
    };
    return length == sizeof(kWardenMemoryReadPattern) &&
           hmacSha1Matches(seed, kWardenMemoryReadPattern, sizeof(kWardenMemoryReadPattern), hash);
}

// Pre-computed SHA1 hashes of known door M2 models that Warden checks
// to verify the client hasn't modified collision geometry (wall-hack detection).
// These hashes match the unmodified 3.3.5a client data files.
const std::unordered_map<std::string, std::array<uint8_t, 20>>& knownDoorHashes() {
    static const std::unordered_map<std::string, std::array<uint8_t, 20>> k = {
        {"world\\lordaeron\\stratholme\\activedoodads\\doors\\nox_door_plague.m2",
         {0xB4,0x45,0x2B,0x6D,0x95,0xC9,0x8B,0x18,0x6A,0x70,0xB0,0x08,0xFA,0x07,0xBB,0xAE,0xF3,0x0D,0xF7,0xA2}},
        {"world\\kalimdor\\onyxiaslair\\doors\\onyxiasgate01.m2",
         {0x75,0x19,0x5E,0x4A,0xED,0xA0,0xBC,0xAF,0x04,0x8C,0xA0,0xE3,0x4D,0x95,0xA7,0x0D,0x4F,0x53,0xC7,0x46}},
        {"world\\generic\\human\\activedoodads\\doors\\deadminedoor02.m2",
         {0x3D,0xFF,0x01,0x1B,0x9A,0xB1,0x34,0xF3,0x7F,0x88,0x50,0x97,0xE6,0x95,0x35,0x1B,0x91,0x95,0x35,0x64}},
        {"world\\kalimdor\\silithus\\activedoodads\\ahnqirajdoor\\ahnqirajdoor02.m2",
         {0xDB,0xD4,0xF4,0x07,0xC4,0x68,0xCC,0x36,0x13,0x4E,0x62,0x1D,0x16,0x01,0x78,0xFD,0xA4,0xD0,0xD2,0x49}},
        {"world\\kalimdor\\diremaul\\activedoodads\\doors\\diremaulsmallinstancedoor.m2",
         {0x0D,0xC8,0xDB,0x46,0xC8,0x55,0x49,0xC0,0xFF,0x1A,0x60,0x0F,0x6C,0x23,0x63,0x57,0xC3,0x05,0x78,0x1A}},
    };
    return k;
}

// The module DLLs a MODULE scan is answered for: system ones a real client
// has loaded, and the cheat tools a server looks for, which it has not.
struct KnownModule { const char* name; bool loaded; };
constexpr KnownModule kKnownModules[] = {
    {"KERNEL32.DLL", true}, {"USER32.DLL", true}, {"NTDLL.DLL", true}, {"WS2_32.DLL", true},
    {"WSOCK32.DLL", true}, {"ADVAPI32.DLL", true}, {"SHELL32.DLL", true}, {"GDI32.DLL", true},
    {"OPENGL32.DLL", true}, {"WINMM.DLL", true},
    {"WPESPY.DLL", false}, {"SPEEDHACK-I386.DLL", false}, {"TAMIA.DLL", false},
    {"PRXDRVPE.DLL", false}, {"D3DHOOK.DLL", false}, {"NJUMD.DLL", false}, {"RPE.DLL", false},
};

CheckType decodeCheckType(uint8_t raw, uint8_t xorByte, const uint8_t opcodes[9]) {
    const uint8_t decoded = raw ^ xorByte;
    if (decoded == opcodes[0]) return CT_MEM;    // READ_MEMORY
    if (decoded == opcodes[1]) return CT_MODULE; // FIND_MODULE_BY_NAME
    if (decoded == opcodes[2]) return CT_PAGE_A; // FIND_MEM_IMAGE_CODE_BY_HASH
    if (decoded == opcodes[3]) return CT_PAGE_B; // FIND_CODE_BY_HASH
    if (decoded == opcodes[4]) return CT_MPQ;    // HASH_CLIENT_FILE
    if (decoded == opcodes[5]) return CT_LUA;    // GET_LUA_VARIABLE
    if (decoded == opcodes[6]) return CT_PROC;   // API_CHECK
    if (decoded == opcodes[7]) return CT_DRIVER; // FIND_DRIVER_BY_NAME
    if (decoded == opcodes[8]) return CT_TIMING; // CHECK_TIMING_VALUES
    return CT_UNKNOWN;
}

// Where the check data starts: past the opcode and the string table.
size_t skipStringTable(const std::vector<uint8_t>& request, std::vector<std::string>* strings) {
    size_t pos = 1;
    while (pos < request.size()) {
        const uint8_t len = request[pos++];
        if (len == 0) break;
        if (pos + len > request.size()) break;
        if (strings) strings->emplace_back(reinterpret_cast<const char*>(request.data() + pos), len);
        pos += len;
    }
    return pos;
}

}  // namespace

bool wardenRequestHasCodeScan(const std::vector<uint8_t>& request, const uint8_t opcodes[9]) {
    if (request.size() < 3) return false;
    const uint8_t xorByte = request.back();
    for (size_t i = skipStringTable(request, nullptr); i + 1 < request.size(); ++i) {
        const uint8_t d = request[i] ^ xorByte;
        if (d == opcodes[2] || d == opcodes[3]) return true;
    }
    return false;
}

std::vector<uint8_t> buildWardenCheckResponse(const std::vector<uint8_t>& request,
                                              const WardenCheckContext& ctx,
                                              const char* pathTag) {
    if (request.size() < 3 || !ctx.opcodes) return {};
    const std::string tag = std::string("Warden: (") + pathTag + ") ";

    std::vector<std::string> strings;
    size_t pos = skipStringTable(request, &strings);
    const uint8_t xorByte = request.back();
    const size_t checkEnd = request.size() - 1;  // exclude xorByte

    auto resolveString = [&](uint8_t oneBasedIndex) -> std::string {
        if (oneBasedIndex == 0) return {};
        const size_t i = static_cast<size_t>(oneBasedIndex - 1);
        return i < strings.size() ? strings[i] : std::string();
    };
    auto decode = [&](size_t at) { return decodeCheckType(request[at], xorByte, ctx.opcodes); };

    // A code scan is 24 bytes ([4 seed][20 sha1]) on some classic servers and
    // 29 ([.. ][4 addr][1 len]) elsewhere; the one that leaves the rest of the
    // request parseable is the one that was sent.
    auto requestSizes = [](CheckType ct) -> std::vector<size_t> {
        switch (ct) {
            case CT_TIMING: return {0};
            case CT_MEM:    return {6};
            case CT_PAGE_A:
            case CT_PAGE_B: return {24, 29};
            case CT_MPQ:
            case CT_LUA:    return {1};
            case CT_DRIVER: return {25};
            case CT_PROC:   return {30};
            case CT_MODULE: return {24};
            default:        return {};
        }
    };
    std::unordered_map<size_t, bool> parseMemo;
    std::function<bool(size_t)> canParseFrom = [&](size_t at) -> bool {
        if (at == checkEnd) return true;
        if (at > checkEnd) return false;
        if (auto it = parseMemo.find(at); it != parseMemo.end()) return it->second;
        bool ok = false;
        const CheckType ct = decode(at);
        if (ct != CT_UNKNOWN) {
            for (size_t size : requestSizes(ct)) {
                if (at + 1 + size <= checkEnd && canParseFrom(at + 1 + size)) { ok = true; break; }
            }
        }
        parseMemo[at] = ok;
        return ok;
    };
    auto codeScanSize = [&](size_t at) -> size_t {
        constexpr size_t kShort = 24, kLong = 29;
        if (at + kShort <= checkEnd && canParseFrom(at + kShort)) return kShort;
        if (at + kLong <= checkEnd && canParseFrom(at + kLong)) return kLong;
        auto boundaryAfter = [&](size_t n) {
            return at + n == checkEnd || (at + n < checkEnd && decode(at + n) != CT_UNKNOWN);
        };
        if (boundaryAfter(kShort)) return kShort;
        if (boundaryAfter(kLong)) return kLong;
        const size_t remaining = checkEnd - at;
        if (remaining >= kLong) return kLong;
        if (remaining >= kShort) return kShort;
        return 0;
    };

    std::vector<uint8_t> results;
    int checkCount = 0;
    int typeCounts[CT_UNKNOWN + 1] = {};

    while (pos < checkEnd) {
        const CheckType ct = decode(pos);
        pos++;
        checkCount++;
        typeCounts[ct]++;

        switch (ct) {
            case CT_TIMING: {
                // Response: [uint8 result][uint32 ticks]
                results.push_back(0x01);
                const uint32_t ticks = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                for (int i = 0; i < 4; ++i) results.push_back(static_cast<uint8_t>(ticks >> (8 * i)));
                break;
            }
            case CT_MEM: {
                // Request: [1 stringIdx][4 offset][1 length]
                if (pos + 6 > checkEnd) { pos = checkEnd; break; }
                const std::string moduleName = resolveString(request[pos++]);
                const uint32_t offset = readLE32(request.data() + pos);
                pos += 4;
                const uint8_t readLen = request[pos++];
                if (offset == WARDEN_TICKCOUNT_ADDRESS && readLen == 4 && ctx.memory && ctx.memory->isLoaded()) {
                    // LastHardwareAction: a moment ago, as for a player at the keyboard.
                    const uint32_t now = static_cast<uint32_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
                    ctx.memory->writeLE32(WARDEN_TICKCOUNT_ADDRESS, now - 2000);
                }
                std::vector<uint8_t> bytes(readLen, 0);
                if (ctx.memory && ctx.memory->isLoaded() && ctx.memory->readMemory(offset, readLen, bytes.data())) {
                    std::string dump;
                    for (uint8_t b : bytes) dump += hex2(b) + " ";
                    LOG_DEBUG(tag, "MEM 0x", hex8(offset), " len=", (int)readLen,
                                moduleName.empty() ? "" : " module=\"" + moduleName + "\"",
                                " -> [", dump, "]");
                    results.push_back(WARDEN_MEM_CHECK_SUCCESS);
                    results.insert(results.end(), bytes.begin(), bytes.end());
                } else {
                    LOG_WARNING(tag, "MEM 0x", hex8(offset), " len=", (int)readLen, " -> unmapped");
                    results.push_back(WARDEN_MEM_CHECK_UNMAPPED);
                }
                break;
            }
            case CT_PAGE_A:
            case CT_PAGE_B: {
                const size_t size = codeScanSize(pos);
                if (size == 0) { pos = checkEnd; results.push_back(kWardenScanNotFound); break; }
                bool found = false;
                bool turtleFallback = false;
                uint32_t off = 0;
                uint8_t patLen = 0;
                if (size == 29) {
                    const uint8_t* seed = request.data() + pos;
                    const uint8_t* sha1 = seed + 4;
                    off = readLE32(seed + 24);
                    patLen = seed[28];
                    if (isKnownWantedCodeScan(seed, sha1, off, patLen)) {
                        found = true;
                    } else if (ctx.memory && ctx.memory->isLoaded() && patLen > 0) {
                        const bool hintOnly = (ct == CT_PAGE_A && ctx.turtle);
                        found = ctx.memory->searchCodePattern(seed, sha1, patLen, ct == CT_PAGE_A, off, hintOnly);
                        if (!found && !hintOnly && ctx.moduleImage && ctx.moduleImageSize >= patLen) {
                            for (size_t i = 0; i + patLen <= ctx.moduleImageSize; ++i) {
                                if (hmacSha1Matches(seed, ctx.moduleImage + i, patLen, sha1)) { found = true; break; }
                            }
                        }
                    }
                    // Turtle PAGE_A: runtime-patched offsets aren't in the
                    // on-disk PE, and the server expects them found.
                    if (!found && ct == CT_PAGE_A && ctx.turtle && off < 0x600000) {
                        found = true;
                        turtleFallback = true;
                    }
                }
                LOG_DEBUG(tag, kCheckTypeNames[ct], " offset=0x", hex8(off), " patLen=", (int)patLen,
                            " (", size, "b) found=", found ? "yes" : "no",
                            turtleFallback ? " (turtle-fallback)" : "");
                pos += size;
                results.push_back(found ? kWardenScanFound : kWardenScanNotFound);
                break;
            }
            case CT_MPQ: {
                // Request: [1 stringIdx]. Response: [0x00][20 sha1], or 0x01 not found.
                if (pos + 1 > checkEnd) { pos = checkEnd; break; }
                const std::string filePath = resolveString(request[pos++]);
                bool found = false;
                std::vector<uint8_t> hash(20, 0);
                if (!filePath.empty()) {
                    std::string key = filePath;
                    for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    std::replace(key.begin(), key.end(), '/', '\\');
                    if (auto it = knownDoorHashes().find(key); it != knownDoorHashes().end()) {
                        found = true;
                        hash.assign(it->second.begin(), it->second.end());
                    } else if (ctx.readGameFile) {
                        if (const auto data = ctx.readGameFile(filePath); !data.empty()) {
                            found = true;
                            hash = auth::Crypto::sha1(data);
                        }
                    }
                }
                LOG_DEBUG(tag, "MPQ \"", filePath.empty() ? "?" : filePath, "\" ", found ? "found" : "not found");
                if (found) {
                    results.push_back(0x00);
                    results.insert(results.end(), hash.begin(), hash.end());
                } else {
                    results.push_back(0x01);
                }
                break;
            }
            case CT_LUA: {
                // Request: [1 stringIdx]. 0x01 is "not found", with nothing after it.
                if (pos + 1 > checkEnd) { pos = checkEnd; break; }
                pos++;
                results.push_back(0x01);
                break;
            }
            case CT_DRIVER: {
                // Request: [4 seed][20 sha1][1 stringIdx]
                if (pos + 25 > checkEnd) { pos = checkEnd; break; }
                pos += 24;
                const std::string driver = resolveString(request[pos++]);
                LOG_DEBUG(tag, "DRIVER \"", driver.empty() ? "?" : driver, "\" not found");
                results.push_back(kWardenScanNotFound);
                break;
            }
            case CT_MODULE: {
                // Request: [4 seed][20 HMAC-SHA1(seed, name)]
                if (pos + 24 > checkEnd) { pos = checkEnd; results.push_back(kWardenScanNotFound); break; }
                const uint8_t* seed = request.data() + pos;
                pos += 24;
                const KnownModule* match = nullptr;
                for (const auto& m : kKnownModules) {
                    if (hmacSha1Matches(seed, m.name, seed + 4)) { match = &m; break; }
                }
                const bool loaded = match && match->loaded;
                LOG_DEBUG(tag, "MODULE \"", match ? match->name : "?", "\" ", loaded ? "found" : "not found");
                results.push_back(loaded ? kWardenScanFound : kWardenScanNotFound);
                break;
            }
            case CT_PROC: {
                // Request: [4 seed][20 sha1][1 stringIdx][1 stringIdx2][4 offset]
                if (pos + 30 > checkEnd) { pos = checkEnd; break; }
                pos += 30;
                results.push_back(0x01);
                break;
            }
            default: {
                const uint8_t raw = request[pos - 1];
                std::string opcodeList;
                for (int i = 0; i < 9; ++i) opcodeList += "0x" + hex2(ctx.opcodes[i]) + " ";
                LOG_WARNING(tag, "unknown check type raw=0x", hex2(raw), " decoded=0x", hex2(raw ^ xorByte),
                            " opcodes=[", opcodeList, "] pos=", pos, "/", checkEnd);
                pos = checkEnd;
                break;
            }
        }
    }

    {
        std::string summary;
        for (int i = 0; i <= CT_UNKNOWN; ++i) {
            if (typeCounts[i] == 0) continue;
            if (!summary.empty()) summary += " ";
            summary += std::string(kCheckTypeNames[i]) + "=" + std::to_string(typeCounts[i]);
        }
        std::string dump;
        for (uint8_t b : results) dump += hex2(b) + " ";
        LOG_INFO(tag, "Parsed ", checkCount, " checks [", summary, "] resultSize=", results.size());
        LOG_DEBUG(tag, "RESPONSE_HEX [", dump, "]");
    }

    // Checksum: XOR of the five little-endian words of SHA1(results)
    const auto digest = auth::Crypto::sha1(results);
    uint32_t checksum = 0;
    for (int i = 0; i < 5; ++i) checksum ^= readLE32(digest.data() + i * 4);

    const uint16_t length = static_cast<uint16_t>(results.size());
    std::vector<uint8_t> response;
    response.reserve(7 + results.size());
    response.push_back(0x02);  // WARDEN_CMSG_CHEAT_CHECKS_RESULT
    response.push_back(static_cast<uint8_t>(length));
    response.push_back(static_cast<uint8_t>(length >> 8));
    for (int i = 0; i < 4; ++i) response.push_back(static_cast<uint8_t>(checksum >> (8 * i)));
    response.insert(response.end(), results.begin(), results.end());
    return response;
}

}  // namespace game
}  // namespace wowee
