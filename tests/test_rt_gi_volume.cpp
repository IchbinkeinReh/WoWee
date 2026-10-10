// The probe volume's layout (rt_gi_volume.hpp), which rt_gi.glsli and the
// probe passes mirror. Nothing on screen says when one of these is off: a
// probe stored in one slot and read from another, or a border texel copying
// the wrong side of its tile, only shows as light in the wrong place, faintly.
#include <catch_amalgamated.hpp>
#include "rendering/rt_gi_volume.hpp"

#include <glm/glm.hpp>

#include <set>

using namespace wowee::rendering::gi;

TEST_CASE("a probe's storage slot does not change as the grid scrolls", "[gi]") {
    const glm::ivec3 abs(1234, -567, 89);
    const glm::ivec3 s = storageOf(abs);
    for (const glm::ivec3 gridMin : {abs - kProbes / 2, abs - glm::ivec3(0), abs - kProbes + 1,
                                     abs - glm::ivec3(3, 30, 15)}) {
        REQUIRE(inside(abs, gridMin));
        CHECK(absoluteOf(s, gridMin) == abs);
    }
}

TEST_CASE("every slot stands for exactly one probe of the grid", "[gi]") {
    const glm::ivec3 gridMin(-5917, 2001, -13);
    std::set<std::tuple<int, int, int>> seen;
    for (uint32_t slot = 0; slot < static_cast<uint32_t>(kProbesPerCascade); ++slot) {
        const glm::ivec3 s = storageOfSlot(slot);
        REQUIRE(slotIndex(s) == slot);
        const glm::ivec3 abs = absoluteOf(s, gridMin);
        REQUIRE(inside(abs, gridMin));
        REQUIRE(storageOf(abs) == s);
        seen.insert({abs.x, abs.y, abs.z});
    }
    CHECK(seen.size() == static_cast<size_t>(kProbesPerCascade));
}

TEST_CASE("the grid follows the camera, but not across a single cell", "[gi]") {
    const float spacing = 3.0f;
    const glm::vec3 cam(-8913.0f, 554.0f, 94.0f);
    const glm::ivec3 first = scrolledGridMin(cam, spacing, {}, false);
    // Centred: the camera's cell is the middle probe.
    CHECK(glm::ivec3(glm::floor(cam / spacing)) - first == kProbes / 2);

    // A step across a cell boundary and back moves nothing.
    CHECK(scrolledGridMin(cam + glm::vec3(spacing, 0, 0), spacing, first, true) == first);
    CHECK(scrolledGridMin(cam - glm::vec3(0, spacing, spacing), spacing, first, true) == first);
    // Two cells and it recentres on that axis alone.
    const glm::ivec3 moved = scrolledGridMin(cam + glm::vec3(2.5f * spacing, 0, 0), spacing, first, true);
    CHECK(moved.x == first.x + 2);
    CHECK(moved.y == first.y);
    CHECK(moved.z == first.z);
}

TEST_CASE("the update list holds every new probe and a share of the rest", "[gi]") {
    const glm::ivec3 prev(100, 200, 10);
    std::vector<uint32_t> list;

    SECTION("nothing before: every probe, all of them reset") {
        appendUpdateList(list, 1, prev, prev, false, 4, 7);
        REQUIRE(list.size() == static_cast<size_t>(kProbesPerCascade));
        for (uint32_t e : list) {
            CHECK((e & kListReset) != 0);
            CHECK(((e >> kListCascadeShift) & 0xF) == 1u);
        }
    }
    SECTION("standing still: one slot in `period`, none reset") {
        appendUpdateList(list, 0, prev, prev, true, 4, 7);
        CHECK(list.size() == static_cast<size_t>(kProbesPerCascade / 4));
        for (uint32_t e : list) {
            CHECK((e & kListReset) == 0);
            CHECK((e & kListSlotMask) % 4 == 7 % 4);
        }
    }
    SECTION("a plane further: that plane reset, in the slots the old far plane had") {
        const glm::ivec3 next = prev + glm::ivec3(1, 0, 0);
        appendUpdateList(list, 0, next, prev, true, 4, 0);
        size_t resets = 0;
        for (uint32_t e : list) {
            if ((e & kListReset) == 0) continue;
            ++resets;
            const glm::ivec3 abs = absoluteOf(storageOfSlot(e & kListSlotMask), next);
            CHECK(abs.x == next.x + kProbes.x - 1);
            CHECK(abs.x == prev.x + kProbes.x);
            CHECK(storageOf(abs).x == storageOf(prev).x);
        }
        CHECK(resets == static_cast<size_t>(kProbes.y * kProbes.z));
    }
}

