// Nothing within the far clip is left to neither the ground nor the .wdl
// horizon, at any field of view, turn and pitch (pipeline::wdl::tileDraw).
//
// The ground's chunks are culled by the far clip's cube about the camera
// (0x00780860, 0x007b6110), the horizon cut by view depth at the far clip
// less 50 (0x00791170). Along a diagonal of the cube, or with a wide view,
// the cube's sides come in front of that depth: the ground past them was not
// drawn and the horizon not yet, and the sky showed through the land at the
// top corners of the view (user 175: fov 70, facing 3.8515 on Red Cloud
// Mesa; a slight turn, 176, closed it).
#include <catch_amalgamated.hpp>

#include "pipeline/wdl_loader.hpp"
#include "rendering/terrain_stream_set.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <set>
#include <utility>

namespace wdl = wowee::pipeline::wdl;
namespace ts = wowee::rendering::terrain_stream;

namespace {

// .go xyz -2977.852 -411.907 50.844 1 (render x, y are the server's).
constexpr float kMesaX = -2977.852f;
constexpr float kMesaY = -411.907f;
constexpr float kMesaZ = 52.5f;

/// The smallest radius a chunk's bounding sphere can have: half its diagonal
/// (a flat chunk). The cube's reach is widest for a tall chunk, so a flat
/// one is the hardest case for the ground.
const float kFlatChunkRadius = wdl::kCellSize * std::sqrt(2.0f) * 0.5f;

int rowOf(float x) { return static_cast<int>(std::floor((wdl::kZeroPoint - x) / wdl::kTileSize)); }
int colOf(float y) { return static_cast<int>(std::floor((wdl::kZeroPoint - y) / wdl::kTileSize)); }
float cellCentre(float c) {
    const float k = std::floor((wdl::kZeroPoint - c) / wdl::kCellSize);
    return wdl::kZeroPoint - (k + 0.5f) * wdl::kCellSize;
}

enum class Loaded { All, Square, None };

struct Pose {
    float farClip, fov, aspect, yaw, pitch;
};

struct Tally {
    long samples = 0;
    long holes = 0;        // neither the ground nor the horizon
    long holesOld = 0;     // the same before (tileNear only, no yielding)
    long horizonOverGround = 0;  // the horizon drawn where the ground is, before the far clip less 50
    Pose firstHole{}, firstOldHole{};
};

/// Every point of the view out to the far clip's depth, on a grid of the
/// screen and of depths, for the camera orbiting the mesa's focus.
void sweep(const Pose& pose, Loaded loaded, const std::set<std::pair<int, int>>& square, Tally& t) {
    const glm::vec3 focus(kMesaX, kMesaY, kMesaZ);
    const float y = glm::radians(pose.yaw), p = glm::radians(pose.pitch);
    const glm::vec3 f(std::cos(y) * std::cos(p), std::sin(y) * std::cos(p), std::sin(p));
    const glm::vec3 eye = focus - f * 6.55f;
    const glm::vec3 r = glm::normalize(glm::cross(f, glm::vec3(0, 0, 1)));
    const glm::vec3 u = glm::cross(r, f);
    const float tv = std::tan(glm::radians(pose.fov * 0.5f)), th = tv * pose.aspect;
    const float F = pose.farClip;
    const float horizonFar = wdl::horizonFar(F, 3.0f);
    constexpr int kGrid = 8;
    constexpr int kDepths = 28;
    for (int i = -kGrid; i <= kGrid; ++i) {
        for (int j = -kGrid; j <= kGrid; ++j) {
            const glm::vec3 dirPerDepth =
                f + r * (th * static_cast<float>(i) / kGrid) + u * (tv * static_cast<float>(j) / kGrid);
            for (int k = 0; k <= kDepths; ++k) {
                // 1.5 yards out to the far clip, denser toward it.
                const float s = static_cast<float>(k) / kDepths;
                const float depth = 1.5f + (F - 1.5f) * std::sqrt(s) - (k == kDepths ? 0.01f : 0.0f);
                const glm::vec3 at = eye + dirPerDepth * depth;
                const int row = rowOf(at.x), col = colOf(at.y);
                // A .wdl tile at (row, col) is ADT tile (col, row).
                const bool groundIn = loaded == Loaded::All ||
                                      (loaded == Loaded::Square && square.count({col, row}) == 1);
                const float maxX = wdl::kZeroPoint - static_cast<float>(row) * wdl::kTileSize;
                const float maxY = wdl::kZeroPoint - static_cast<float>(col) * wdl::kTileSize;
                const wdl::TileDraw draw = wdl::tileDraw(groundIn, maxX - wdl::kTileSize, maxY - wdl::kTileSize,
                                                         maxX, maxY, eye.x, eye.y, F);
                const float dx = at.x - eye.x, dy = at.y - eye.y;
                const float cdx = cellCentre(at.x) - eye.x, cdy = cellCentre(at.y) - eye.y;
                ++t.samples;
                if (!wdl::groundOrHorizonAt(groundIn, draw, cdx, cdy, kFlatChunkRadius, dx, dy, depth, F,
                                            horizonFar)) {
                    if (t.holes++ == 0) t.firstHole = pose;
                }
                const wdl::TileDraw old{wdl::tileNear(groundIn, F), 0.0f};
                if (!wdl::groundOrHorizonAt(groundIn, old, cdx, cdy, kFlatChunkRadius, dx, dy, depth, F,
                                            horizonFar)) {
                    if (t.holesOld++ == 0) t.firstOldHole = pose;
                }
                // Where the ground is drawn the horizon is the client's: not
                // in front of the far clip less 50.
                const bool groundDraws = groundIn && wdl::groundDrawsAt(dx, dy, F);
                const bool horizonDraws = depth >= draw.fromDepth &&
                                          !wdl::horizonYieldsAt(dx, dy, depth, draw.groundNear, F);
                if (groundDraws && horizonDraws && depth < wdl::horizonNear(F)) ++t.horizonOverGround;
            }
        }
    }
}

}  // namespace

