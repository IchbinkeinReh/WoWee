// What a camera inside a building sees of the outside (rendering/wmo_portal_walk.hpp),
// on the files' own portals: Booty Bay, whose decks went missing beyond a door,
// and the Goldshire inn, whose roof must stay hidden from the bedrooms.

#include "rendering/frustum.hpp"
#include "rendering/wmo_portal_walk.hpp"


#include <catch_amalgamated.hpp>
#include "test_support.hpp"
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <utility>
#include <vector>

using namespace wowee::rendering;
namespace walk = wowee::rendering::wmo_portal_walk;

namespace {

struct Data {
    std::vector<glm::vec3> vertices;
    std::vector<walk::Portal> portals;
    std::vector<walk::Ref> refs;
    std::vector<std::pair<uint16_t, uint16_t>> groupRefs;
    std::vector<uint32_t> flags;

    explicit Data(size_t groups, uint32_t all = 0x2000u) : groupRefs(groups, {0, 0}), flags(groups, all) {}
    uint16_t portal(glm::vec3 normal, float distance, std::vector<glm::vec3> corners) {
        portals.push_back({static_cast<uint32_t>(vertices.size()), static_cast<uint32_t>(corners.size()), normal,
                           distance});
        vertices.insert(vertices.end(), corners.begin(), corners.end());
        return static_cast<uint16_t>(portals.size() - 1);
    }
    void group(uint16_t g, std::vector<walk::Ref> its) {
        groupRefs[g] = {static_cast<uint16_t>(refs.size()), static_cast<uint16_t>(its.size())};
        refs.insert(refs.end(), its.begin(), its.end());
    }
    walk::Building building() const { return {vertices, portals, refs, groupRefs, flags}; }
};

// Booty Bay's port authority: rooms 17-19, the exterior group 20 past their
// doors, and the exterior groups no portal of theirs leads to.
constexpr uint16_t kRoom = 19, kPorch = 20, kBeyond = 23, kDeck = 29;
Data bootyBay() {
    Data d(46);
    for (uint16_t g : {3, 8, 9, 14, 16, 20, 23, 29, 39, 41, 42, 44}) d.flags[g] = 0x48u;
    const uint16_t p11 = d.portal({-0.91705537f, 0.39876011f, 0.0f}, -57.0421257f,
                                  {{-5.674f, 129.999f, -37.372f},
                                   {-9.871f, 120.348f, -37.372f},
                                   {-9.871f, 120.348f, -22.217f},
                                   {-5.674f, 129.999f, -22.217f}});
    const uint16_t p12 = d.portal({-0.92388237f, 0.38267657f, 0.0f}, -66.5079803f,
                                  {{-16.316f, 134.407f, -37.372f},
                                   {-20.342f, 124.685f, -37.372f},
                                   {-20.342f, 124.685f, -22.178f},
                                   {-16.316f, 134.407f, -22.178f}});
    const uint16_t p13 = d.portal({0.37460878f, 0.92718297f, 0.0f}, -120.4615555f,
                                  {{-6.347f, 132.487f, -32.374f},
                                   {-6.347f, 132.487f, -37.372f},
                                   {-14.435f, 135.754f, -37.372f},
                                   {-14.435f, 135.754f, -32.374f}});
    const uint16_t p14 = d.portal({-0.92050332f, 0.39073476f, 0.0f}, -90.8389435f,
                                  {{-38.897f, 140.849f, -37.415f},
                                   {-40.906f, 136.115f, -37.415f},
                                   {-40.906f, 136.115f, -32.05f},
                                   {-38.897f, 140.849f, -32.05f}});
    d.group(17, {{p11, 18, -1}});
    d.group(18, {{p11, 17, 1}, {p12, kRoom, -1}, {p13, kPorch, -1}});
    d.group(kRoom, {{p12, 18, 1}, {p14, kPorch, -1}});
    d.group(kPorch, {{p13, 18, 1}, {p14, kRoom, 1}});
    return d;
}
struct Box {
    glm::vec3 lo, hi;
};
const Box kDeckBox{{-111.296f, 166.923f, -73.137f}, {-1.240f, 277.601f, 19.606f}};
const Box kBeyondBox{{-62.763f, 72.277f, -75.308f}, {150.350f, 204.494f, 4.481f}};

// The Goldshire inn, every portal of it. Group 4 is the shell with the roof,
// which no portal leads to; 11 the porch past the one way out.
Data goldshireInn() {
    Data d(12);
    d.flags[4] = 0x8u;
    d.flags[11] = 0x8u;
    const auto wallX = [&](float nx, float dist, float x, float y0, float y1, float z0, float z1) {
        return d.portal({nx, 0, 0}, dist, {{x, y1, z0}, {x, y0, z0}, {x, y0, z1}, {x, y1, z1}});
    };
    const uint16_t p0 = wallX(-1, 20.443f, 20.44f, -4.83f, -0.41f, 0.42f, 4.34f);
    const uint16_t p1 = wallX(-1, 14.139f, 14.14f, -4.83f, -0.41f, 0.42f, 4.34f);
    const uint16_t p2 = d.portal({0, 1, 0}, -2.795f,
                                 {{14.14f, 2.8f, 5.48f}, {14.14f, 2.8f, 0.44f}, {10.08f, 2.8f, 0.44f}, {10.08f, 2.8f, 5.48f}});
    const uint16_t p3 = d.portal({0, 0, 1}, -0.423f,
                                 {{-31.29f, -8.6f, 0.42f}, {-21.11f, -8.6f, 0.42f}, {-21.11f, -5.15f, 0.42f}, {-31.29f, -5.15f, 0.42f}});
    const uint16_t p4 = wallX(-1, 3.114f, 3.11f, 7.43f, 11.3f, 7.29f, 11.32f);
    const uint16_t p5 = d.portal({-0.719f, 0.695f, 0}, -0.237f,
                                 {{2.61f, 3.03f, 7.29f}, {0.55f, 0.91f, 7.29f}, {0.55f, 0.91f, 11.33f}, {2.61f, 3.03f, 11.33f}});
    const uint16_t p6 = d.portal({0.687f, 0.727f, 0}, 0.833f,
                                 {{-2.42f, 1.15f, 11.33f}, {-2.42f, 1.15f, 7.29f}, {-4.37f, 2.98f, 7.29f}, {-4.37f, 2.98f, 11.33f}});
    const uint16_t p7 = wallX(-1, -15.439f, -15.44f, -6.04f, -3.08f, 7.29f, 11.32f);
    const uint16_t p8 = wallX(-1, -4.93f, -4.93f, 4.99f, 15.5f, 7.17f, 16.29f);
    const uint16_t p9 = wallX(-1, -17.041f, -17.04f, 0.92f, 3.55f, 0.44f, 4.21f);
    d.group(0, {{p0, 11, 1}, {p1, 1, -1}});
    d.group(1, {{p1, 0, 1}, {p2, 5, -1}});
    d.group(2, {{p4, 6, 1}, {p5, 7, 1}, {p6, 9, 1}, {p8, 5, -1}});
    d.group(3, {{p3, 10, 1}, {p9, 5, 1}});
    d.group(5, {{p2, 1, 1}, {p8, 2, 1}, {p9, 3, -1}});
    d.group(6, {{p4, 2, -1}});
    d.group(7, {{p5, 2, -1}});
    d.group(8, {{p7, 9, 1}});
    d.group(9, {{p6, 2, -1}, {p7, 8, -1}});
    d.group(10, {{p3, 3, -1}});
    d.group(11, {{p0, 0, -1}});
    return d;
}

// Looking along `yaw` (radians from +x) and `pitch` (degrees, down positive).
glm::mat4 camera(const glm::vec3& eye, float yaw, float pitchDown = 0.0f) {
    const float p = glm::radians(pitchDown);
    const glm::vec3 dir(std::cos(yaw) * std::cos(p), std::sin(yaw) * std::cos(p), -std::sin(p));
    return glm::perspective(glm::radians(45.0f), 16.0f / 9.0f, 0.2f, 2000.0f) *
           glm::lookAt(eye, eye + dir, glm::vec3(0, 0, 1));
}

bool shown(const std::optional<walk::Rect>& outside, const glm::mat4& viewProj, const Box& box) {
    if (!outside) return false;
    Frustum f;
    f.extractFromMatrix(walk::throughRect(*outside, viewProj));
    return f.intersectsAABB(box.lo, box.hi);
}

std::optional<walk::Rect> outsideFrom(const Data& d, uint32_t group, const glm::vec3& eye, const glm::mat4& vp) {
    const std::vector<uint32_t> groups{group};
    return walk::outsideRect(d.building(), groups, eye, vp);
}

// The building's own axes are turned 43 degrees from the world's (MODF -137).
constexpr float kBayTurn = 0.750492f;

}  // namespace

