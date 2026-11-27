// MpqArchiveSet: the original client's archives read in place - small
// archives written here in the 3.3.5a layout (encrypted hash and block
// tables, zlib sectors with and without the CRC table, a single-unit file, a
// stored file, a deletion marker in a later patch) and read back.
#include <catch_amalgamated.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <zlib.h>

#include "pipeline/mpq_archive_set.hpp"
#include "pipeline/no_wait_reads.hpp"

using wowee::pipeline::MpqArchiveSet;
namespace fs = std::filesystem;

namespace {

uint32_t table[0x500];
void initTable() {
    uint32_t seed = 0x00100001;
    for (uint32_t i = 0; i < 0x100; ++i) {
        for (uint32_t j = 0, idx = i; j < 5; ++j, idx += 0x100) {
            seed = (seed * 125 + 3) % 0x2AAAAB;
            const uint32_t t1 = (seed & 0xFFFF) << 16;
            seed = (seed * 125 + 3) % 0x2AAAAB;
            table[idx] = t1 | (seed & 0xFFFF);
        }
    }
}
uint32_t hashOf(const std::string& s, uint32_t type) {
    uint32_t s1 = 0x7FED7FED, s2 = 0xEEEEEEEE;
    for (unsigned char ch : s) {
        if (ch == '/') ch = '\\';
        if (ch >= 'a' && ch <= 'z') ch = static_cast<unsigned char>(ch - 32);
        s1 = table[type * 0x100 + ch] ^ (s1 + s2);
        s2 = ch + s1 + s2 + (s2 << 5) + 3;
    }
    return s1;
}
void encrypt(std::vector<uint32_t>& d, uint32_t key) {
    uint32_t s2 = 0xEEEEEEEE;
    for (auto& w : d) {
        s2 += table[0x400 + (key & 0xFF)];
        const uint32_t plain = w;
        w = plain ^ (key + s2);
        key = ((~key << 0x15) + 0x11111111) | (key >> 0x0B);
        s2 = plain + s2 + (s2 << 5) + 3;
    }
}

std::vector<uint8_t> zlibUnit(const std::vector<uint8_t>& in) {
    uLongf n = compressBound(static_cast<uLong>(in.size()));
    std::vector<uint8_t> out(n + 1);
    compress(out.data() + 1, &n, in.data(), static_cast<uLong>(in.size()));
    out[0] = 0x02;
    out.resize(n + 1);
    return out;
}

struct File {
    std::string name;
    std::vector<uint8_t> data;
    enum Kind { Stored, Sectored, SectoredCrc, SingleUnit, Deleted } kind;
};

// A v0 archive, sector size 4096 (shift 3), 16 hash slots.
void writeArchive(const fs::path& path, const std::vector<File>& files) {
    constexpr uint32_t kSector = 4096;
    std::vector<uint8_t> body(32, 0);  // header first, filled at the end
    struct Block { uint32_t pos, csize, fsize, flags; };
    std::vector<Block> blocks;
    for (const auto& f : files) {
        Block b{static_cast<uint32_t>(body.size()), 0, static_cast<uint32_t>(f.data.size()), 0x80000000u};
        std::vector<uint8_t> raw;
        if (f.kind == File::Stored) {
            raw = f.data;
        } else if (f.kind == File::SingleUnit) {
            // Stored as it is when compressing does not make it smaller, as
            // Storm writes it; the flag stays.
            raw = zlibUnit(f.data);
            if (raw.size() >= f.data.size()) raw = f.data;
            b.flags |= 0x01000000 | 0x200;
        } else if (f.kind == File::Deleted) {
            b.flags |= 0x02000000;
            b.fsize = 0;
        } else {
            const bool crc = f.kind == File::SectoredCrc;
            b.flags |= 0x200 | (crc ? 0x04000000 : 0);
            const uint32_t sectors = (b.fsize + kSector - 1) / kSector;
            std::vector<std::vector<uint8_t>> units;
            for (uint32_t i = 0; i < sectors; ++i) {
                const size_t off = size_t(i) * kSector;
                std::vector<uint8_t> s(f.data.begin() + off,
                                       f.data.begin() + std::min<size_t>(off + kSector, f.data.size()));
                auto z = zlibUnit(s);
                units.push_back(z.size() < s.size() ? z : s);
            }
            std::vector<uint32_t> offs;
            uint32_t at = static_cast<uint32_t>((sectors + 1 + (crc ? 1 : 0)) * 4);
            for (const auto& u : units) { offs.push_back(at); at += static_cast<uint32_t>(u.size()); }
            offs.push_back(at);
            if (crc) offs.push_back(at);  // the CRC block would follow; empty here
            raw.resize(offs.size() * 4);
            std::memcpy(raw.data(), offs.data(), raw.size());
            for (const auto& u : units) raw.insert(raw.end(), u.begin(), u.end());
        }
        b.csize = static_cast<uint32_t>(raw.size());
        body.insert(body.end(), raw.begin(), raw.end());
        blocks.push_back(b);
    }
    std::vector<uint32_t> hashes(16 * 4, 0xFFFFFFFF);
    for (size_t i = 0; i < files.size(); ++i) {
        uint32_t slot = hashOf(files[i].name, 0) & 15;
        while (hashes[slot * 4 + 3] != 0xFFFFFFFF) slot = (slot + 1) & 15;
        hashes[slot * 4 + 0] = hashOf(files[i].name, 1);
        hashes[slot * 4 + 1] = hashOf(files[i].name, 2);
        hashes[slot * 4 + 2] = 0;  // locale neutral, platform 0
        hashes[slot * 4 + 3] = static_cast<uint32_t>(i);
    }
    std::vector<uint32_t> blockWords;
    for (const auto& b : blocks) {
        blockWords.insert(blockWords.end(), {b.pos, b.csize, b.fsize, b.flags});
    }
    encrypt(hashes, hashOf("(hash table)", 3));
    encrypt(blockWords, hashOf("(block table)", 3));
    const uint32_t hashPos = static_cast<uint32_t>(body.size());
    body.insert(body.end(), reinterpret_cast<uint8_t*>(hashes.data()),
                reinterpret_cast<uint8_t*>(hashes.data()) + hashes.size() * 4);
    const uint32_t blockPos = static_cast<uint32_t>(body.size());
    body.insert(body.end(), reinterpret_cast<uint8_t*>(blockWords.data()),
                reinterpret_cast<uint8_t*>(blockWords.data()) + blockWords.size() * 4);
    const uint32_t header[8] = {0x1A51504D, 32, static_cast<uint32_t>(body.size()), 3u << 16, hashPos, blockPos,
                                16, static_cast<uint32_t>(blocks.size())};
    std::memcpy(body.data(), header, 32);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(body.data()),
                                                 static_cast<std::streamsize>(body.size()));
}

