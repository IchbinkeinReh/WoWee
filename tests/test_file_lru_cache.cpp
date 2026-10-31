// The asset manager's file cache: least recently read out first.
//
// It was first in, first out - a hit never counted as a use - so the files a
// city reads every few seconds were the first dropped once the cache filled.
#include <catch_amalgamated.hpp>

#include <string>
#include <vector>

#include "pipeline/file_lru_cache.hpp"

using wowee::pipeline::FileLruCache;

namespace {
std::vector<uint8_t> bytes(size_t n, uint8_t v = 1) { return std::vector<uint8_t>(n, v); }
}  // namespace

TEST_CASE("A read keeps a file from being the next one evicted", "[file_lru_cache]") {
    FileLruCache cache;
    const size_t budget = 300;
    cache.insert("a", bytes(100), budget);
    cache.insert("b", bytes(100), budget);
    cache.insert("c", bytes(100), budget);
    std::vector<uint8_t> out;
    REQUIRE(cache.lookup("a", out));  // a is now the most recent
    cache.insert("d", bytes(100), budget);  // evicts b, the least recent
    CHECK(cache.contains("a"));
    CHECK_FALSE(cache.contains("b"));
    CHECK(cache.contains("c"));
    CHECK(cache.contains("d"));
    CHECK(cache.bytes() == 300);
}

TEST_CASE("Eviction frees as many as the new file needs", "[file_lru_cache]") {
    FileLruCache cache;
    const size_t budget = 350;
    cache.insert("a", bytes(100), budget);
    cache.insert("b", bytes(100), budget);
    cache.insert("c", bytes(100), budget);
    cache.insert("big", bytes(170), budget);  // needs room for 170: a and b go
    CHECK_FALSE(cache.contains("a"));
    CHECK_FALSE(cache.contains("b"));
    CHECK(cache.contains("c"));
    CHECK(cache.contains("big"));
    CHECK(cache.bytes() == 270);
}

TEST_CASE("A lookup returns the bytes and a miss returns false", "[file_lru_cache]") {
    FileLruCache cache;
    cache.insert("a", bytes(10, 7), 100);
    std::vector<uint8_t> out;
    REQUIRE(cache.lookup("a", out));
    CHECK(out == bytes(10, 7));
    CHECK_FALSE(cache.lookup("missing", out));
}

TEST_CASE("Files of half the budget or more, empty files and repeats are not cached", "[file_lru_cache]") {
    FileLruCache cache;
    cache.insert("half", bytes(50), 100);
    cache.insert("empty", {}, 100);
    CHECK(cache.size() == 0);
    cache.insert("a", bytes(10), 100);
    cache.insert("a", bytes(20), 100);  // a second insert of the same file is ignored
    CHECK(cache.size() == 1);
    CHECK(cache.bytes() == 10);
    cache.clear();
    CHECK(cache.size() == 0);
    CHECK(cache.bytes() == 0);
}