TEST_CASE("the camera's view through a door shows every exterior group, not the one behind it", "[wmo_portal]") {
    const Data bay = bootyBay();
    // /gps: -14384.074 403.240 6.631, facing 3.3793, camera 3.50 / 18.10.
    const glm::vec3 eye(-39.320f, 137.532f, -34.421f);
    const glm::mat4 vp = camera(eye, 3.3793f - kBayTurn, 18.10f);
    for (uint16_t g : {kDeck, kBeyond}) {
        // No portal of the room's building leads there.
        for (const auto& r : bay.refs) CHECK(r.groupIndex != g);
    }
    const auto groups = walk::cameraGroups(bay.building(), eye, kRoom, 2.9f);
    REQUIRE(groups == std::vector<uint32_t>{kRoom});
    const auto outside = walk::outsideRect(bay.building(), groups, eye, vp);
    REQUIRE(outside);
    CHECK(shown(outside, vp, kDeckBox));
    CHECK(shown(outside, vp, kBeyondBox));
    // The camera is 0.9 yards from a door five across: it fills the view.
    CHECK(outside->x < -0.9f);
    CHECK(outside->z > 0.9f);
}

TEST_CASE("a camera turning about in the doorway's room keeps what is in the door", "[wmo_portal]") {
    const Data bay = bootyBay();
    const glm::vec3 player(-42.24f, 139.14f, -35.5f);
    // Every corner of the door ahead of the eye and on the screen is in the
    // rectangle, whichever way the camera looks and wherever it stands.
    for (int deg = 0; deg < 360; deg += 10) {
        const float yaw = glm::radians(static_cast<float>(deg));
        const glm::vec3 eye = player - glm::vec3(std::cos(yaw), std::sin(yaw), 0) * 3.5f;
        const auto& door = bay.portals[3];
        if (-walk::planeDistance(door, eye) < 0.0f) continue;  // out of the room
        const glm::mat4 vp = camera(eye, yaw, 10.0f);
        const auto outside = outsideFrom(bay, kRoom, eye, vp);
        for (float u = 0.05f; u < 1.0f; u += 0.1f) {
            for (float v = 0.05f; v < 1.0f; v += 0.1f) {
                const glm::vec3* c = &bay.vertices[door.startVertex];
                const glm::vec3 at = c[0] + (c[1] - c[0]) * u + (c[3] - c[0]) * v;
                const glm::vec4 clip = vp * glm::vec4(at, 1.0f);
                if (clip.w < 0.01f || std::abs(clip.x) > clip.w || std::abs(clip.y) > clip.w) continue;
                INFO("yaw " << deg << " u " << u << " v " << v);
                REQUIRE(outside);
                CHECK(clip.x / clip.w >= outside->x - 1e-3f);
                CHECK(clip.x / clip.w <= outside->z + 1e-3f);
                CHECK(clip.y / clip.w >= outside->y - 1e-3f);
                CHECK(clip.y / clip.w <= outside->w + 1e-3f);
            }
        }
    }
}