TEST_CASE("the ground's cube and the horizon cover the view out to the far clip", "[horizon]") {
    for (const float F : {183.4f, 791.6667f, 1583.3334f}) {
        const auto order = ts::loadOrder(kMesaX, kMesaY, ts::loadReach(F));
        const std::set<std::pair<int, int>> square(order.begin(), order.end());
        for (const Loaded loaded : {Loaded::All, Loaded::Square, Loaded::None}) {
            Tally t;
            for (const float fov : {45.0f, 54.0f, 70.0f, 90.0f}) {
                for (const float aspect : {4.0f / 3.0f, 1918.0f / 1001.0f, 21.0f / 9.0f}) {
                    for (float yaw = 0.0f; yaw < 360.0f; yaw += 5.0f) {
                        for (const float pitch : {-60.0f, -16.74f, 0.0f, 16.74f, 45.0f}) {
                            sweep({F, fov, aspect, yaw, pitch}, loaded, square, t);
                        }
                    }
                }
            }
            INFO("far clip " << F << " loaded " << static_cast<int>(loaded) << " samples " << t.samples
                             << " first hole fov " << t.firstHole.fov << " aspect " << t.firstHole.aspect
                             << " yaw " << t.firstHole.yaw << " pitch " << t.firstHole.pitch);
            CHECK(t.samples > 0);
            CHECK(t.holes == 0);
            CHECK(t.horizonOverGround == 0);
            // The cube left holes before wherever ground was in.
            if (loaded != Loaded::None) CHECK(t.holesOld > 0);
        }
    }
}

TEST_CASE("the user's 175 pose had the hole, at fov 70 and not only wide", "[horizon]") {
    // Facing 3.8515 rad (220.7 degrees), 4.3 degrees off the cube's diagonal;
    // the camera's yaw in the test's frame is the orientation.
    const float F = 791.6667f;
    const auto order = ts::loadOrder(kMesaX, kMesaY, ts::loadReach(F));
    const std::set<std::pair<int, int>> square(order.begin(), order.end());
    Tally t175, t176;
    sweep({F, 70.0f, 1918.0f / 1001.0f, glm::degrees(3.8515f), 16.74f}, Loaded::Square, square, t175);
    sweep({F, 70.0f, 1918.0f / 1001.0f, glm::degrees(4.2329f), 16.74f}, Loaded::Square, square, t176);
    CHECK(t175.holesOld > 0);
    CHECK(t175.holes == 0);
    CHECK(t176.holes == 0);
}

TEST_CASE("a tile wholly inside the cube is the client's horizon", "[horizon]") {
    const float F = 791.6667f;
    const wdl::TileDraw in = wdl::tileDraw(true, -100.0f, -100.0f, 433.0f, 433.0f, 0.0f, 0.0f, F);
    CHECK(in.fromDepth == wdl::horizonNear(F));
    CHECK(in.groundNear == 0.0f);
    const wdl::TileDraw out = wdl::tileDraw(true, 500.0f, 0.0f, 1033.0f, 533.0f, 0.0f, 0.0f, F);
    CHECK(out.fromDepth == wdl::kNearWithoutGround);
    CHECK(out.groundNear == wdl::horizonNear(F));
    const wdl::TileDraw none = wdl::tileDraw(false, -100.0f, -100.0f, 433.0f, 433.0f, 0.0f, 0.0f, F);
    CHECK(none.fromDepth == wdl::kNearWithoutGround);
    CHECK(none.groundNear == 0.0f);
    // Inside the cube before the far clip less 50 it yields to the ground;
    // past the cube's side, or past that depth, it does not.
    CHECK(wdl::horizonYieldsAt(600.0f, 100.0f, 700.0f, out.groundNear, F));
    CHECK_FALSE(wdl::horizonYieldsAt(800.0f, 100.0f, 700.0f, out.groundNear, F));
    CHECK_FALSE(wdl::horizonYieldsAt(600.0f, 100.0f, 760.0f, out.groundNear, F));
}
