#include "pipeline/mpq_archive_set.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <zlib.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace wowee {
namespace pipeline {

namespace {

// ---- Storm's hash and cipher -------------------------------------------------

struct CryptTable {
    uint32_t v[0x500];
    CryptTable() {
        uint32_t seed = 0x00100001;
        for (uint32_t i = 0; i < 0x100; ++i) {
            for (uint32_t j = 0, idx = i; j < 5; ++j, idx += 0x100) {
                seed = (seed * 125 + 3) % 0x2AAAAB;
                const uint32_t t1 = (seed & 0xFFFF) << 16;
                seed = (seed * 125 + 3) % 0x2AAAAB;
                const uint32_t t2 = seed & 0xFFFF;
                v[idx] = t1 | t2;
            }
        }
    }
};
const CryptTable& cryptTable() {
    static const CryptTable t;
    return t;
}

enum HashType : uint32_t { kHashOffset = 0, kHashNameA = 1, kHashNameB = 2, kHashKey = 3 };

uint32_t stormHash(const std::string& s, uint32_t type) {
    const uint32_t* t = cryptTable().v;
    uint32_t s1 = 0x7FED7FED, s2 = 0xEEEEEEEE;
    for (unsigned char ch : s) {
        if (ch == '/') ch = '\\';
        ch = static_cast<unsigned char>(std::toupper(ch));
        s1 = t[type * 0x100 + ch] ^ (s1 + s2);
        s2 = ch + s1 + s2 + (s2 << 5) + 3;
    }
    return s1;
}

void decryptBlock(uint32_t* data, size_t words, uint32_t key) {
    const uint32_t* t = cryptTable().v;
    uint32_t s2 = 0xEEEEEEEE;
    for (size_t i = 0; i < words; ++i) {
        s2 += t[0x400 + (key & 0xFF)];
        const uint32_t v = data[i] ^ (key + s2);
        data[i] = v;
        key = ((~key << 0x15) + 0x11111111) | (key >> 0x0B);
        s2 = v + s2 + (s2 << 5) + 3;
    }
}

uint32_t readU32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint16_t readU16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Block flags (MPQ_FILE_*).
constexpr uint32_t kImplode = 0x00000100;
constexpr uint32_t kCompress = 0x00000200;
constexpr uint32_t kEncrypted = 0x00010000;
constexpr uint32_t kFixKey = 0x00020000;
constexpr uint32_t kPatchFile = 0x00100000;
constexpr uint32_t kSingleUnit = 0x01000000;
constexpr uint32_t kDeleteMarker = 0x02000000;
constexpr uint32_t kSectorCrc = 0x04000000;
constexpr uint32_t kExists = 0x80000000;

constexpr uint32_t kHashEmpty = 0xFFFFFFFF;
constexpr uint32_t kHashDeleted = 0xFFFFFFFE;

} // namespace

// ---- streaming hook ------------------------------------------------------------

namespace {
std::mutex g_rangeSourceMutex;
std::function<MpqArchiveSet::RangeFetch(const std::string&)> g_rangeSource;

MpqArchiveSet::RangeFetch rangeSourceFor(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_rangeSourceMutex);
    return g_rangeSource ? g_rangeSource(path) : MpqArchiveSet::RangeFetch{};
}
}  // namespace

void MpqArchiveSet::setRangeSource(std::function<RangeFetch(const std::string& path)> source) {
    std::lock_guard<std::mutex> lock(g_rangeSourceMutex);
    g_rangeSource = std::move(source);
}

namespace {
MpqArchiveSet::ViewSink g_viewSink;   // under g_rangeSourceMutex
void tellView(const std::string& path, uint8_t* view) {
    MpqArchiveSet::ViewSink sink;
    {
        std::lock_guard<std::mutex> lock(g_rangeSourceMutex);
        sink = g_viewSink;
    }
    if (sink) sink(path, view);
}
}  // namespace

