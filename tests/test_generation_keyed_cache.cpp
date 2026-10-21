// What is made over the scene's targets is not found again once they are
// rebuilt, whatever handles the new ones are given
// (rendering/generation_keyed_cache.hpp). The ray traced lighting's G-buffer
// framebuffer was found by view handles the driver had handed out again for
// new FXAA targets, and the pass wrote into the destroyed ones.
#include <catch_amalgamated.hpp>

#include "rendering/generation_keyed_cache.hpp"

#include <algorithm>
#include <utility>

using wowee::rendering::GenerationKeyedCache;

namespace {
// Stand-ins for VkImageView and VkFramebuffer handles.
using View = uint64_t;
using Framebuffer = int;
using Key = std::pair<View, View>;
}  // namespace

TEST_CASE("within one generation a key finds what was made for it", "[generation_cache]") {
    GenerationKeyedCache<Key, Framebuffer> cache;
    CHECK(cache.advance(1).empty());
    cache.insert({0x10, 0x20}, 7);
    REQUIRE(cache.find({0x10, 0x20}) != nullptr);
    CHECK(*cache.find({0x10, 0x20}) == 7);
    CHECK(cache.find({0x10, 0x21}) == nullptr);
    // The same generation again changes nothing.
    CHECK(cache.advance(1).empty());
    CHECK(cache.size() == 1);
}

TEST_CASE("a recycled handle after a rebuild does not find the old framebuffer", "[generation_cache]") {
    // FXAA on: its targets' views are 0x10 and 0x20, a framebuffer is made.
    GenerationKeyedCache<Key, Framebuffer> cache;
    (void)cache.advance(1);
    cache.insert({0x10, 0x20}, 7);

    // FXAA off and on again: the targets are destroyed and made anew, and the
    // driver hands the new views out at the old values. The rebuild moved the
    // generation, so the old framebuffer comes back to be released...
    const auto released = cache.advance(2);
    REQUIRE(released.size() == 1);
    CHECK(released.front() == 7);
    // ...and the same handles find nothing: a framebuffer is made over the
    // new images instead of drawing into the destroyed ones.
    CHECK(cache.find({0x10, 0x20}) == nullptr);
    CHECK(cache.size() == 0);
}

TEST_CASE("everything made under the old generation is handed back once", "[generation_cache]") {
    GenerationKeyedCache<View, Framebuffer> cache;
    (void)cache.advance(5);
    cache.insert(0x1, 1);
    cache.insert(0x2, 2);
    cache.insert(0x3, 3);
    auto released = cache.advance(6);
    std::sort(released.begin(), released.end());
    CHECK(released == std::vector<Framebuffer>{1, 2, 3});
    CHECK(cache.advance(6).empty());
    CHECK(cache.generation() == 6);
}

TEST_CASE("takeAll empties the cache without moving the generation", "[generation_cache]") {
    GenerationKeyedCache<View, Framebuffer> cache;
    (void)cache.advance(3);
    cache.insert(0x1, 1);
    CHECK(cache.takeAll() == std::vector<Framebuffer>{1});
    CHECK(cache.size() == 0);
    CHECK(cache.generation() == 3);
}
