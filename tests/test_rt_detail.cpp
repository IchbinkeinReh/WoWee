// The textures a traced hit is coloured by (rt_detail.hpp): the pyramid the
// trace shaders read (rt_trace_detail.glsli), and the store that shares the
// pyramids between meshes and frees them with the last.
#include "rendering/rt_detail.hpp"

#include <catch_amalgamated.hpp>

#include <array>
#include <cmath>

using namespace wowee::rendering;

namespace {

uint32_t rgba(int r, int g, int b, int a = 255) {
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
}

std::array<uint32_t, 64 * 64> filled(uint32_t c) {
    std::array<uint32_t, 64 * 64> t;
    t.fill(c);
    return t;
}

std::vector<uint32_t> slotOf(uint32_t v) { return std::vector<uint32_t>(rt_detail::kSlotWords, v); }

}  // namespace

TEST_CASE("rt_detail: the levels follow one another, the whole fits a slot", "[rt_detail]") {
    CHECK(rt_detail::levelOffset(0) == 0);
    CHECK(rt_detail::levelOffset(1) == 4096);
    CHECK(rt_detail::levelOffset(2) == 4096 + 1024);
    CHECK(rt_detail::levelOffset(6) == rt_detail::kTexels - 1);
    CHECK(rt_detail::kSlotWords * 2 >= rt_detail::kTexels);
    CHECK(rt_detail::kSlotWords % 16 == 0);
}

TEST_CASE("rt_detail: RGB565 keeps a colour to its steps", "[rt_detail]") {
    const auto c = rt_detail::unpackRgb565(rt_detail::packRgb565(0.25f, 0.5f, 0.75f));
    CHECK(c[0] == Catch::Approx(0.25f).margin(0.5f / 31.0f + 1e-4f));
    CHECK(c[1] == Catch::Approx(0.5f).margin(0.5f / 63.0f + 1e-4f));
    CHECK(c[2] == Catch::Approx(0.75f).margin(0.5f / 31.0f + 1e-4f));
    CHECK(rt_detail::unpackRgb565(rt_detail::packRgb565(1.0f, 1.0f, 1.0f))[1] == 1.0f);
}

TEST_CASE("rt_detail: a plain texture is the same at every level", "[rt_detail]") {
    const auto p = rt_detail::buildPyramid(filled(rgba(255, 0, 255)), {1.0f, 0.0f, 1.0f});
    REQUIRE(p.size() == rt_detail::kSlotWords);
    for (int l = 0; l < rt_detail::kLevels; ++l) {
        const int side = rt_detail::kSide >> l;
        const auto t = rt_detail::texel(p, l, side - 1, side / 2);
        CHECK(t[0] == 1.0f);
        CHECK(t[1] == 0.0f);
        CHECK(t[2] == 1.0f);
    }
}

TEST_CASE("rt_detail: each level is the mean of the one above, the last the given mean", "[rt_detail]") {
    // Black and white columns a texel wide: grey from the first mip down.
    std::array<uint32_t, 64 * 64> stripes;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) stripes[size_t(y) * 64 + size_t(x)] = (x & 1) ? rgba(255, 255, 255) : rgba(0, 0, 0);
    const auto p = rt_detail::buildPyramid(stripes, {0.1f, 0.2f, 0.3f});
    CHECK(rt_detail::texel(p, 0, 0, 5)[0] == 0.0f);
    CHECK(rt_detail::texel(p, 0, 1, 5)[0] == 1.0f);
    for (int l = 1; l < 6; ++l) CHECK(rt_detail::texel(p, l, 0, 0)[1] == Catch::Approx(0.5f).margin(0.02f));
    // The 1x1 level is the visible mean the triangle's colour was made of.
    const auto m = rt_detail::texel(p, 6, 0, 0);
    CHECK(m[0] == Catch::Approx(0.1f).margin(0.02f));
    CHECK(m[1] == Catch::Approx(0.2f).margin(0.02f));
    CHECK(m[2] == Catch::Approx(0.3f).margin(0.02f));
}

TEST_CASE("rt_detail: alpha does not darken or brighten the colour", "[rt_detail]") {
    // A terrain tileset's alpha is its highlight's mask: the colour is kept.
    const auto p = rt_detail::buildPyramid(filled(rgba(0, 255, 0, 0)), {0.0f, 1.0f, 0.0f});
    CHECK(rt_detail::texel(p, 0, 3, 3)[1] == 1.0f);
    CHECK(rt_detail::texel(p, 3, 3, 3)[1] == 1.0f);
}

TEST_CASE("RtSlotStore: the same content is stored once", "[rt_detail]") {
    RtSlotStore store(rt_detail::kSlotWords);
    const auto a = slotOf(1), b = slotOf(2);
    const uint32_t sa = store.add(a.data());
    CHECK(store.add(a.data()) == sa);
    const uint32_t sb = store.add(b.data());
    CHECK(sb != sa);
    CHECK(store.live() == 2);
    CHECK(store.slot(sb)[7] == 2u);
    CHECK(store.bytes() == 2ull * rt_detail::kSlotWords * 4);
}

TEST_CASE("RtSlotStore: a slot goes with its last reference and is taken again", "[rt_detail]") {
    RtSlotStore store(16);
    std::vector<uint32_t> a(16, 7), b(16, 9);
    const uint32_t s = store.add(a.data());
    store.acquire(s);
    store.acquire(s);
    store.release(s);
    CHECK(store.live() == 1);
    store.release(s);
    CHECK(store.live() == 0);
    // Released past zero: nothing happens.
    store.release(s);
    CHECK(store.live() == 0);
    // The freed slot is the next one used, and the old content is not found.
    const uint32_t t = store.add(b.data());
    CHECK(t == s);
    CHECK(store.capacity() == 1);
    CHECK(store.slot(t)[0] == 9u);
    const uint32_t u = store.add(a.data());
    CHECK(u != t);
}

TEST_CASE("RtSlotStore: one added and never taken is freed by collect", "[rt_detail]") {
    RtSlotStore store(16);
    std::vector<uint32_t> a(16, 1), b(16, 2);
    const uint32_t sa = store.add(a.data());
    const uint32_t sb = store.add(b.data());
    store.acquire(sb);
    store.collect();
    CHECK(store.live() == 1);
    CHECK(store.refs(sb) == 1);
    CHECK(store.refs(sa) == 0);
    // A taken slot survives later collects.
    store.collect();
    CHECK(store.live() == 1);
}

TEST_CASE("RtSlotStore: what changed is reported once, and all of it on request", "[rt_detail]") {
    RtSlotStore store(16);
    std::vector<uint32_t> a(16, 1), b(16, 2);
    const uint32_t sa = store.add(a.data());
    const uint32_t sb = store.add(b.data());
    auto d = store.takeDirty();
    CHECK(d.size() == 2);
    CHECK(store.takeDirty().empty());
    store.markAllDirty();
    d = store.takeDirty();
    CHECK(d.size() == 2);
    CHECK(((d[0] == sa && d[1] == sb) || (d[0] == sb && d[1] == sa)));
}