void MpqArchiveSet::setStreamViewSink(ViewSink sink) {
    std::lock_guard<std::mutex> lock(g_rangeSourceMutex);
    g_viewSink = std::move(sink);
}

// ---- one archive ---------------------------------------------------------------

struct MpqArchiveSet::Archive {
    std::string path;
    const uint8_t* base = nullptr;
    // Streaming mode: the archive is a sparse local copy filled on demand
    // (pipeline/data_stream); every range is asked for before it is read.
    // Null for an archive on disk, where everything is there already.
    MpqArchiveSet::RangeFetch stream;
    bool need(uint64_t offset, uint64_t length) const {
        return !stream || stream(offset, length);
    }
    uint64_t size = 0;
    uint64_t header = 0;  // offset of the MPQ header in the file
    uint32_t sectorSize = 4096;
    struct HashEntry {
        uint32_t nameA, nameB;
        uint16_t locale, platform;
        uint32_t block;
    };
    struct BlockEntry {
        uint64_t pos;  // relative to the header
        uint32_t csize, fsize, flags;
    };
    std::vector<HashEntry> hashes;
    std::vector<BlockEntry> blocks;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
#else
    int fd = -1;
#endif

    ~Archive() { unmap(); }

    bool map() {
#ifdef _WIN32
        file = CreateFileW(fs::path(path).wstring().c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER li{};
        if (!GetFileSizeEx(file, &li) || li.QuadPart <= 0) return false;
        size = static_cast<uint64_t>(li.QuadPart);
        stream = rangeSourceFor(path);
        // Streamed: copy-on-write (see setStreamViewSink); else read-only.
        mapping = CreateFileMappingW(file, nullptr, stream ? PAGE_WRITECOPY : PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) return false;
        base = static_cast<const uint8_t*>(MapViewOfFile(mapping, stream ? FILE_MAP_COPY : FILE_MAP_READ, 0, 0, 0));
        if (base && stream) tellView(path, const_cast<uint8_t*>(base));
        return base != nullptr;
#else
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        struct stat st{};
        if (fstat(fd, &st) != 0 || st.st_size <= 0) return false;
        size = static_cast<uint64_t>(st.st_size);
        stream = rangeSourceFor(path);
        // Streamed: private, writable (see setStreamViewSink); else shared, read-only.
        void* p = stream ? mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0)
                         : mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) return false;
        base = static_cast<const uint8_t*>(p);
        if (stream) tellView(path, static_cast<uint8_t*>(p));
        return true;
#endif
    }

    void unmap() {
        if (base && stream) tellView(path, nullptr);
#ifdef _WIN32
        if (base) UnmapViewOfFile(base);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        mapping = nullptr;
        file = INVALID_HANDLE_VALUE;
#else
        if (base) munmap(const_cast<uint8_t*>(base), size);
        if (fd >= 0) ::close(fd);
        fd = -1;
#endif
        base = nullptr;
    }

    // The header sits at a multiple of 512, possibly behind a user-data block.
    bool parse() {
        for (uint64_t at = 0; at + 0x20 <= size; at += 0x200) {
            if (!need(at, 0x20)) return false;
            const uint8_t* p = base + at;
            if (std::memcmp(p, "MPQ\x1B", 4) == 0) {
                const uint64_t hdr = at + readU32(p + 8);
                if (hdr + 0x20 <= size && need(hdr, 0x20) && std::memcmp(base + hdr, "MPQ\x1A", 4) == 0) {
                    header = hdr;
                    break;
                }
                continue;
            }
            if (std::memcmp(p, "MPQ\x1A", 4) == 0) {
                header = at;
                break;
            }
            if (at > (64u << 20)) return false;  // no header in the first 64 MB
        }
        if (!need(header, std::min<uint64_t>(0x2C, size - header))) return false;
        const uint8_t* h = base + header;
        if (std::memcmp(h, "MPQ\x1A", 4) != 0) return false;
        const uint32_t headerSize = readU32(h + 4);
        const uint16_t version = readU16(h + 12);
        sectorSize = 512u << readU16(h + 14);
        uint64_t hashPos = readU32(h + 16), blockPos = readU32(h + 20);
        const uint32_t hashCount = readU32(h + 24), blockCount = readU32(h + 28);
        uint64_t hiBlockPos = 0;
        if (version >= 1 && headerSize >= 0x2C) {
            std::memcpy(&hiBlockPos, h + 0x20, 8);
            hashPos |= static_cast<uint64_t>(readU16(h + 0x28)) << 32;
            blockPos |= static_cast<uint64_t>(readU16(h + 0x2A)) << 32;
        }
        if (hashCount == 0 || (hashCount & (hashCount - 1)) != 0) return false;
        if (header + hashPos + uint64_t(hashCount) * 16 > size) return false;
        if (header + blockPos + uint64_t(blockCount) * 16 > size) return false;

        std::vector<uint32_t> buf(size_t(hashCount) * 4);
        if (!need(header + hashPos, buf.size() * 4)) return false;
        std::memcpy(buf.data(), base + header + hashPos, buf.size() * 4);
        decryptBlock(buf.data(), buf.size(), stormHash("(hash table)", kHashKey));
        hashes.resize(hashCount);
        for (uint32_t i = 0; i < hashCount; ++i) {
            const uint32_t* e = &buf[size_t(i) * 4];
            hashes[i] = {e[0], e[1], static_cast<uint16_t>(e[2] & 0xFFFF),
                         static_cast<uint16_t>(e[2] >> 16), e[3]};
        }
        buf.assign(size_t(blockCount) * 4, 0);
        if (blockCount && !need(header + blockPos, buf.size() * 4)) return false;
        std::memcpy(buf.data(), base + header + blockPos, buf.size() * 4);
        decryptBlock(buf.data(), buf.size(), stormHash("(block table)", kHashKey));
        blocks.resize(blockCount);
        const bool hasHi = hiBlockPos != 0 && header + hiBlockPos + uint64_t(blockCount) * 2 <= size &&
                           (blockCount == 0 || need(header + hiBlockPos, uint64_t(blockCount) * 2));
        for (uint32_t i = 0; i < blockCount; ++i) {
            const uint32_t* e = &buf[size_t(i) * 4];
            uint64_t pos = e[0];
            if (hasHi) pos |= static_cast<uint64_t>(readU16(base + header + hiBlockPos + i * 2)) << 32;
            blocks[i] = {pos, e[1], e[2], e[3]};
        }
        return true;
    }

    // The block of a name, or -1. Locale-neutral entries are preferred, as the
    // client asks for its own locale and falls back to neutral (the locale
    // archives hold their files as neutral).
    int64_t lookup(uint32_t start, uint32_t a, uint32_t b) const {
        const uint32_t mask = static_cast<uint32_t>(hashes.size()) - 1;
        int64_t any = -1;
        for (uint32_t i = 0, at = start & mask; i < hashes.size(); ++i, at = (at + 1) & mask) {
            const HashEntry& e = hashes[at];
            if (e.block == kHashEmpty) break;
            if (e.block == kHashDeleted) continue;
            if (e.nameA == a && e.nameB == b && e.block < blocks.size()) {
                if (e.locale == 0) return e.block;
                if (any < 0) any = e.block;
            }
        }
        return any;
    }
};

