// rendering::ray_mesh: where a pick ray first meets a skinned model's
// triangles, and which of several overlapping units that makes the one
// pointed at.

#include <catch_amalgamated.hpp>

#include "rendering/pointer_pick.hpp"
#include "rendering/ray_mesh.hpp"

#include <glm/gtc/matrix_transform.hpp>

using namespace wowee::rendering::ray_mesh;

namespace {

// A unit's body as one upright square, a yard wide and two tall, facing +x,
// its four corners on bone 0.
std::vector<SkinnedVertex> body() {
    std::vector<SkinnedVertex> v(4);
    const glm::vec3 corners[4] = {{0, -0.5f, 0}, {0, 0.5f, 0}, {0, 0.5f, 2}, {0, -0.5f, 2}};
    for (int i = 0; i < 4; ++i) {
        v[i].position = corners[i];
        v[i].boneIndices[0] = 0;
        v[i].boneWeights[0] = 255;
    }
    return v;
}
const std::vector<uint16_t> kQuad = {0, 1, 2, 0, 2, 3};
const std::vector<std::pair<uint32_t, uint32_t>> kAll = {{0u, 6u}};

}  // namespace

TEST_CASE("a ray meets a triangle from either face, in front of its origin", "[ray_mesh]") {
    const glm::vec3 a(0, -1, -1), b(0, 1, -1), c(0, 0, 1);
    auto t = rayTriangle(glm::vec3(-5, 0, 0), glm::vec3(1, 0, 0), a, b, c);
    REQUIRE(t);
    CHECK(*t == Catch::Approx(5.0f));
    t = rayTriangle(glm::vec3(5, 0, 0), glm::vec3(-1, 0, 0), a, b, c);
    REQUIRE(t);
    CHECK(*t == Catch::Approx(5.0f));
    CHECK_FALSE(rayTriangle(glm::vec3(5, 0, 0), glm::vec3(1, 0, 0), a, b, c));   // behind
    CHECK_FALSE(rayTriangle(glm::vec3(-5, 3, 0), glm::vec3(1, 0, 0), a, b, c));  // beside
    // An unnormalised direction keeps its own parameter.
    t = rayTriangle(glm::vec3(-5, 0, 0), glm::vec3(2, 0, 0), a, b, c);
    REQUIRE(t);
    CHECK(*t == Catch::Approx(2.5f));
}

TEST_CASE("a skinned model is hit where its bones hold it, by the world ray's distance", "[ray_mesh]") {
    const auto v = body();
    // Placed ten yards out along x and doubled in size.
    const glm::mat4 model = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0)), glm::vec3(2.0f));
    const std::vector<glm::mat4> rest(1, glm::mat4(1.0f));
    auto t = rayHitSkinned(v, kQuad, kAll, rest, model, glm::vec3(0, 0, 1), glm::vec3(1, 0, 0), 100.0f);
    REQUIRE(t);
    CHECK(*t == Catch::Approx(10.0f));
    // Above its head at rest (four yards tall once doubled)...
    CHECK_FALSE(rayHitSkinned(v, kQuad, kAll, rest, model, glm::vec3(0, 0, 4.5f), glm::vec3(1, 0, 0), 100.0f));
    // ...but the bone lifts it a yard, two in the world.
    const std::vector<glm::mat4> lifted(1, glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 1)));
    t = rayHitSkinned(v, kQuad, kAll, lifted, model, glm::vec3(0, 0, 4.5f), glm::vec3(1, 0, 0), 100.0f);
    REQUIRE(t);
    CHECK(*t == Catch::Approx(10.0f));
    // Not past the limit, and not on a batch it does not draw.
    CHECK_FALSE(rayHitSkinned(v, kQuad, kAll, rest, model, glm::vec3(0, 0, 1), glm::vec3(1, 0, 0), 9.0f));
    const std::vector<std::pair<uint32_t, uint32_t>> none;
    CHECK_FALSE(rayHitSkinned(v, kQuad, none, rest, model, glm::vec3(0, 0, 1), glm::vec3(1, 0, 0), 100.0f));
    // A vertex's weights split between two bones.
    auto split = v;
    for (auto& s : split) {
        s.boneIndices[1] = 1;
        s.boneWeights[0] = 128;
        s.boneWeights[1] = 127;
    }
    const std::vector<glm::mat4> two = {glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(2, 0, 0))};
    t = rayHitSkinned(split, kQuad, kAll, two, glm::mat4(1.0f), glm::vec3(-5, 0, 1), glm::vec3(1, 0, 0), 100.0f);
    REQUIRE(t);
    CHECK(*t == Catch::Approx(5.0f + 2.0f * 127.0f / 255.0f).margin(1e-3f));
}