std::vector<uint8_t> text(const std::string& s) { return {s.begin(), s.end()}; }
std::vector<uint8_t> pattern(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>((i * 7) ^ (i >> 5));
    return v;
}

struct TempData {
    fs::path root;
    TempData() {
        initTable();
        root = fs::temp_directory_path() / ("wowee_mpq_test_" + std::to_string(std::rand()));
        fs::create_directories(root / "Data" / "deDE");
        const auto big = pattern(10000);
        writeArchive(root / "Data" / "common.MPQ",
                     {{"A\\Stored.txt", text("stored bytes"), File::Stored},
                      {"A\\Zipped.bin", big, File::Sectored},
                      {"A\\Crc.bin", big, File::SectoredCrc},
                      {"A\\Gone.txt", text("old"), File::Stored},
                      {"(listfile)", text("A\\Stored.txt\r\nA\\Zipped.bin\r\nA\\Crc.bin\r\nA\\Gone.txt\r\n"),
                       File::Stored}});
        writeArchive(root / "Data" / "deDE" / "locale-deDE.MPQ",
                     {{"A\\Stored.txt", text("locale bytes"), File::SingleUnit}});
        writeArchive(root / "Data" / "patch.MPQ",
                     {{"A\\Gone.txt", {}, File::Deleted}, {"A\\Zipped.bin", pattern(5000), File::SingleUnit}});
    }
    ~TempData() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

} // namespace