TEST_CASE("a bedroom of the Goldshire inn sees no way out, whichever way it looks", "[wmo_portal]") {
    const Data inn = goldshireInn();
    for (const auto& [group, eye] : {std::pair<uint32_t, glm::vec3>{6, {9.192f, 5.186f, 10.400f}},
                                     {6, {14.978f, 8.797f, 10.201f}},
                                     {6, {3.168f, 8.386f, 10.201f}},
                                     {7, {7.0f, -3.0f, 10.0f}},
                                     {8, {-24.0f, -5.0f, 10.0f}}}) {
        for (int deg = 0; deg < 360; deg += 10) {
            INFO("group " << group << " yaw " << deg);
            CHECK_FALSE(outsideFrom(inn, group, eye, camera(eye, glm::radians(static_cast<float>(deg)), 10.0f)));
        }
    }
}

TEST_CASE("the inn's way out shows the shell no portal leads to", "[wmo_portal]") {
    const Data inn = goldshireInn();
    const glm::vec3 eye(17.0f, -2.6f, 2.2f);
    const glm::mat4 out = camera(eye, 0.0f), in = camera(eye, glm::radians(180.0f));
    const Box shell{{-37.8f, -13.5f, -0.2f}, {19.9f + 2.0f, 18.7f, 23.3f}};
    CHECK(shown(outsideFrom(inn, 0, eye, out), out, shell));
    // The door behind the camera is not in sight.
    CHECK_FALSE(outsideFrom(inn, 0, eye, in));
}