TEST_CASE("of two units in one another the one in front at the pointer is picked", "[ray_mesh]") {
    // Two of the same body on one spot, one turned a quarter so its square
    // faces +y: from the front the first is met, from the side the turned
    // one - each where its body shows, whatever their centres.
    const auto v = body();
    const std::vector<glm::mat4> rest(1, glm::mat4(1.0f));
    const glm::mat4 front(1.0f);
    const glm::mat4 turned = glm::translate(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 0, 1)),
                                            glm::vec3(0.3f, 0, 0));
    auto hits = [&](const glm::vec3& origin, const glm::vec3& dir) {
        return std::vector<std::optional<float>>{
            rayHitSkinned(v, kQuad, kAll, rest, front, origin, dir, 100.0f),
            rayHitSkinned(v, kQuad, kAll, rest, turned, origin, dir, 100.0f)};
    };
    // From in front (-x) through the first body's middle: the first.
    auto best = nearestPrecise(hits(glm::vec3(-5, 0, 1), glm::vec3(1, 0, 0)));
    REQUIRE(best);
    CHECK(*best == 0u);
    // From the side (-y): the first is edge-on there, the turned one shows.
    best = nearestPrecise(hits(glm::vec3(0, -5, 1), glm::vec3(0, 1, 0)));
    REQUIRE(best);
    CHECK(*best == 1u);
    // Where the ray meets neither, nothing: the spheres decide.
    CHECK_FALSE(nearestPrecise(hits(glm::vec3(-5, 3, 1), glm::vec3(1, 0, 0))));
    CHECK_FALSE(nearestPrecise({}));
}

// What hides an object from the pointer (0x004f9930, playtest P78): what is
// drawn, not what only stands in the way.
TEST_CASE("the pointer goes through a building's collision faces, not its drawn ones", "[ray_mesh][pointer]") {
    namespace pp = wowee::rendering::pointer_pick;
    // 0x007ae140, read off its instructions for the traces the client makes.
    CHECK(pp::buildingFaceMask(pp::kTraceSeen) == 0x188);
    CHECK(pp::buildingFaceMask(pp::kTraceSeenLiquid) == 0x188);
    CHECK(pp::buildingFaceMask(pp::kTraceGround) == 0x86);
    CHECK(pp::buildingFaceMask(0x120111) == 0x84);  // movement and sight
    CHECK(pp::buildingFaceMask(0x100171) == 0x82);  // the camera
    CHECK(pp::buildingFaceMask(0x220122) == 0x88);  // the weather

    const uint32_t seen = pp::buildingFaceMask(pp::kTraceSeen);
    CHECK_FALSE(pp::buildingFaceHit(seen, 0x08, false));  // collision alone
    CHECK_FALSE(pp::buildingFaceHit(seen, 0x48, false));
    CHECK(pp::buildingFaceHit(seen, 0x20, false));  // drawn
    CHECK(pp::buildingFaceHit(seen, 0x22, false));  // ...that the camera goes through
    CHECK(pp::buildingFaceHit(seen, 0x24, false));  // ...a detail
    CHECK_FALSE(pp::buildingFaceHit(seen, 0x20, true));  // ...in glass
    const uint32_t ground = pp::buildingFaceMask(pp::kTraceGround);
    CHECK(pp::buildingFaceHit(ground, 0x08, false));
    CHECK(pp::buildingFaceHit(ground, 0x20, true));
    CHECK_FALSE(pp::buildingFaceHit(ground, 0x22, false));
    CHECK_FALSE(pp::buildingFaceHit(ground, 0x24, false));
    // The movement's faces are as they were.
    CHECK(pp::buildingFaceHit(0x04, 0x08, false));
    CHECK(pp::buildingFaceHit(0x04, 0x20, true));
    CHECK_FALSE(pp::buildingFaceHit(0x04, 0x24, false));
}