struct MpqArchiveSet::Counters {
    std::atomic<uint64_t> reads{0}, bytes{0}, failures{0};
    std::mutex warnMutex;
    std::unordered_set<uint32_t> warnedMasks;
};

MpqArchiveSet::MpqArchiveSet() : counters_(std::make_unique<Counters>()) {}
MpqArchiveSet::~MpqArchiveSet() { close(); }

void MpqArchiveSet::close() {
    archives_.clear();
    names_.clear();
}

bool MpqArchiveSet::findArchiveDir(const std::string& dir, std::string* archiveDir) {
    auto hasMpq = [](const fs::path& d) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) return false;
        for (fs::directory_iterator it(d, ec), end; it != end && !ec; it.increment(ec)) {
            if (lower(it->path().extension().string()) == ".mpq" && it->is_regular_file(ec)) return true;
        }
        return false;
    };
    if (dir.empty()) return false;
    const fs::path d(dir);
    if (hasMpq(d)) {
        if (archiveDir) *archiveDir = d.string();
        return true;
    }
    std::error_code ec;
    for (fs::directory_iterator it(d, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->is_directory(ec) && lower(it->path().filename().string()) == "data" && hasMpq(it->path())) {
            if (archiveDir) *archiveDir = it->path().string();
            return true;
        }
    }
    return false;
}