TEST_CASE("a camera at the door, before and just past its plane, sees out", "[wmo_portal]") {
    const Data inn = goldshireInn();
    for (float x : {20.2f, 20.40f, 20.44f, 20.5f, 20.6f}) {
        const glm::vec3 eye(x, -2.6f, 2.2f);
        INFO("x " << x);
        const auto outside = outsideFrom(inn, 0, eye, camera(eye, 0.0f));
        REQUIRE(outside);
        CHECK(outside->z - outside->x > 1.9f);
        CHECK(outside->w - outside->y > 1.9f);
    }
    // From its far side a portal leads nowhere out of the room.
    CHECK_FALSE(outsideFrom(inn, 0, {23.0f, -2.6f, 2.2f}, camera({23.0f, -2.6f, 2.2f}, glm::radians(180.0f))));
    // A door's corner beside and behind the eye is cut off, not taken for the
    // whole view: seen from close beside it, the door is at one edge.
    const glm::vec3 eye(20.0f, 0.5f, 2.2f);
    const auto outside = outsideFrom(inn, 0, eye, camera(eye, glm::radians(-60.0f)));
    REQUIRE(outside);
    CHECK(outside->z - outside->x < 1.9f);
}

TEST_CASE("the walk goes through the rooms between", "[wmo_portal]") {
    const Data inn = goldshireInn();
    // From group 1 through 0 to the porch, in a line; from its far side not.
    const glm::vec3 eye(11.0f, -2.6f, 2.2f);
    const auto outside = outsideFrom(inn, 1, eye, camera(eye, 0.0f));
    REQUIRE(outside);
    CHECK((outside->z - outside->x) < 0.7f);
    const glm::vec3 aside(12.0f, 2.0f, 2.2f);
    CHECK_FALSE(outsideFrom(inn, 1, aside, camera(aside, glm::radians(-70.0f), 0.0f)));
}

TEST_CASE("the camera's rooms are its floor's, and those a portal under it joins", "[wmo_portal]") {
    const Data inn = goldshireInn();
    const auto b = inn.building();
    CHECK(walk::cameraGroups(b, {0, 0, 2}, 5, 2.0f) == std::vector<uint32_t>{5});
    CHECK(walk::cameraGroups(b, {0, 0, 2}, 4, 2.0f).empty());
    CHECK(walk::cameraGroups(b, {0, 0, 2}, 99, 2.0f).empty());
    // Over the stairwell into the cellar: the cellar's floor, 8.8 below, with
    // the portal (z 0.42) before it.
    CHECK(walk::cameraGroups(b, {-26, -7, 2}, 10, 8.8f) == std::vector<uint32_t>{3, 10});
    // Beside it, and below it.
    CHECK(walk::cameraGroups(b, {-26, -3, 2}, 3, 1.6f) == std::vector<uint32_t>{3});
    CHECK(walk::cameraGroups(b, {-26, -7, -1}, 10, 5.8f) == std::vector<uint32_t>{10});
    // The face before the portal.
    CHECK(walk::cameraGroups(b, {-26, -7, 2}, 3, 1.0f) == std::vector<uint32_t>{3});
}

TEST_CASE("a camera over an exit in the floor is outside", "[wmo_portal]") {
    Data inn = goldshireInn();
    inn.flags[3] = 0x8u;
    CHECK(walk::cameraGroups(inn.building(), {-26, -7, 2}, 10, 8.8f).empty());
    // Below it, the far side is dropped.
    CHECK(walk::cameraGroups(inn.building(), {-26, -7, -1}, 10, 5.8f) == std::vector<uint32_t>{10});
}