TEST_CASE("a doodad hides what is behind its opaque triangles alone", "[ray_mesh][pointer]") {
    namespace pp = wowee::rendering::pointer_pick;
    CHECK(pp::doodadBatchHides(0x10, 0, 0, 0x0, true));
    CHECK_FALSE(pp::doodadBatchHides(0x10, 0, 1, 0x0, true));  // leaves
    CHECK_FALSE(pp::doodadBatchHides(0x10, 0, 2, 0x0, true));  // glass
    CHECK(pp::doodadBatchHides(0x10, 0, 2, 0x20, true));
    CHECK_FALSE(pp::doodadBatchHides(0x10, 1, 0, 0x0, true));  // a second layer
    CHECK_FALSE(pp::doodadBatchHides(0x18, 0, 0, 0x0, true));
    CHECK_FALSE(pp::doodadBatchHides(0x10, 0, 0, 0x0, false));  // of no alpha

    // A post a yard wide and two tall, and beside it, along y, a pane.
    auto v = body();
    for (const auto& c : body()) {
        v.push_back(c);
        v.back().position.y += 5.0f;
    }
    const std::vector<uint16_t> indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    const glm::mat4 at = glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0));
    const auto mesh = pp::doodadMesh(v, indices, {{0u, 6u}});
    CHECK(mesh.vertices.size() == 4);
    CHECK(mesh.indices.size() == 6);
    const auto t = pp::rayHitDoodad(mesh, {}, at, glm::vec3(0, 0, 1), glm::vec3(1, 0, 0), 100.0f);
    REQUIRE(t);
    CHECK(*t == Catch::Approx(10.0f));
    CHECK_FALSE(pp::rayHitDoodad(mesh, {}, at, glm::vec3(0, 5, 1), glm::vec3(1, 0, 0), 100.0f));
    CHECK_FALSE(pp::rayHitDoodad(mesh, {}, at, glm::vec3(0, 0, 1), glm::vec3(1, 0, 0), 9.0f));
    // A model of collision alone draws nothing, and so hides nothing.
    CHECK(pp::doodadMesh(v, indices, {}).empty());
}

// 0x0081daf0, mode 0
TEST_CASE("an object is found by its first layers, blended or not", "[pointer]") {
    using namespace wowee::rendering::pointer_pick;
    CHECK(objectBatchFound(0, 0));
    CHECK_FALSE(objectBatchFound(0x8, 0));
    CHECK_FALSE(objectBatchFound(0, 1));
    struct Vertex {
        glm::vec3 position{0.0f};
        uint8_t boneWeights[4] = {255, 0, 0, 0};
        uint8_t boneIndices[4] = {0, 0, 0, 0};
    };
    struct Batch {
        uint8_t flags = 0;
        uint16_t materialLayer = 0;
        uint32_t indexStart = 0, indexCount = 3;
    };
    struct {
        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices = {0, 1, 2, 3, 4, 5};
        std::vector<Batch> batches;
    } model;
    for (const glm::vec3& p : {glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(5, 0, 0),
                               glm::vec3(6, 0, 0), glm::vec3(5, 1, 0)})
        model.vertices.push_back({.position = p});
    CHECK(objectMesh(model).indices.size() == 6);   // without batches, whole
    model.batches = {{.indexStart = 0}, {.materialLayer = 1, .indexStart = 3}};
    const auto mesh = objectMesh(model);
    CHECK(mesh.indices.size() == 3);
    CHECK(mesh.vertices.size() == 3);
    const std::vector<glm::mat4> bones;
    const glm::vec3 down(0.0f, 0.0f, -1.0f);
    const auto hit = rayHitDoodad(mesh, bones, glm::mat4(1.0f), glm::vec3(0.2f, 0.2f, 4.0f), down, 100.0f);
    REQUIRE(hit);
    CHECK(*hit == Catch::Approx(4.0f));
    CHECK_FALSE(rayHitDoodad(mesh, bones, glm::mat4(1.0f), glm::vec3(0.8f, 0.8f, 4.0f), down, 100.0f));
    CHECK_FALSE(rayHitDoodad(mesh, bones, glm::mat4(1.0f), glm::vec3(5.2f, 0.2f, 4.0f), down, 100.0f));
}
