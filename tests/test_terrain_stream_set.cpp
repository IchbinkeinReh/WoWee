// Which tiles are streamed in about a place (rendering/terrain_stream_set.hpp):
// by the place alone, the far clip's square about it (0x00780860, 0x007b5950);
// and that nothing in sight is then left to neither the ground nor the .wdl
// horizon (pipeline::wdl::tileNear).
#include <catch_amalgamated.hpp>

#include "pipeline/wdl_loader.hpp"
#include "rendering/terrain_stream_set.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <set>
#include <tuple>
#include <utility>

using namespace wowee::rendering::terrain_stream;

namespace {

// Red Cloud Mesa, where a turn of the camera opened the hole (server x, y are
// render x, y): .go xyz -2977.852 -411.907 50.844 1.
constexpr float kMesaX = -2977.852f;
constexpr float kMesaY = -411.907f;
constexpr float kMesaZ = 52.5f;

std::set<std::pair<int, int>> asSet(const std::vector<std::pair<int, int>>& v) {
    return {v.begin(), v.end()};
}

/// The camera's eye for the focus, a zoom and a turn, as the camera
/// controller orbits it.
glm::vec3 eyeFor(const glm::vec3& focus, float distance, float yawDeg, float pitchDeg) {
    const float y = glm::radians(yawDeg), p = glm::radians(pitchDeg);
    const glm::vec3 fwd(std::cos(y) * std::cos(p), std::sin(y) * std::cos(p), std::sin(p));
    return focus - fwd * distance;
}

/// A point in sight: where it is, and its depth along the view.
struct Sight {
    glm::vec3 at;
    float depth;
};

/// Points of the view frustum out to the far clip's depth, for an eye looking
/// at yaw/pitch, from 100 yards under the eye to 400 over it - any ground in
/// sight is among them.
std::vector<Sight> pointsInView(const glm::vec3& eye, float yawDeg, float pitchDeg, float farClip,
                                float vfovDeg = 70.0f, float aspect = 1918.0f / 1001.0f) {
    const float y = glm::radians(yawDeg), p = glm::radians(pitchDeg);
    const glm::vec3 f(std::cos(y) * std::cos(p), std::sin(y) * std::cos(p), std::sin(p));
    const glm::vec3 r = glm::normalize(glm::cross(f, glm::vec3(0, 0, 1)));
    const glm::vec3 u = glm::cross(r, f);
    const float tv = std::tan(glm::radians(vfovDeg * 0.5f)), th = tv * aspect;
    std::vector<Sight> out;
    for (int i = -16; i <= 16; ++i) {
        for (int j = -16; j <= 16; ++j) {
            const glm::vec3 dir = glm::normalize(f + r * (th * static_cast<float>(i) / 16.0f) +
                                                 u * (tv * static_cast<float>(j) / 16.0f));
            const float perDepth = 1.0f / glm::dot(dir, f);  // ray length per yard of depth
            for (int k = 1; k <= 48; ++k) {
                const float depth = farClip * static_cast<float>(k) / 48.0f;
                const glm::vec3 at = eye + dir * (depth * perDepth);
                if (at.z < eye.z - 100.0f || at.z > eye.z + 400.0f) continue;
                out.push_back({at, depth});
            }
        }
    }
    return out;
}

std::pair<int, int> tileOf(const glm::vec3& at) {
    return {static_cast<int>(std::floor(32.0f - at.y / kTileSize)),
            static_cast<int>(std::floor(32.0f - at.x / kTileSize))};
}

}  // namespace

TEST_CASE("the load set is a square of tiles about the point, nearest first", "[terrain_stream]") {
    const TileRect r = tilesWithin(kMesaX, kMesaY, loadReach(791.6667f));
    // The mesa stands in tile (32, 37).
    CHECK(r.contains(32, 37));
    const auto order = loadOrder(kMesaX, kMesaY, loadReach(791.6667f));
    REQUIRE(!order.empty());
    CHECK(order.front() == std::make_pair(32, 37));
    CHECK(static_cast<int>(order.size()) == (r.xMax - r.xMin + 1) * (r.yMax - r.yMin + 1));
    for (size_t i = 1; i < order.size(); ++i) {
        CHECK(distanceToTile(order[i - 1].first, order[i - 1].second, kMesaX, kMesaY) <=
              distanceToTile(order[i].first, order[i].second, kMesaX, kMesaY));
    }
}

TEST_CASE("the same place loads the same tiles whatever the camera's yaw and pitch", "[terrain_stream]") {
    // What is streamed is asked of the focus the camera turns about, not of
    // the eye, which orbits it: two yards from a tile's edge the eye at a
    // zoom of 6.55 is on one side of it or the other by the yaw alone, and
    // a set taken about the eye changed with the turn.
    const float reach = loadReach(791.6667f);
    // The square's north edge two yards short of a tile's edge.
    const glm::vec3 focus(kMesaX, 2.0f * kTileSize - reach - 2.0f, kMesaZ);
    const auto ref = loadOrder(focus.x, focus.y, reach);
    std::set<TileRect, bool (*)(const TileRect&, const TileRect&)> eyeSets(
        [](const TileRect& a, const TileRect& b) {
            return std::tie(a.xMin, a.xMax, a.yMin, a.yMax) < std::tie(b.xMin, b.xMax, b.yMin, b.yMax);
        });
    for (float yaw = 0.0f; yaw < 360.0f; yaw += 7.5f) {
        for (float pitch : {-60.0f, -16.74f, 0.0f, 30.0f}) {
            const glm::vec3 eye = eyeFor(focus, 6.55f, yaw, pitch);
            eyeSets.insert(tilesWithin(eye.x, eye.y, reach));
            CHECK(loadOrder(focus.x, focus.y, reach) == ref);
        }
    }
    CHECK(eyeSets.size() > 1);
}