std::vector<std::string> MpqArchiveSet::discoverArchives(const std::string& archiveDir,
                                                         std::string& localeOut) {
    static const char* kLocales[] = {"enUS", "enGB", "deDE", "frFR", "esES", "esMX", "ruRU",
                                     "koKR", "zhCN", "zhTW", "enCN", "enTW", "ptBR", "ptPT", "itIT"};
    // lower-case file name -> real path, for the folder and its locale folder
    std::unordered_map<std::string, std::string> files;
    std::error_code ec;
    localeOut.clear();
    std::string localeDir;
    for (fs::directory_iterator it(archiveDir, ec), end; it != end && !ec; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
            for (const char* loc : kLocales) {
                if (lower(name) == lower(loc) && localeOut.empty()) {
                    localeOut = loc;
                    localeDir = it->path().string();
                }
            }
        } else if (lower(it->path().extension().string()) == ".mpq") {
            files[lower(name)] = it->path().string();
        }
    }
    const std::string loc = lower(localeOut);
    if (!localeDir.empty()) {
        for (fs::directory_iterator it(localeDir, ec), end; it != end && !ec; it.increment(ec)) {
            if (lower(it->path().extension().string()) == ".mpq") {
                files[loc + "/" + lower(it->path().filename().string())] = it->path().string();
            }
        }
    }
    // Wow.exe 12340, 0x00405ab0 and the archive list it reads: the base set,
    // the locale set, then the patches tier by tier, base before locale.
    std::vector<std::string> order = {"common.mpq", "common-2.mpq", "expansion.mpq", "lichking.mpq"};
    if (!loc.empty()) {
        for (const char* n : {"backup-", "base-", "locale-", "speech-", "expansion-locale-",
                              "expansion-speech-", "lichking-locale-", "lichking-speech-"}) {
            order.push_back(loc + "/" + n + loc + ".mpq");
        }
    }
    std::vector<std::string> tiers = {""};
    for (int i = 2; i <= 9; ++i) tiers.push_back("-" + std::to_string(i));
    for (char c = 'a'; c <= 'z'; ++c) tiers.push_back(std::string("-") + c);
    for (const auto& t : tiers) {
        order.push_back("patch" + t + ".mpq");
        if (!loc.empty()) order.push_back(loc + "/patch-" + loc + t + ".mpq");
    }
    std::vector<std::string> out;
    for (const auto& n : order) {
        auto it = files.find(n);
        if (it != files.end()) out.push_back(it->second);
    }
    return out;
}

bool MpqArchiveSet::open(const std::string& dir) {
    close();
    std::string ad;
    if (!findArchiveDir(dir, &ad)) {
        LOG_ERROR("MPQ: no archives in ", dir);
        return false;
    }
    archiveDir_ = ad;
    const auto paths = discoverArchives(ad, locale_);
    for (const auto& p : paths) {
        auto a = std::make_unique<Archive>();
        a->path = p;
        if (!a->map() || !a->parse()) {
            LOG_ERROR("MPQ: cannot open archive ", p);
            continue;
        }
        LOG_INFO("MPQ: ", fs::path(p).filename().string(), " - ", a->blocks.size(), " files, sector ",
                 a->sectorSize);
        archives_.push_back(std::move(a));
    }
    LOG_WARNING("MPQ: ", archives_.size(), " archives from ", ad,
                locale_.empty() ? std::string(" (no locale folder)") : (" (locale " + locale_ + ")"));
    return !archives_.empty();
}

