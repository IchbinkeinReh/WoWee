// Which tiles are streamed in about a place (rendering/terrain_stream_set.hpp):
// by the place alone, the far clip's square about it (0x00780860, 0x007b5950);
// and that nothing in sight is then left to neither the ground nor the .wdl
// horizon (pipeline::wdl::tileNear).
#include <catch_amalgamated.hpp>

#include "pipeline/wdl_loader.hpp"
#include "rendering/terrain_stream_set.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

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

TEST_CASE("a unit is held only over a tile that is still to come", "[terrain_stream]") {
    CHECK(tileComing(true, false, false));
    // In: what is under it now is all there is, and under the ground there is
    // nothing.
    CHECK_FALSE(tileComing(true, true, false));
    // None there: open sea, or a map of one building.
    CHECK_FALSE(tileComing(true, false, true));
    CHECK_FALSE(tileComing(false, false, false));
}

// On a flight path at ten frames a second the workers read the whole square of
// tiles ahead of the main thread, and all of them were in memory at once.
TEST_CASE("few tiles are read ahead of a slow main thread", "[terrain_stream]") {
    constexpr size_t kWorkers = 8;
    size_t toRead = 300, waiting = 0, puttingIn = 0, done = 0, mostHeld = 0;
    std::vector<int> reading;  // ticks left of each worker's tile
    int putTicks = 0;
    for (int tick = 0; tick < 100000 && done < 300; ++tick) {
        for (int& left : reading) --left;
        for (size_t i = reading.size(); i-- > 0;)
            if (reading[i] <= 0) {
                reading.erase(reading.begin() + static_cast<std::ptrdiff_t>(i));
                ++waiting;
            }
        while (reading.size() < kWorkers && toRead > 0 && mayPrepare(waiting)) {
            reading.push_back(3);
            --toRead;
        }
        // The main thread, every tick a frame: what it takes, then its work.
        while (takesReady(puttingIn) && waiting > 0) {
            --waiting;
            ++puttingIn;
        }
        if (puttingIn > 0 && ++putTicks >= 20) {
            putTicks = 0;
            --puttingIn;
            ++done;
        }
        mostHeld = std::max(mostHeld, waiting + puttingIn + reading.size());
    }
    CHECK(done == 300);
    CHECK(mostHeld <= kReadyTiles + kWorkers);
}

// ---- which queued tile is read next (streaming from a server) --------------

namespace {
/// The centre of tile (x, y) in render coordinates.
glm::vec2 tileCentre(int x, int y) {
    return {(31.5f - static_cast<float>(y)) * kTileSize, (31.5f - static_cast<float>(x)) * kTileSize};
}
size_t pick(const std::vector<std::pair<int, int>>& queue, glm::vec2 focus, glm::vec2 heading) {
    return nextToRead(queue.size(), [&](size_t i) { return queue[i]; }, focus.x, focus.y, heading.x, heading.y);
}
}  // namespace