TEST_CASE("The archives are found and ordered as the client orders them", "[mpq]") {
    TempData t;
    std::string dir;
    REQUIRE(MpqArchiveSet::findArchiveDir(t.root.string(), &dir));  // the game folder: its Data
    std::string locale;
    const auto order = MpqArchiveSet::discoverArchives(dir, locale);
    CHECK(locale == "deDE");
    REQUIRE(order.size() == 3);
    CHECK(fs::path(order[0]).filename() == "common.MPQ");
    CHECK(fs::path(order[1]).filename() == "locale-deDE.MPQ");
    CHECK(fs::path(order[2]).filename() == "patch.MPQ");
}

TEST_CASE("Files are read from the newest archive that holds them", "[mpq]") {
    TempData t;
    MpqArchiveSet set;
    REQUIRE(set.open(t.root.string()));
    CHECK(set.archiveCount() == 3);
    const auto stored = set.read("a\\stored.txt");  // the locale archive's single unit wins
    CHECK(std::string(stored.begin(), stored.end()) == "locale bytes");
    CHECK(set.read("a\\zipped.bin") == pattern(5000));  // the patch's single unit, compressed
    CHECK(set.read("a\\crc.bin") == pattern(10000));  // two zlib sectors behind a CRC table entry
    CHECK(set.contains("a/crc.bin"));
}

TEST_CASE("A deletion marker in a patch hides the older file", "[mpq]") {
    TempData t;
    MpqArchiveSet set;
    REQUIRE(set.open(t.root.string()));
    CHECK_FALSE(set.contains("a\\gone.txt"));
    CHECK(set.read("a\\gone.txt").empty());
    CHECK_FALSE(set.contains("a\\never.txt"));
}

TEST_CASE("The listfiles name what is there", "[mpq]") {
    TempData t;
    MpqArchiveSet set;
    REQUIRE(set.open(t.root.string()));
    const auto names = set.list("a\\");
    CHECK(names == std::vector<std::string>{"a\\crc.bin", "a\\stored.txt", "a\\zipped.bin"});
}

TEST_CASE("Read with positional reads instead of a mapping, the same files come out", "[mpq]") {
    // iOS's way (setMapArchives(false)): its address space cannot take the
    // archives whole.
    TempData t;
    MpqArchiveSet::setMapArchives(false);
    {
        MpqArchiveSet set;
        REQUIRE(set.open(t.root.string()));
        CHECK(set.archiveCount() == 3);
        const auto stored = set.read("a\\stored.txt");
        CHECK(std::string(stored.begin(), stored.end()) == "locale bytes");
        CHECK(set.read("a\\zipped.bin") == pattern(5000));
        CHECK(set.read("a\\crc.bin") == pattern(10000));
        CHECK_FALSE(set.contains("a\\gone.txt"));
        CHECK(set.list("a\\") == std::vector<std::string>{"a\\crc.bin", "a\\stored.txt", "a\\zipped.bin"});
    }
    MpqArchiveSet::setMapArchives(true);
}

namespace {
/// Rewrites block `index`'s file size in an archive written by writeArchive
/// (v0 header at 0, block table encrypted).
void setBlockFileSize(const fs::path& path, uint32_t index, uint32_t fsize) {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    uint32_t header[8];
    f.read(reinterpret_cast<char*>(header), sizeof(header));
    std::vector<uint32_t> words(size_t(header[7]) * 4);
    f.seekg(header[5]);
    f.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
    // Decrypted as the reader does, changed, encrypted again.
    uint32_t key = hashOf("(block table)", 3), s2 = 0xEEEEEEEE;
    for (auto& w : words) {
        s2 += table[0x400 + (key & 0xFF)];
        w ^= key + s2;
        key = ((~key << 0x15) + 0x11111111) | (key >> 0x0B);
        s2 = w + s2 + (s2 << 5) + 3;
    }
    words[size_t(index) * 4 + 2] = fsize;
    encrypt(words, hashOf("(block table)", 3));
    f.seekp(header[5]);
    f.write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
}
}  // namespace

