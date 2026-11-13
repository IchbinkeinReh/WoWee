// ChunkCache / ChunkBitmap: the streaming data mode's local copy of a remote
// file. Chunks are fetched only when a read needs them, consecutive missing
// ones in one request; what is there is a bitmap kept across restarts, thrown
// away for another version of the file; a failed fetch leaves nothing marked.
#include <catch_amalgamated.hpp>

#include <atomic>
#include <map>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#include "pipeline/chunk_cache.hpp"

using wowee::pipeline::ChunkBitmap;
using wowee::pipeline::ChunkCache;
namespace fs = std::filesystem;

namespace {

struct Remote {
    std::vector<uint8_t> bytes;
    std::atomic<int> requests{0};
    std::atomic<uint64_t> sent{0};
    std::atomic<bool> broken{false};
    std::vector<std::pair<uint64_t, uint64_t>> asked;
    std::mutex askedMutex;

    explicit Remote(size_t n) : bytes(n) {
        for (size_t i = 0; i < n; ++i) bytes[i] = static_cast<uint8_t>((i * 131) ^ (i >> 9));
    }
    ChunkCache::Fetcher fetcher() {
        return [this](uint64_t off, uint64_t len, std::vector<uint8_t>& out, std::string& err) {
            if (broken) {
                err = "connection refused";
                return false;
            }
            ++requests;
            sent += len;
            {
                std::lock_guard<std::mutex> lock(askedMutex);
                asked.push_back({off, len});
            }
            out.assign(bytes.begin() + static_cast<std::ptrdiff_t>(off),
                       bytes.begin() + static_cast<std::ptrdiff_t>(off + len));
            return true;
        };
    }
};

struct Dir {
    fs::path root;
    Dir() {
        static std::atomic<int> n{0};
        root = fs::temp_directory_path() / ("wowee_chunks_" + std::to_string(n++) + "_" +
                                            std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(root);
    }
    ~Dir() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

std::vector<uint8_t> readLocal(const fs::path& p, uint64_t off, uint64_t len) {
    std::ifstream in(p, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(off));
    std::vector<uint8_t> v(static_cast<size_t>(len));
    in.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(len));
    return v;
}

constexpr uint32_t kChunk = 4096;

}  // namespace

TEST_CASE("Bitmap: set, count, save and load", "[chunks]") {
    Dir d;
    ChunkBitmap b;
    b.reset(10 * kChunk + 5, kChunk, "abc");
    CHECK(b.chunkCount() == 11);
    CHECK_FALSE(b.has(3));
    b.set(3);
    b.set(10);
    b.set(99);  // past the end: ignored
    CHECK(b.has(3));
    CHECK(b.has(10));
    CHECK(b.present() == 2);
    const std::string p = (d.root / "f.idx").string();
    REQUIRE(b.save(p));

    ChunkBitmap c;
    REQUIRE(c.load(p, 10 * kChunk + 5, kChunk, "abc"));
    CHECK(c.has(3));
    CHECK(c.has(10));
    CHECK(c.present() == 2);
    // Another version of the file, another chunk size: not this bitmap.
    CHECK_FALSE(c.load(p, 10 * kChunk + 5, kChunk, "abd"));
    CHECK_FALSE(c.load(p, 10 * kChunk + 6, kChunk, "abc"));
    CHECK_FALSE(c.load(p, 10 * kChunk + 5, kChunk * 2, "abc"));
    // Damaged: cut short.
    fs::resize_file(p, fs::file_size(p) - 1);
    CHECK_FALSE(c.load(p, 10 * kChunk + 5, kChunk, "abc"));
}

TEST_CASE("Only the chunks a read needs are fetched, runs in one request", "[chunks]") {
    Dir d;
    Remote r(100 * kChunk + 123);
    ChunkCache cache;
    const fs::path local = d.root / "Data" / "common.MPQ";
    REQUIRE(cache.open(local.string(), r.bytes.size(), kChunk, "v1", r.fetcher()));
    // The copy has its full size at once (it is mapped like an archive).
    CHECK(fs::file_size(local) == r.bytes.size());
    CHECK(r.requests == 0);

    // A read inside chunk 0: one chunk.
    REQUIRE(cache.ensure(10, 20));
    CHECK(r.requests == 1);
    CHECK(r.sent == kChunk);
    // Across chunks 5..7: one request of three chunks.
    REQUIRE(cache.ensure(5 * kChunk + 100, 2 * kChunk));
    CHECK(r.requests == 2);
    CHECK(r.asked.back() == std::make_pair<uint64_t, uint64_t>(5 * kChunk, 3 * kChunk));
    // Again: nothing.
    REQUIRE(cache.ensure(5 * kChunk, 3 * kChunk));
    CHECK(r.requests == 2);
    // Over present and missing chunks: only the missing ones.
    REQUIRE(cache.ensure(4 * kChunk, 5 * kChunk));
    CHECK(r.requests == 4);  // chunk 4, then chunk 8
    // The last, short chunk.
    REQUIRE(cache.ensure(r.bytes.size() - 3, 3));
    CHECK(r.asked.back().second == 123);

    CHECK(readLocal(local, 5 * kChunk + 100, 2 * kChunk) ==
          std::vector<uint8_t>(r.bytes.begin() + 5 * kChunk + 100, r.bytes.begin() + 7 * kChunk + 100));
    CHECK(cache.chunksPresent() == 7);  // 0, 4, 5, 6, 7, 8, 100
    // Out of range: refused, nothing fetched.
    CHECK_FALSE(cache.ensure(r.bytes.size(), 1));
    CHECK(r.requests == 5);
}

TEST_CASE("A restart fetches nothing it already has", "[chunks]") {
    Dir d;
    Remote r(64 * kChunk);
    const fs::path local = d.root / "patch.MPQ";
    {
        ChunkCache cache;
        REQUIRE(cache.open(local.string(), r.bytes.size(), kChunk, "v1", r.fetcher()));
        REQUIRE(cache.ensure(0, 10 * kChunk));
        REQUIRE(cache.ensure(40 * kChunk, kChunk));
    }
    const int before = r.requests;
    {
        ChunkCache cache;
        REQUIRE(cache.open(local.string(), r.bytes.size(), kChunk, "v1", r.fetcher()));
        CHECK(cache.chunksPresent() == 11);
        REQUIRE(cache.ensure(0, 10 * kChunk));
        REQUIRE(cache.ensure(40 * kChunk + 7, 100));
        CHECK(r.requests == before);
        CHECK(readLocal(local, 40 * kChunk, kChunk) ==
              std::vector<uint8_t>(r.bytes.begin() + 40 * kChunk, r.bytes.begin() + 41 * kChunk));
    }
    // A new version of the file on the server: the cache starts over.
    {
        ChunkCache cache;
        REQUIRE(cache.open(local.string(), r.bytes.size(), kChunk, "v2", r.fetcher()));
        CHECK(cache.chunksPresent() == 0);
        REQUIRE(cache.ensure(0, 1));
        CHECK(r.requests == before + 1);
    }
}

TEST_CASE("A broken connection marks nothing and the next read tries again", "[chunks]") {
    Dir d;
    Remote r(16 * kChunk);
    ChunkCache cache;
    const fs::path local = d.root / "x.MPQ";
    REQUIRE(cache.open(local.string(), r.bytes.size(), kChunk, "v1", r.fetcher()));
    r.broken = true;
    CHECK_FALSE(cache.ensure(0, 3 * kChunk));
    CHECK(cache.chunksPresent() == 0);
    r.broken = false;
    REQUIRE(cache.ensure(0, 3 * kChunk));
    CHECK(cache.chunksPresent() == 3);
    // And the bitmap on disk says the same after a restart.
    ChunkCache again;
    REQUIRE(again.open(local.string(), r.bytes.size(), kChunk, "v1", r.fetcher()));
    CHECK(again.chunksPresent() == 3);
}

TEST_CASE("A short or wrong-sized reply is not kept", "[chunks]") {
    Dir d;
    Remote r(8 * kChunk);
    ChunkCache cache;
    auto shortFetch = [](uint64_t, uint64_t len, std::vector<uint8_t>& out, std::string&) {
        out.assign(static_cast<size_t>(len - 1), 0);
        return true;
    };
    REQUIRE(cache.open((d.root / "s.MPQ").string(), r.bytes.size(), kChunk, "v1", shortFetch));
    CHECK_FALSE(cache.ensure(0, 10));
    CHECK(cache.chunksPresent() == 0);
}

TEST_CASE("Readers on several threads fetch each chunk once", "[chunks]") {
    Dir d;
    Remote r(256 * kChunk);
    ChunkCache cache;
    REQUIRE(cache.open((d.root / "t.MPQ").string(), r.bytes.size(), kChunk, "v1", r.fetcher()));
    std::vector<std::thread> threads;
    std::atomic<int> ok{0};
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 64; ++i) {
                const uint64_t at = static_cast<uint64_t>((i * 37 + t * 11) % 250) * kChunk + 17;
                if (cache.ensure(at, 3 * kChunk)) ++ok;
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(ok == 8 * 64);
    CHECK(r.sent <= r.bytes.size());  // no chunk fetched twice
    CHECK(r.sent == cache.chunksPresent() * kChunk);
}

TEST_CASE("Held bytes count the short last chunk, and a clear empties the cache", "[chunks]") {
    Dir d;
    Remote r(10 * kChunk + 100);   // the last chunk is 100 bytes
    const fs::path local = d.root / "common.MPQ";
    {
        ChunkCache cache;
        REQUIRE(cache.open(local.string(), r.bytes.size(), kChunk, "v1", r.fetcher()));
        CHECK(cache.bytesPresent() == 0);
        const uint64_t fetchesBefore = ChunkCache::threadFetchCount();
        REQUIRE(cache.ensure(0, 2 * kChunk));
        CHECK(ChunkCache::threadFetchCount() == fetchesBefore + 1);
        REQUIRE(cache.ensure(10 * kChunk, 100));
        CHECK(cache.bytesPresent() == 2 * kChunk + 100);
        // Already held: no fetch, so the counter stays.
        const uint64_t held = ChunkCache::threadFetchCount();
        REQUIRE(cache.ensure(10, 10));
        CHECK(ChunkCache::threadFetchCount() == held);

        REQUIRE(cache.clear());
        CHECK(cache.bytesPresent() == 0);
        CHECK(fs::file_size(local) == r.bytes.size());
    }
    // The clear is kept: a restart finds nothing and fetches again.
    const int before = r.requests;
    ChunkCache cache;
    REQUIRE(cache.open(local.string(), r.bytes.size(), kChunk, "v1", r.fetcher()));
    CHECK(cache.chunksPresent() == 0);
    REQUIRE(cache.ensure(0, kChunk));
    CHECK(r.requests == before + 1);
    CHECK(readLocal(local, 0, kChunk) ==
          std::vector<uint8_t>(r.bytes.begin(), r.bytes.begin() + kChunk));
}

TEST_CASE("The size limit drops what was read longest ago, to nine tenths", "[chunks]") {
    using wowee::pipeline::chooseUseCutoff;
    // Bytes held by the run that last read them; this is run 9.
    const std::map<uint32_t, uint64_t> byRun = {{0, 100}, {3, 300}, {5, 500}, {9, 300}};
    // 1200 held. No limit, or room enough: nothing.
    CHECK(chooseUseCutoff(byRun, 0, 9) == 0);
    CHECK(chooseUseCutoff(byRun, 1200, 9) == 0);
    // 1150 (target 1035): run 0's chunks -> 1100, still over; run 3's too -> 800.
    CHECK(chooseUseCutoff(byRun, 1150, 9) == 4);
    // 1000 (target 900): 0 and 3 go -> 800.
    CHECK(chooseUseCutoff(byRun, 1000, 9) == 4);
    // 700 (target 630): 0, 3 and 5 go -> 300.
    CHECK(chooseUseCutoff(byRun, 700, 9) == 6);
    // 100: what this run read stays all the same.
    CHECK(chooseUseCutoff(byRun, 100, 9) == 9);
    // Started anew (run 10): what the last start (9) read stays, even over.
    CHECK(chooseUseCutoff(byRun, 100, 10) == 9);
    CHECK(chooseUseCutoff({{2, 2000}}, 1000, 3) == 2);   // one start alone: nothing to drop
}

TEST_CASE("Chunks remember the run that read them; old ones are dropped", "[chunks]") {
    Dir d;
    Remote r(8 * kChunk + 10);
    const fs::path local = d.root / "patch-2.MPQ";
    {
        ChunkCache::setRun(1);
        ChunkCache cache;
        REQUIRE(cache.open(local.string(), r.bytes.size(), kChunk, "v1", r.fetcher()));
        REQUIRE(cache.ensure(0, 4 * kChunk));      // chunks 0-3 in run 1
        REQUIRE(cache.saveUse());
    }
    {
        ChunkCache::setRun(2);
        ChunkCache cache;
        REQUIRE(cache.open(local.string(), r.bytes.size(), kChunk, "v1", r.fetcher()));
        REQUIRE(cache.ensure(2 * kChunk, 1));      // chunk 2 read again in run 2
        REQUIRE(cache.ensure(8 * kChunk, 10));     // the short last chunk, run 2
        REQUIRE(cache.saveUse());
    }
    ChunkCache::setRun(3);
    ChunkCache cache;
    REQUIRE(cache.open(local.string(), r.bytes.size(), kChunk, "v1", r.fetcher()));
    std::map<uint32_t, uint64_t> byRun;
    cache.addBytesByLastUse(byRun);
    CHECK(byRun == std::map<uint32_t, uint64_t>{{1, 3 * kChunk}, {2, kChunk + 10}});
    // Drop what run 1 read last: chunks 0, 1, 3.
    CHECK(cache.dropReadBefore(2) == 3 * kChunk);
    CHECK(cache.chunksPresent() == 2);
    const int before = r.requests;
    REQUIRE(cache.ensure(2 * kChunk, kChunk));     // kept
    CHECK(r.requests == before);
    REQUIRE(cache.ensure(0, 1));                   // dropped: fetched again
    CHECK(r.requests == before + 1);
    CHECK(readLocal(local, 0, kChunk) ==
          std::vector<uint8_t>(r.bytes.begin(), r.bytes.begin() + kChunk));
    ChunkCache::setRun(1);
}

TEST_CASE("A loading screen drops older runs' chunks, never this run's", "[chunks][limit]") {
    using wowee::pipeline::chooseLoadCutoff;
    // This run is 9; it read 300 bytes so far.
    const std::map<uint32_t, uint64_t> byRun = {{3, 300}, {5, 500}, {9, 300}};
    CHECK(chooseLoadCutoff(byRun, 0, 9) == 0);
    CHECK(chooseLoadCutoff(byRun, 1300, 9) == 0);   // 1100 is within 1170
    CHECK(chooseLoadCutoff(byRun, 1150, 9) == 4);   // under the limit, over 1035: 300 go
    CHECK(chooseLoadCutoff(byRun, 1000, 9) == 4);   // 300 go -> 800 <= 900
    CHECK(chooseLoadCutoff(byRun, 700, 9) == 6);    // both older -> 300 <= 630
    // This run's own 300 are over a limit of 100: they stay, and it stops.
    CHECK(chooseLoadCutoff(byRun, 100, 9) == 9);
    // Unlike at start, the newest older run is no shield during play.
    CHECK(chooseLoadCutoff({{8, 2000}, {9, 10}}, 1000, 9) == 9);

    // On a cache that a reader of this run holds: run 1 read chunks 0-3,
    // run 2 (now) chunk 2; dropping before run 2 keeps chunk 2, and the
    // shared on-disk count follows.
    Dir d;
    Remote r(8 * kChunk);
    const fs::path local = d.root / "common.MPQ";
    ChunkCache::setRun(1);
    ChunkCache cache;
    REQUIRE(cache.open(local.string(), r.bytes.size(), kChunk, "v1", r.fetcher()));
    const uint64_t disk0 = ChunkCache::diskBytes();
    REQUIRE(cache.ensure(0, 4 * kChunk));
    CHECK(ChunkCache::diskBytes() == disk0 + 4 * kChunk);
    ChunkCache::setRun(2);
    REQUIRE(cache.ensure(2 * kChunk, 1));
    std::map<uint32_t, uint64_t> held;
    cache.addBytesByLastUse(held);
    REQUIRE(chooseLoadCutoff(held, 2 * kChunk, 2) == 2);
    CHECK(cache.dropReadBefore(2) == 3 * kChunk);
    CHECK(ChunkCache::diskBytes() == disk0 + kChunk);
    const int before = r.requests;
    REQUIRE(cache.ensure(2 * kChunk, kChunk));      // kept: no fetch
    CHECK(r.requests == before);
    REQUIRE(cache.ensure(0, 1));                    // dropped: fetched again
    CHECK(r.requests == before + 1);
    ChunkCache::setRun(1);
}

TEST_CASE("Past the disk limit, chunks are held in the view's memory only", "[chunks][limit]") {
    Dir d;
    Remote r(8 * kChunk);
    const fs::path local = d.root / "patch-3.MPQ";
    std::vector<uint8_t> view(r.bytes.size(), 0);   // the archive's private view
    const uint64_t mem0 = ChunkCache::memoryBytes();
    {
        ChunkCache cache;
        REQUIRE(cache.open(local.string(), r.bytes.size(), kChunk, "v1", r.fetcher()));
        cache.setView(view.data());
        // Room on disk for two chunks more than now.
        ChunkCache::setDiskLimit(ChunkCache::diskBytes() + 2 * kChunk);
        ChunkCache::setMemoryLimit(3 * kChunk);
        REQUIRE(cache.ensure(0, 2 * kChunk));           // to disk: fits
        CHECK(cache.chunksPresent() == 2);
        REQUIRE(cache.ensure(2 * kChunk, 2 * kChunk));  // past it: memory
        CHECK(cache.chunksPresent() == 2);              // not in the bitmap
        CHECK(cache.chunksInMemory() == 2);
        CHECK(ChunkCache::memoryBytes() == mem0 + 2 * kChunk);
        // Readable from the view, the server's bytes (its SHA-256 is the
        // fetcher's to check, as for any chunk); not written to the copy.
        CHECK(std::vector<uint8_t>(view.begin() + 2 * kChunk, view.begin() + 4 * kChunk) ==
              std::vector<uint8_t>(r.bytes.begin() + 2 * kChunk, r.bytes.begin() + 4 * kChunk));
        CHECK(readLocal(local, 2 * kChunk, kChunk) == std::vector<uint8_t>(kChunk, 0));
        // Held: reading it again fetches nothing.
        const int before = r.requests;
        REQUIRE(cache.ensure(3 * kChunk, 10));
        CHECK(r.requests == before);
        // The memory is bounded: the next chunk goes to disk after all.
        REQUIRE(cache.ensure(4 * kChunk, kChunk));      // the third in memory
        CHECK(cache.chunksInMemory() == 3);
        REQUIRE(cache.ensure(5 * kChunk, kChunk));      // memory full: disk
        CHECK(cache.chunksInMemory() == 3);
        CHECK(cache.chunksPresent() == 3);
        ChunkCache::setDiskLimit(0);
        ChunkCache::setMemoryLimit(256ull * 1024 * 1024);
    }
    CHECK(ChunkCache::memoryBytes() == mem0);
    // A restart: the memory-only chunks are not in the bitmap, so they are
    // fetched again; the disk ones are not.
    ChunkCache cache;
    REQUIRE(cache.open(local.string(), r.bytes.size(), kChunk, "v1", r.fetcher()));
    CHECK(cache.chunksPresent() == 3);
    const int before = r.requests;
    REQUIRE(cache.ensure(0, 2 * kChunk));
    CHECK(r.requests == before);
    REQUIRE(cache.ensure(2 * kChunk, 2 * kChunk));
    CHECK(r.requests == before + 1);
    CHECK(readLocal(local, 2 * kChunk, kChunk) ==
          std::vector<uint8_t>(r.bytes.begin() + 2 * kChunk, r.bytes.begin() + 3 * kChunk));
}