TEST_CASE("tiles do not overlap and stay in the atlas", "[gi]") {
    // The last probe's tiles end at the atlas's edge.
    const glm::ivec3 last = kProbes - 1;
    const glm::ivec2 irr = irradianceTileOrigin(kCascades - 1, last) + kIrradianceTile;
    const glm::ivec2 vis = visibilityTileOrigin(kCascades - 1, last) + kVisibilityTile;
    CHECK(irr.x <= kAtlasWidth);
    CHECK(irr.y <= visibilityTileOrigin(kCascades - 1, {0, 0, 0}).y);
    CHECK(vis.x == kAtlasWidth);
    CHECK(vis.y == kAtlasHeight);
    // Neighbouring probes' tiles sit side by side.
    CHECK(irradianceTileOrigin(0, {1, 0, 0}).x - irradianceTileOrigin(0, {0, 0, 0}).x == kIrradianceTile);
    CHECK(visibilityTileOrigin(0, {0, 1, 0}).y - visibilityTileOrigin(0, {0, 0, 0}).y == kVisibilityTile);
    CHECK(visibilityTileOrigin(0, {0, 0, 1}).x - visibilityTileOrigin(0, {kProbes.x - 1, 0, 0}).x ==
          kVisibilityTile);
    CHECK(irradianceTileOrigin(1, {0, 0, 0}).y == kBandHeight);
}

TEST_CASE("octahedral directions round-trip", "[gi]") {
    for (int i = 0; i < 200; ++i) {
        const glm::vec3 d = fibonacciDirection(i, 200);
        CHECK(glm::length(d) == Catch::Approx(1.0f).margin(1e-5));
        const glm::vec3 back = octDecode(octEncode(d));
        CHECK(glm::dot(back, d) > 0.99999f);
    }
}

namespace {

// The octahedral square continued past its edge: mirrored across the edge
// crossed, which is the neighbour on the sphere.
glm::vec2 wrapOct(glm::vec2 e) {
    if (e.x > 1.0f) e = {2.0f - e.x, -e.y};
    else if (e.x < -1.0f) e = {-2.0f - e.x, -e.y};
    if (e.y > 1.0f) e = {-e.x, 2.0f - e.y};
    else if (e.y < -1.0f) e = {-e.x, -2.0f - e.y};
    return e;
}

}  // namespace

TEST_CASE("a border texel copies its neighbour on the sphere", "[gi]") {
    for (int texels : {kIrradianceTexels, kVisibilityTexels}) {
        for (int y = -1; y <= texels; ++y) {
            for (int x = -1; x <= texels; ++x) {
                const glm::ivec2 t(x, y);
                const glm::ivec2 src = borderSource(t, texels);
                REQUIRE(src.x >= 0);
                REQUIRE(src.y >= 0);
                REQUIRE(src.x < texels);
                REQUIRE(src.y < texels);
                const bool border = x < 0 || y < 0 || x >= texels || y >= texels;
                if (!border) {
                    CHECK(src == t);
                    continue;
                }
                // The direction the border texel's centre would have on the
                // continued map, against the interior texel nearest to it.
                const glm::vec2 e = (glm::vec2(t) + 0.5f) / static_cast<float>(texels) * 2.0f - 1.0f;
                const glm::vec3 want = octDecode(wrapOct(e));
                float best = -2.0f;
                glm::ivec2 nearest(0);
                for (int j = 0; j < texels; ++j) {
                    for (int i = 0; i < texels; ++i) {
                        const float c = glm::dot(texelDirection({i, j}, texels), want);
                        if (c > best) {
                            best = c;
                            nearest = {i, j};
                        }
                    }
                }
                INFO("texels " << texels << " border (" << x << ", " << y << ") copies (" << src.x
                               << ", " << src.y << "), nearest (" << nearest.x << ", " << nearest.y
                               << ")");
                CHECK(glm::dot(texelDirection(src, texels), want) >= best - 1e-4f);
            }
        }
    }
}

TEST_CASE("list entries carry slot, cascade and reset apart", "[gi]") {
    const uint32_t e = packListEntry(static_cast<uint32_t>(kProbesPerCascade - 1), 1, true);
    CHECK((e & kListSlotMask) == static_cast<uint32_t>(kProbesPerCascade - 1));
    CHECK(((e >> kListCascadeShift) & 0xF) == 1u);
    CHECK((e & kListReset) != 0);
    // What the shaders take apart with 0xFFFFF and bit 31.
    CHECK(kListSlotMask == 0xFFFFFu);
    CHECK(kListReset == 0x80000000u);
}