// V394: a portal to a group lit by the sky, and a camera in one, is the sky's
// rectangle (0xadf570), not the outside's (0xadf58c: 0x007a8f20's last argument).
TEST_CASE("the outside shows through the outside's groups alone", "[wmo_portal]") {
    Data inn = goldshireInn();
    const glm::vec3 eye(11.0f, -2.6f, 2.2f);
    inn.flags[11] = 0x2000u;  // no exterior group left to find
    CHECK_FALSE(outsideFrom(inn, 1, eye, camera(eye, 0.0f)));
    inn.flags[0] = 0x2040u;  // lit by the outside, the room between
    CHECK_FALSE(outsideFrom(inn, 1, eye, camera(eye, 0.0f)));
    // Past it as before.
    inn.flags[11] = 0x8u;
    CHECK(outsideFrom(inn, 1, eye, camera(eye, 0.0f)));
    inn.flags[11] = 0x2000u;
    inn.flags[0] = 0x2000u;
    inn.flags[1] = 0x2040u;  // a street, as Stormwind's
    CHECK_FALSE(outsideFrom(inn, 1, eye, camera(eye, glm::radians(180.0f))));
}

TEST_CASE("portals without a plane and broken ones lose nothing and hang nothing", "[wmo_portal]") {
    // A ring of rooms, each door in sight of every other, no planes: walked
    // until it is given up on, and then taken for open.
    Data ring(12);
    std::vector<glm::vec3> door{{5, -1, -1}, {5, 1, -1}, {5, 1, 1}, {5, -1, 1}};
    for (uint16_t g = 0; g < 12; ++g) {
        std::vector<walk::Ref> refs;
        for (uint16_t t = 0; t < 12; ++t) {
            if (t != g) refs.push_back({ring.portal(glm::vec3(0), 0, door), t, 1});
        }
        ring.group(g, refs);
    }
    const glm::vec3 eye(0.0f);
    CHECK(outsideFrom(ring, 0, eye, camera(eye, 0.0f)));
    Data bad(2);
    bad.flags[1] = 0x8u;
    const uint16_t p = bad.portal({1, 0, 0}, 0, {{5, 0, 0}, {5, 1, 0}});
    bad.group(0, {{p, 1, 1}, {99, 1, 1}, {p, 77, 1}, {p, 0xffff, 1}});
    CHECK_FALSE(outsideFrom(bad, 0, {1, 0, 0}, camera({1, 0, 0}, 0.0f)));
}

TEST_CASE("the view through a rectangle is that part of the screen", "[wmo_portal]") {
    const glm::mat4 vp = camera({0, 0, 0}, 0.0f);
    const walk::Rect r(-0.5f, -0.25f, 0.1f, 0.75f);
    const glm::mat4 cut = walk::throughRect(r, vp);
    for (const glm::vec3 at : {glm::vec3(10, 1, 1), glm::vec3(30, -4, 2), glm::vec3(5, 0, -1)}) {
        const glm::vec4 a = vp * glm::vec4(at, 1), c = cut * glm::vec4(at, 1);
        CHECK(c.w == Catch::Approx(a.w));
        CHECK(c.x / c.w == Catch::Approx((a.x / a.w - r.x) / (r.z - r.x) * 2.0f - 1.0f).margin(1e-4));
        CHECK(c.y / c.w == Catch::Approx((a.y / a.w - r.y) / (r.w - r.y) * 2.0f - 1.0f).margin(1e-4));
    }
}

// U14: a building's doodad is drawn with its groups (MODR), and a room is seen
// from outside only through the portals in sight (0x007ad350 from the outside's
// groups, 0x007ad1f0 from the camera's rooms). Undercity's chandelier in the
// throne-room shaft hung its chain up through the ruins' courtyard.
TEST_CASE("from the porch the inn's rooms behind the door are seen, the bedrooms not", "[wmo_portal]") {
    const Data inn = goldshireInn();
    const std::vector<uint32_t> outside{11};
    const glm::vec3 eye(30.0f, -2.6f, 2.2f);
    const auto seen = walk::groupsSeen(inn.building(), outside, eye, camera(eye, glm::radians(180.0f)));
    REQUIRE(seen.size() == inn.flags.size());
    CHECK(seen[11]);
    CHECK(seen[0]);
    CHECK(seen[1]);
    CHECK_FALSE(seen[6]);
    CHECK_FALSE(seen[7]);
    // Turned away from the door: no room.
    const auto away = walk::groupsSeen(inn.building(), outside, eye, camera(eye, 0.0f));
    CHECK_FALSE(away[0]);
    CHECK_FALSE(away[1]);
}