MpqArchiveSet::Hit MpqArchiveSet::find(const std::string& name) const {
    if (name.empty() || archives_.empty()) return {};
    const uint32_t h0 = stormHash(name, kHashOffset);
    const uint32_t ha = stormHash(name, kHashNameA);
    const uint32_t hb = stormHash(name, kHashNameB);
    for (auto it = archives_.rbegin(); it != archives_.rend(); ++it) {
        const int64_t b = (*it)->lookup(h0, ha, hb);
        if (b < 0) continue;
        const auto& be = (*it)->blocks[static_cast<size_t>(b)];
        if (!(be.flags & kExists) || (be.flags & kDeleteMarker)) return {};
        return {it->get(), static_cast<uint32_t>(b)};
    }
    return {};
}

bool MpqArchiveSet::contains(const std::string& normalizedPath) const {
    return find(normalizedPath).archive != nullptr;
}

bool MpqArchiveSet::decode(const Archive& a, uint32_t blockIndex, const std::string& name,
                           std::vector<uint8_t>& out) const {
    const auto& be = a.blocks[blockIndex];
    const uint64_t start = a.header + be.pos;
    if (start > a.size || be.csize > a.size - start) return false;
    if (!a.need(start, be.csize)) return false;
    const uint8_t* src = a.base + start;
    out.assign(be.fsize, 0);
    if (be.fsize == 0) return true;
    if (be.flags & kPatchFile) return false;  // incremental patches are Cataclysm's

    uint32_t key = 0;
    if (be.flags & kEncrypted) {
        const size_t slash = name.find_last_of('\\');
        key = stormHash(slash == std::string::npos ? name : name.substr(slash + 1), kHashKey);
        if (be.flags & kFixKey) key = (key + static_cast<uint32_t>(be.pos)) ^ be.fsize;
    }

    auto refuse = [&](uint32_t mask) {
        std::lock_guard<std::mutex> lk(counters_->warnMutex);
        if (counters_->warnedMasks.insert(mask).second) {
            LOG_ERROR("MPQ: compression 0x", std::hex, mask, std::dec, " is not supported (", name, ")");
        }
        return false;
    };
    // One compressed unit: a mask byte, then the data (zlib alone in 3.3.5a).
    auto inflateUnit = [&](const uint8_t* in, size_t inSize, uint8_t* dst, size_t dstSize) {
        if (be.flags & kImplode) return refuse(0x08);
        if (inSize < 1) return false;
        const uint8_t mask = in[0];
        if (mask != 0x02) return refuse(mask);
        uLongf got = static_cast<uLongf>(dstSize);
        if (uncompress(dst, &got, in + 1, static_cast<uLong>(inSize - 1)) != Z_OK) return false;
        return got == dstSize;
    };
    auto unit = [&](const uint8_t* in, size_t inSize, uint32_t unitKey, uint8_t* dst, size_t dstSize) {
        std::vector<uint8_t> plain;
        if (be.flags & kEncrypted) {
            plain.assign(in, in + inSize);
            decryptBlock(reinterpret_cast<uint32_t*>(plain.data()), inSize / 4, unitKey);
            in = plain.data();
        }
        if ((be.flags & (kCompress | kImplode)) && inSize < dstSize) {
            return inflateUnit(in, inSize, dst, dstSize);
        }
        if (inSize < dstSize) return false;
        std::memcpy(dst, in, dstSize);
        return true;
    };

    if (be.flags & kSingleUnit) {
        return unit(src, be.csize, key, out.data(), out.size());
    }
    const uint32_t sectors = (be.fsize + a.sectorSize - 1) / a.sectorSize;
    if (!(be.flags & (kCompress | kImplode))) {
        // Stored: the sectors are back to back, encrypted one by one if at all.
        if (be.csize < be.fsize) return false;
        for (uint32_t i = 0; i < sectors; ++i) {
            const size_t off = size_t(i) * a.sectorSize;
            const size_t n = std::min<size_t>(a.sectorSize, be.fsize - off);
            if (!unit(src + off, n, key + i, out.data() + off, n)) return false;
        }
        return true;
    }
    const size_t tableWords = sectors + 1 + ((be.flags & kSectorCrc) ? 1 : 0);
    if (tableWords * 4 > be.csize) return false;
    std::vector<uint32_t> table(tableWords);
    std::memcpy(table.data(), src, tableWords * 4);
    if (be.flags & kEncrypted) decryptBlock(table.data(), table.size(), key - 1);
    for (uint32_t i = 0; i < sectors; ++i) {
        const uint32_t s0 = table[i], s1 = table[i + 1];
        if (s1 < s0 || s1 > be.csize) return false;
        const size_t off = size_t(i) * a.sectorSize;
        const size_t n = std::min<size_t>(a.sectorSize, be.fsize - off);
        if (!unit(src + s0, s1 - s0, key + i, out.data() + off, n)) return false;
    }
    return true;
}