TEST_CASE("the tile under the focus is read first, then the nearest", "[terrain_stream][read_order]") {
    const glm::vec2 focus = tileCentre(30, 40) + glm::vec2(100.0f, -50.0f);
    const auto [ux, uy] = tileOf(glm::vec3(focus, 0.0f));
    std::vector<std::pair<int, int>> queue = {{ux + 3, uy}, {ux - 1, uy + 1}, {ux, uy}, {ux + 1, uy}};
    for (const glm::vec2 heading : {glm::vec2(0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(-0.6f, 0.8f)}) {
        CHECK(queue[pick(queue, focus, heading)] == std::make_pair(ux, uy));
    }
    queue.erase(queue.begin() + 2);
    // Without a heading the nearest square, whatever its place in the queue.
    const size_t next = pick(queue, focus, glm::vec2(0.0f));
    for (const auto& [x, y] : queue) {
        CHECK(distanceToTile(queue[next].first, queue[next].second, focus.x, focus.y) <=
              distanceToTile(x, y, focus.x, focus.y));
    }
}

TEST_CASE("a tile ahead of the travel is read before one as near behind", "[terrain_stream][read_order]") {
    const glm::vec2 focus = tileCentre(32, 32);
    // Render +x is a smaller tile y: one tile ahead and one behind when going +x.
    const std::pair<int, int> ahead = {32, 31}, behind = {32, 33};
    const std::vector<std::pair<int, int>> queue = {behind, ahead};
    CHECK(queue[pick(queue, focus, glm::vec2(0.0f))] == behind);   // a tie: the earlier
    CHECK(queue[pick(queue, focus, glm::vec2(1.0f, 0.0f))] == ahead);
    CHECK(queue[pick(queue, focus, glm::vec2(-1.0f, 0.0f))] == behind);
    // Sideways, neither is ahead: the earlier again.
    CHECK(queue[pick(queue, focus, glm::vec2(0.0f, 1.0f))] == behind);
    // Ahead but much further does not jump the queue past a near one behind.
    const std::vector<std::pair<int, int>> far = {behind, {32, 28}};
    CHECK(far[pick(far, focus, glm::vec2(1.0f, 0.0f))] == behind);
}

// A flight over ground none of which is cached yet, the tiles read one at a
// time at a fixed rate (each waits for a slow server). Read in the order they
// were queued - nearest first from where they were asked for, newest in
// front - the reader works through tiles the flier has left behind; picked
// about the focus, the tile under the flier is in sooner, and picked about
// the focus and its way, the tile it flies into next as well.
TEST_CASE("streamed, the ground the flier comes to is in sooner", "[terrain_stream][read_order]") {
    constexpr float kReach = 600.0f;
    constexpr float kSpeed = 40.0f;          // yards a second, a fast flight
    constexpr float kSecondsPerTile = 3.0f;  // a slow server
    const glm::vec2 heading = glm::normalize(glm::vec2(1.0f, 0.6f));
    struct Result {
        int under = 0;   // seconds over a tile not in
        int next = 0;    // seconds the tile a tile ahead was not in
    };
    auto fly = [&](int mode) {               // 0 queue order, 1 nearest, 2 nearest and ahead
        std::set<std::pair<int, int>> asked, loaded;
        std::vector<std::pair<int, int>> queue;
        glm::vec2 at = tileCentre(32, 40);
        float readClock = 0.0f;
        Result r;
        for (int second = 0; second < 240; ++second) {
            // Streamed about the focus: what came into reach, nearest first,
            // in front of what was queued before (TerrainManager::streamTiles).
            std::vector<std::pair<int, int>> fresh;
            for (const auto& t : loadOrder(at.x, at.y, kReach))
                if (asked.insert(t).second) fresh.push_back(t);
            queue.insert(queue.begin(), fresh.begin(), fresh.end());
            for (readClock += 1.0f; readClock >= kSecondsPerTile && !queue.empty(); readClock -= kSecondsPerTile) {
                size_t i = 0;
                if (mode > 0) i = pick(queue, at, mode == 2 ? heading : glm::vec2(0.0f));
                loaded.insert(queue[i]);
                queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(i));
            }
            if (!loaded.count(tileOf(glm::vec3(at, 0.0f)))) ++r.under;
            if (!loaded.count(tileOf(glm::vec3(at + heading * kTileSize, 0.0f)))) ++r.next;
            at += heading * kSpeed;
        }
        return r;
    };
    const Result queued = fly(0), nearest = fly(1), ahead = fly(2);
    INFO("under / next: queue order " << queued.under << "/" << queued.next << ", nearest " << nearest.under << "/"
                                      << nearest.next << ", ahead " << ahead.under << "/" << ahead.next);
    CHECK(nearest.under < queued.under);
    CHECK(ahead.under <= nearest.under);
    CHECK(ahead.next < nearest.next);
    CHECK(ahead.next < queued.next);
}