TEST_CASE("a doodad is drawn with a group of its own that is seen", "[wmo_portal]") {
    const Data inn = goldshireInn();
    std::vector<std::optional<walk::Rect>> seen(inn.flags.size());
    seen[0] = walk::Rect(-0.2f, -0.3f, 0.2f, 0.3f);
    auto anywhere = [](uint32_t, const walk::Rect&) { return true; };
    auto nowhere = [](uint32_t, const walk::Rect&) { return false; };
    const std::vector<uint32_t> room0{0}, bedroom{6}, shell{4}, none{}, both{6, 0}, unknown{40};
    CHECK(walk::doodadSeen(room0, inn.flags, seen, anywhere));
    CHECK_FALSE(walk::doodadSeen(room0, inn.flags, seen, nowhere));  // outside the door's part of the screen
    CHECK_FALSE(walk::doodadSeen(bedroom, inn.flags, seen, anywhere));
    CHECK(walk::doodadSeen(both, inn.flags, seen, anywhere));
    // The outside's and the always-drawn groups' doodads as before; one no
    // group lists, or of a group the file does not have, too.
    CHECK(walk::doodadSeen(shell, inn.flags, seen, nowhere));
    CHECK(walk::doodadSeen(none, inn.flags, seen, nowhere));
    CHECK(walk::doodadSeen(unknown, inn.flags, seen, nowhere));
}

TEST_CASE("a camera over another floor is outside the building whose room box it is in (V457)", "[wmo_portal]") {
    // The Ruins of Lordaeron: the camera in the box of UNDERCITY.WMO's group
    // 7 (0x2040, a room under the sky), over ground that is not the group's.
    // 0x007d59b0 takes the camera's group from the face under it, so
    // Undercity is seen from outside (0x007ad350), and group 96's chandelier,
    // which no portal shows, is not drawn.
    using wowee::rendering::wmo_portal_walk::insideUnwalkable;
    CHECK_FALSE(insideUnwalkable(true, false, true));
    // Only with nothing under the camera at all is a room's box taken for
    // inside: its doodads all drawn rather than a room emptied.
    CHECK(insideUnwalkable(true, false, false));
    CHECK_FALSE(insideUnwalkable(true, true, false));
    CHECK_FALSE(insideUnwalkable(false, false, false));
}

TEST_CASE("the room doodads take a camera over a floor for outside (V457)", "[wmo_portal]") {
    const std::string wmo = wowee::test::slurp("src/rendering/wmo_renderer.cpp");
    REQUIRE_FALSE(wmo.empty());
    const size_t from = wmo.find("void WMORenderer::updateRoomDoodads(");
    REQUIRE(from != std::string::npos);
    const std::string body = wmo.substr(from, wmo.find("size_t WMORenderer::instanceDoodadCount(", from) - from);
    CHECK(body.find("insideUnwalkable(inRoomBox, inOutsideBox, anythingBelow)") != std::string::npos);
    CHECK(body.find("if (inRoomBox && !inOutsideBox) cull = false;") == std::string::npos);
}

TEST_CASE("a building's doodads belong to its one instance, whichever tile placed it (V468)", "[wmo_portal]") {
    using wowee::rendering::wmo_portal_walk::doodadBuilding;
    // UNDERCITY.WMO is listed by the tiles on both sides of x = 1600. One
    // tile placed it (instance 77) before the other, whose worker had read
    // its doodads, came to it: that tile skipped the placement (0 for it),
    // and the doodads - the chandelier of group 96 among them - were drawn
    // from everywhere, its chain over the Ruins of Lordaeron.
    const std::unordered_map<uint32_t, uint32_t> placed{{0x1234u, 77u}};
    CHECK(doodadBuilding(0, 0x1234u, placed) == 77u);
    CHECK(doodadBuilding(12, 0x1234u, placed) == 12u);
    CHECK(doodadBuilding(0, 0x9999u, placed) == 0u);
    CHECK(doodadBuilding(0, 0, placed) == 0u);
}

TEST_CASE("the tiles hand a building's doodads to the instance placed for its unique id (V468)", "[wmo_portal]") {
    const std::string tm = wowee::test::slurp("src/rendering/terrain_manager.cpp");
    REQUIRE_FALSE(tm.empty());
    CHECK(tm.find("placedWmoInstances_[wmoReady.uniqueId] = wmoInstId;") != std::string::npos);
    CHECK(tm.find("wmo_portal_walk::doodadBuilding(") != std::string::npos);
}
