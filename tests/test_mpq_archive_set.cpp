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