TEST_CASE("A damaged size in the block table reads as a missing file, not an allocation", "[mpq]") {
    TempData t;
    // common.MPQ's A\Crc.bin (block 2) claiming 3.5 GB.
    setBlockFileSize(t.root / "Data" / "common.MPQ", 2, 0xE0000000u);
    for (const bool map : {true, false}) {
        MpqArchiveSet::setMapArchives(map);
        MpqArchiveSet set;
        REQUIRE(set.open(t.root.string()));
        CHECK(set.read("a\\crc.bin").empty());
        CHECK(set.read("a\\zipped.bin") == pattern(5000));   // the rest reads on
        CHECK(set.stats().failures == 1);
    }
    MpqArchiveSet::setMapArchives(true);
}

// ---- streaming mode: the same archives read through a chunk cache -------------

#include "pipeline/chunk_cache.hpp"
#include "pipeline/stream_wait.hpp"

#include <chrono>
#include <thread>

#include <map>
#include <memory>

TEST_CASE("Streaming: the archives read through a chunk cache, fetching only what is read",
          "[mpq][stream]") {
    using wowee::pipeline::ChunkCache;
    TempData remote;
    const fs::path mirror = remote.root.parent_path() / (remote.root.filename().string() + "_mirror");
    std::map<std::string, uint64_t> fetched;
    uint64_t total = 0;

    bool serverUp = true;
    auto run = [&](uint64_t& bytesOut, bool map, bool fresh) {
        MpqArchiveSet::setMapArchives(map);
        bytesOut = 0;
        std::vector<std::unique_ptr<ChunkCache>> caches;
        std::map<std::string, ChunkCache*> byPath;
        for (const auto& rel : {"Data/common.MPQ", "Data/deDE/locale-deDE.MPQ", "Data/patch.MPQ"}) {
            const fs::path src = remote.root / rel;
            const fs::path dst = mirror / rel;
            auto c = std::make_unique<ChunkCache>();
            const std::string srcPath = src.string();
            uint64_t* counter = &bytesOut;
            const bool* up = &serverUp;
            REQUIRE(c->open(dst.string(), fs::file_size(src), 512, "v1",
                            [srcPath, counter, up](uint64_t off, uint64_t len, std::vector<uint8_t>& out,
                                                   std::string& err) {
                                if (!*up) {
                                    err = "server stopped";
                                    return false;
                                }
                                std::ifstream in(srcPath, std::ios::binary);
                                in.seekg(static_cast<std::streamoff>(off));
                                out.resize(static_cast<size_t>(len));
                                in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(len));
                                *counter += len;
                                return in.gcount() == static_cast<std::streamsize>(len);
                            }));
            byPath[fs::absolute(dst).lexically_normal().string()] = c.get();
            caches.push_back(std::move(c));
        }
        // The chunks held in memory past the disk limit, as the stream hands
        // them to the reader (pipeline/data_stream).
        MpqArchiveSet::setStreamMemorySource([&byPath](const std::string& p) -> MpqArchiveSet::MemoryRead {
            auto it = byPath.find(fs::absolute(fs::path(p)).lexically_normal().string());
            if (it == byPath.end()) return {};
            ChunkCache* c = it->second;
            return [c](uint64_t o, uint64_t l, uint8_t* d) { return c->readMemory(o, l, d); };
        });
        MpqArchiveSet::setRangeSource([&byPath](const std::string& p) -> MpqArchiveSet::RangeFetch {
            auto it = byPath.find(fs::absolute(fs::path(p)).lexically_normal().string());
            if (it == byPath.end()) return {};
            ChunkCache* c = it->second;
            return [c](uint64_t o, uint64_t l) { return c->ensure(o, l); };
        });
        MpqArchiveSet set;
        REQUIRE(set.open(mirror.string()));
        CHECK(set.archiveCount() == 3);
        // The server gone after the open: a file not cached yet reads as a
        // missing file - no throw, no bytes that never arrived - and once
        // the server is back, it reads. A restart has it cached.
        serverUp = false;
        const auto whileDown = set.read("a\\crc.bin");
        serverUp = true;
        if (fresh) CHECK(whileDown.empty());
        else CHECK(whileDown == pattern(10000));
        CHECK(set.read("a\\crc.bin") == pattern(10000));
        const auto stored = set.read("a\\stored.txt");
        CHECK(std::string(stored.begin(), stored.end()) == "locale bytes");
        set.close();
        MpqArchiveSet::setRangeSource({});
        MpqArchiveSet::setStreamMemorySource({});
    };

    for (const auto& rel : {"Data/common.MPQ", "Data/deDE/locale-deDE.MPQ", "Data/patch.MPQ"}) {
        total += fs::file_size(remote.root / rel);
    }
    for (const bool map : {true, false}) {
        INFO("mapped: " << map);
        std::error_code ec;
        fs::remove_all(mirror, ec);
        uint64_t first = 0, second = 0;
        run(first, map, true);
        // Headers, tables and the two files read - not the zipped file nobody read.
        CHECK(first > 0);
        CHECK(first < total);
        // A restart: everything it reads is in the cache already.
        run(second, map, false);
        CHECK(second == 0);
    }
    MpqArchiveSet::setMapArchives(true);
    std::error_code ec;
    fs::remove_all(mirror, ec);
}