std::vector<uint8_t> MpqArchiveSet::read(const std::string& normalizedPath) const {
    const Hit h = find(normalizedPath);
    if (!h.archive) return {};
    std::vector<uint8_t> out;
    if (!decode(*h.archive, h.block, normalizedPath, out)) {
        counters_->failures.fetch_add(1, std::memory_order_relaxed);
        LOG_WARNING("MPQ: cannot decode ", normalizedPath, " in ", fs::path(h.archive->path).filename().string());
        return {};
    }
    counters_->reads.fetch_add(1, std::memory_order_relaxed);
    counters_->bytes.fetch_add(out.size(), std::memory_order_relaxed);
    return out;
}

void MpqArchiveSet::buildNameIndex() const {
    std::unordered_set<std::string> seen;
    for (const auto& a : archives_) {
        const std::string lf = "(listfile)";
        const int64_t b = a->lookup(stormHash(lf, kHashOffset), stormHash(lf, kHashNameA), stormHash(lf, kHashNameB));
        if (b < 0) continue;
        std::vector<uint8_t> data;
        if (!decode(*a, static_cast<uint32_t>(b), lf, data)) continue;
        std::string cur;
        auto flush = [&]() {
            if (!cur.empty()) {
                for (char& c : cur) {
                    c = (c == '/') ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                seen.insert(cur);
                cur.clear();
            }
        };
        for (uint8_t c : data) {
            if (c == '\r' || c == '\n' || c == ';' || c == 0) flush();
            else cur.push_back(static_cast<char>(c));
        }
        flush();
    }
    names_.reserve(seen.size());
    for (const auto& n : seen) {
        if (contains(n)) names_.push_back(n);
    }
    std::sort(names_.begin(), names_.end());
    LOG_INFO("MPQ: name index of ", names_.size(), " files");
}

std::vector<std::string> MpqArchiveSet::list(const std::string& prefix) const {
    std::call_once(nameIndexOnce_, [this]() { buildNameIndex(); });
    std::vector<std::string> out;
    for (auto it = std::lower_bound(names_.begin(), names_.end(), prefix); it != names_.end(); ++it) {
        if (it->compare(0, prefix.size(), prefix) != 0) break;
        out.push_back(*it);
    }
    return out;
}

MpqArchiveSet::Stats MpqArchiveSet::stats() const {
    return {counters_->reads.load(), counters_->bytes.load(), counters_->failures.load()};
}

} // namespace pipeline
} // namespace wowee