TEST_CASE("the far clip's square about the eye is loaded, however the camera orbits", "[terrain_stream]") {
    // The client's guarantee (0x00780860): every tile within the far clip of
    // the eye, on either axis. The square is taken about the focus, with the
    // eye's orbit added to it, so that holds at every yaw, pitch and zoom.
    const glm::vec3 focus(kMesaX, kMesaY, kMesaZ);
    for (const float farClip : {183.4f, 400.0f, 791.6667f, 1277.0f, 1583.3334f}) {
        const auto loaded = asSet(loadOrder(focus.x, focus.y, loadReach(farClip)));
        for (float yaw = 0.0f; yaw < 360.0f; yaw += 10.0f) {
            for (float pitch : {-45.0f, -16.74f, 0.0f, 20.0f}) {
                for (float zoom : {0.0f, 6.55f, 30.0f, 50.0f}) {
                    const glm::vec3 eye = eyeFor(focus, zoom, yaw, pitch);
                    const TileRect need = tilesWithin(eye.x, eye.y, farClip);
                    for (int y = need.yMin; y <= need.yMax; ++y) {
                        for (int x = need.xMin; x <= need.xMax; ++x) {
                            INFO("far clip " << farClip << " yaw " << yaw << " pitch " << pitch << " zoom "
                                             << zoom << " tile " << x << "," << y);
                            CHECK(loaded.count({x, y}) == 1);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("nothing in sight is left to neither the ground nor the horizon", "[terrain_stream]") {
    // Before the far clip a point is drawn by its tile's ground if that is
    // loaded, else by the horizon tile, which is drawn from its near plane on.
    // At the screen's top corners on the mesa the frustum reaches ground 1300
    // yards off, past the load square: the horizon's near plane of the far
    // clip less 50 left a hole there, which a turn of the camera opened and
    // closed (user 175/176). Drawn from in front of the camera where the
    // ground is not in, it leaves none.
    const glm::vec3 focus(kMesaX, kMesaY, kMesaZ);
    const float farClip = 791.6667f;
    const auto loaded = asSet(loadOrder(focus.x, focus.y, loadReach(farClip)));
    int heldByHorizon = 0, holesOld = 0;
    for (float yaw = 0.0f; yaw < 360.0f; yaw += 5.0f) {
        for (float pitch : {-16.74f, 0.0f}) {
            const glm::vec3 eye = eyeFor(focus, 6.55f, yaw, pitch);
            for (const Sight& s : pointsInView(eye, yaw, pitch, farClip)) {
                if (s.depth < 2.0f) continue;
                const bool ground = loaded.count(tileOf(s.at)) == 1;
                // Its ground in, the ground draws it within its cube and the
                // horizon past that (test_horizon_coverage.cpp).
                if (ground) continue;
                ++heldByHorizon;
                CHECK(s.depth >= wowee::pipeline::wdl::tileNear(false, farClip));
                if (s.depth < wowee::pipeline::wdl::horizonNear(farClip)) ++holesOld;
            }
        }
    }
    CHECK(heldByHorizon > 0);  // the case arises
    CHECK(holesOld > 0);       // and the far clip's near plane left it a hole
    // With its ground in, a tile is the horizon past the far clip, as the client's.
    CHECK(wowee::pipeline::wdl::tileNear(true, farClip) == wowee::pipeline::wdl::horizonNear(farClip));
}

TEST_CASE("the circle the load set replaced did not hold the far clip's square", "[terrain_stream]") {
    // The old rule: a circle of radius ceil(far clip / tile) + 1 tiles about
    // the eye's tile. At a far clip of 1277 (Outland) its corners miss
    // tiles of the far clip's square about the eye.
    bool missed = false;
    for (const float farClip : {791.6667f, 1277.0f, 1583.3334f}) {
        const int radius = static_cast<int>(std::ceil(farClip / kTileSize)) + 1;
        for (float yaw = 0.0f; yaw < 360.0f; yaw += 30.0f) {
            const glm::vec3 eye = eyeFor({kMesaX, kMesaY, kMesaZ}, 6.55f, yaw, -16.74f);
            const auto [cx, cy] = tileOf(eye);
            const TileRect need = tilesWithin(eye.x, eye.y, farClip);
            for (int y = need.yMin; y <= need.yMax; ++y)
                for (int x = need.xMin; x <= need.xMax; ++x)
                    if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > radius * radius) missed = true;
        }
    }
    CHECK(missed);
}

TEST_CASE("a loaded tile is kept until it is a tile past the load square", "[terrain_stream]") {
    const float reach = loadReach(791.6667f);
    const TileRect load = tilesWithin(kMesaX, kMesaY, reach);
    const TileRect keep = tilesWithin(kMesaX, kMesaY, unloadReach(reach));
    CHECK(keep.xMin <= load.xMin);
    CHECK(keep.xMax >= load.xMax);
    CHECK(keep.yMin <= load.yMin);
    CHECK(keep.yMax >= load.yMax);
    // A step over a tile's edge does not let the tile just left go.
    const TileRect afterStep = tilesWithin(kMesaX - 400.0f, kMesaY, unloadReach(reach));
    for (int y = load.yMin; y <= load.yMax; ++y)
        for (int x = load.xMin; x <= load.xMax; ++x) CHECK(afterStep.contains(x, y));
}

TEST_CASE("the square is held to the map's 64 x 64 tiles", "[terrain_stream]") {
    const TileRect r = tilesWithin(17000.0f, -17000.0f, loadReach(1583.3334f));
    CHECK(r.xMin >= 0);
    CHECK(r.xMax <= 63);
    CHECK(r.yMin >= 0);
    CHECK(r.yMax <= 63);
}