namespace {
/// A streamed copy of `remote`'s archives under `mirror`: one ChunkCache per
/// archive (chunks of `chunk` bytes, fetched from the remote file, padded
/// with zeros to `padTo` when that is larger), wired to MpqArchiveSet as
/// pipeline/data_stream wires it. Unwired again when it goes.
struct StreamedCopy {
    std::vector<std::unique_ptr<wowee::pipeline::ChunkCache>> caches;
    std::map<std::string, wowee::pipeline::ChunkCache*> byPath;

    StreamedCopy(const fs::path& remote, const fs::path& mirror, const std::vector<std::string>& rels,
                 uint32_t chunk, uint64_t padTo = 0, int delayMs = 0) {
        using wowee::pipeline::ChunkCache;
        for (const auto& rel : rels) {
            const fs::path src = remote / rel;
            const uint64_t real = fs::file_size(src);
            const uint64_t size = std::max(real, padTo);
            auto c = std::make_unique<ChunkCache>();
            const std::string srcPath = src.string();
            REQUIRE(c->open((mirror / rel).string(), size, chunk, "v1",
                            [srcPath, real, delayMs](uint64_t off, uint64_t len, std::vector<uint8_t>& out,
                                                     std::string&) {
                                // A slow server, each request this long.
                                if (delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                                out.assign(static_cast<size_t>(len), 0);
                                if (off < real) {
                                    std::ifstream in(srcPath, std::ios::binary);
                                    in.seekg(static_cast<std::streamoff>(off));
                                    in.read(reinterpret_cast<char*>(out.data()),
                                            static_cast<std::streamsize>(std::min(len, real - off)));
                                }
                                return true;
                            }));
            byPath[fs::absolute(mirror / rel).lexically_normal().string()] = c.get();
            caches.push_back(std::move(c));
        }
        auto find = [this](const std::string& p) -> ChunkCache* {
            auto it = byPath.find(fs::absolute(fs::path(p)).lexically_normal().string());
            return it == byPath.end() ? nullptr : it->second;
        };
        MpqArchiveSet::setRangeSource([find](const std::string& p) -> MpqArchiveSet::RangeFetch {
            ChunkCache* c = find(p);
            if (!c) return {};
            return [c](uint64_t o, uint64_t l) { return c->ensure(o, l); };
        });
        MpqArchiveSet::setStreamMemorySource([find](const std::string& p) -> MpqArchiveSet::MemoryRead {
            ChunkCache* c = find(p);
            if (!c) return {};
            return [c](uint64_t o, uint64_t l, uint8_t* d) { return c->readMemory(o, l, d); };
        });
    }
    ~StreamedCopy() {
        MpqArchiveSet::setRangeSource({});
        MpqArchiveSet::setStreamMemorySource({});
    }
};
}  // namespace

TEST_CASE("Streaming: a read that may not wait is filled in the background, not failed",
          "[mpq][stream][nowait]") {
    // Travel in streaming mode: the main thread asks for a file that is not
    // here yet (a minimap picture of the ground ahead). It is not waited for
    // and not taken for a missing file; the frames go on and it reads whole
    // once the background has fetched it.
    namespace sw = wowee::pipeline::stream_wait;
    using Clock = std::chrono::steady_clock;
    TempData remote;
    const fs::path mirror = remote.root.parent_path() / (remote.root.filename().string() + "_nowaitmirror");
    std::error_code ec;
    fs::remove_all(mirror, ec);
    {
        StreamedCopy copy(remote.root, mirror, {"Data/common.MPQ", "Data/deDE/locale-deDE.MPQ", "Data/patch.MPQ"},
                          512, 0, 150);
        MpqArchiveSet set;
        REQUIRE(set.open(mirror.string()));   // the tables: read behind the start's screen
        const uint64_t failuresBefore = set.stats().failures;
        int frames = 0;
        double longestFrameMs = 0.0;
        std::vector<uint8_t> got;
        const auto start = Clock::now();
        while (got.empty() && std::chrono::duration<double>(Clock::now() - start).count() < 20.0) {
            const auto frame = Clock::now();
            {
                sw::NoWait noWait;
                got = set.read("a\\crc.bin");
                if (got.empty()) CHECK(noWait.deferred());
            }
            longestFrameMs = std::max(longestFrameMs,
                                      std::chrono::duration<double, std::milli>(Clock::now() - frame).count());
            ++frames;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(got == pattern(10000));
        CHECK(frames > 1);                 // it came later, the frames went on
        CHECK(longestFrameMs < 100.0);     // none waited for the 150 ms server
        CHECK(set.stats().failures == failuresBefore);   // never counted as missing
        set.close();
    }
    fs::remove_all(mirror, ec);
}

TEST_CASE("Streaming: a sound kit's clips are gathered over frames, none waited for",
          "[mpq][stream][nowait][kit]") {
    // A zone change in streaming mode: the ambience asks for its kit's clips
    // on the main thread. NoWaitReads reads what is here, leaves the rest to
    // the background and is asked again each frame until every clip is in;
    // a clip the archives do not have is settled at once as missing, and
    // what came is not read twice.
    namespace sw = wowee::pipeline::stream_wait;
    using Clock = std::chrono::steady_clock;
    TempData remote;
    const fs::path mirror = remote.root.parent_path() / (remote.root.filename().string() + "_kitmirror");
    std::error_code ec;
    fs::remove_all(mirror, ec);
    {
        StreamedCopy copy(remote.root, mirror, {"Data/common.MPQ", "Data/deDE/locale-deDE.MPQ", "Data/patch.MPQ"},
                          512, 0, 150);
        MpqArchiveSet set;
        REQUIRE(set.open(mirror.string()));
        const std::vector<std::string> paths = {"a\\crc.bin", "a\\zipped.bin", "a\\stored.txt", "a\\nothere.wav"};
        wowee::pipeline::NoWaitReads reads;
        std::map<std::string, int> readsOf;
        auto read = [&](const std::string& p) {
            auto data = set.read(p);
            if (!data.empty()) ++readsOf[p];
            return data;
        };
        int frames = 0;
        double longestFrameMs = 0.0;
        bool ready = false;
        const auto start = Clock::now();
        while (!ready && std::chrono::duration<double>(Clock::now() - start).count() < 20.0) {
            const auto frame = Clock::now();
            ready = reads.ready(paths, read);
            longestFrameMs = std::max(longestFrameMs,
                                      std::chrono::duration<double, std::milli>(Clock::now() - frame).count());
            ++frames;
            if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(ready);
        CHECK(frames > 1);                // the clips came later, the frames went on
        CHECK(longestFrameMs < 100.0);    // no frame waited for the 150 ms server
        CHECK(reads.take("a\\crc.bin") == pattern(10000));
        CHECK(reads.take("a\\zipped.bin") == pattern(5000));
        const auto stored = reads.take("a\\stored.txt");
        CHECK(std::string(stored.begin(), stored.end()) == "locale bytes");
        CHECK(reads.take("a\\nothere.wav").empty());   // missing, settled, not pending forever
        CHECK(reads.held() == 0);
        for (const auto& [p, n] : readsOf) CHECK(n == 1);   // nothing read twice
        CHECK_FALSE(sw::noWait());   // no scope left behind
        set.close();
    }
    fs::remove_all(mirror, ec);
}

TEST_CASE("NoWaitReads on a local install reads every clip at the first ask", "[mpq][nowait][kit]") {
    // Local MPQ mode: nothing is ever deferred, so a kit is complete at once
    // - the same as the plain reads it replaced.
    TempData data;
    MpqArchiveSet set;
    REQUIRE(set.open(data.root.string()));
    wowee::pipeline::NoWaitReads reads;
    const std::vector<std::string> paths = {"a\\crc.bin", "a\\nothere.wav"};
    CHECK(reads.ready(paths, [&](const std::string& p) { return set.read(p); }));
    CHECK(reads.take("a\\crc.bin") == pattern(10000));
    CHECK(reads.take("a\\nothere.wav").empty());
}

TEST_CASE("Streaming past the disk limit: what is held in memory reads through the archives",
          "[mpq][stream][limit]") {
    // Chunks past the limit are not written to the copy (it has holes there);
    // the reader lays them over what it read, mapped or not.
    using wowee::pipeline::ChunkCache;
    TempData remote;
    const fs::path mirror = remote.root.parent_path() / (remote.root.filename().string() + "_memmirror");
    const std::vector<std::string> rels = {"Data/common.MPQ", "Data/deDE/locale-deDE.MPQ", "Data/patch.MPQ"};
    for (const bool map : {true, false}) {
        INFO("mapped: " << map);
        std::error_code ec;
        fs::remove_all(mirror, ec);
        MpqArchiveSet::setMapArchives(map);
        {
            StreamedCopy copy(remote.root, mirror, rels, 512);
            // Full already: every chunk fetched from here goes to memory.
            ChunkCache::setDiskLimit(ChunkCache::diskBytes() + 1);
            MpqArchiveSet set;
            REQUIRE(set.open(mirror.string()));
            CHECK(set.archiveCount() == 3);
            CHECK(set.read("a\\crc.bin") == pattern(10000));
            CHECK(set.read("a\\zipped.bin") == pattern(5000));
            const auto stored = set.read("a\\stored.txt");
            CHECK(std::string(stored.begin(), stored.end()) == "locale bytes");
            uint64_t inMemory = 0, onDisk = 0;
            for (const auto& c : copy.caches) {
                inMemory += c->chunksInMemory();
                onDisk += c->chunksPresent();
            }
            CHECK(inMemory > 0);
            CHECK(onDisk == 0);   // nothing reached the copies
            set.close();
            ChunkCache::setDiskLimit(0);
        }
    }
    MpqArchiveSet::setMapArchives(true);
    std::error_code ec;
    fs::remove_all(mirror, ec);
}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>

TEST_CASE("A streamed archive is mapped without charging its size as commit", "[mpq][stream]") {
    // A copy-on-write view (what streamed archives were mapped as) is charged
    // its whole size as commit when it is mapped: 17 GB for a 3.3.5a install,
    // and on a PC with a small pagefile archives would not map at all. A
    // read-only view of the file is backed by the file.
    TempData remote;
    const fs::path mirror = remote.root.parent_path() / (remote.root.filename().string() + "_commitmirror");
    std::error_code ec;
    fs::remove_all(mirror, ec);
    constexpr uint64_t kSize = 512ull << 20;   // sparse: only what is read takes room
    auto commit = [] {
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), pmc.cb);
        return static_cast<uint64_t>(pmc.PrivateUsage);
    };
    MpqArchiveSet::setMapArchives(true);
    {
        StreamedCopy copy(remote.root, mirror, {"Data/common.MPQ"}, 64 * 1024, kSize);
        const uint64_t before = commit();
        MpqArchiveSet set;
        REQUIRE(set.open(mirror.string()));
        const uint64_t after = commit();
        CHECK(set.read("a\\crc.bin") == pattern(10000));
        INFO("commit before " << before << ", after open " << after);
        CHECK(after < before + kSize / 8);
        set.close();
    }
    fs::remove_all(mirror, ec);
}
#endif
