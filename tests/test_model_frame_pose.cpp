// A UI model frame's pose and screen (0x0095fba0, 0x0047bf90).
#include <catch_amalgamated.hpp>

#include "rendering/figure_camera.hpp"
#include "rendering/model_frame_view.hpp"
#include "ui/widget_tree.hpp"

namespace mf = wowee::rendering::model_frame;

TEST_CASE("the screen's DDC height: a diagonal of 1 (0x0047bf90)", "[model_frame]") {
    CHECK(mf::ddcHeight(4.0f / 3.0f) == Catch::Approx(0.6f));
    CHECK(mf::ddcHeight(16.0f / 9.0f) == Catch::Approx(0.4903f).epsilon(0.001));
}

TEST_CASE("a model frame's pose (0x0095fba0)", "[model_frame]") {
    mf::Pose pose;
    pose.position = {0.1f, 0.2f, 0.0f};
    pose.effectiveScale = 2.0f;
    const glm::mat4 m = mf::modelMatrix(pose, 0.6f);
    CHECK(m[3].x == Catch::Approx(0.2f));
    CHECK(m[3].y == Catch::Approx(0.4f));
    // ModelScale 1, effective scale 2, at 4:3: twice the model's units.
    CHECK(m[0].x == Catch::Approx(2.0f).epsilon(0.001));
    // A quarter turn of facing takes the model's X to the frame's Y.
    pose.facing = 1.5707964f;
    const glm::vec4 x = mf::modelMatrix(pose, 0.6f) * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    CHECK(x.x == Catch::Approx(0.0f).margin(1e-4));
    CHECK(x.y == Catch::Approx(2.0f).epsilon(0.001));
}

namespace {
struct PerFrame {
    glm::vec4 lightDir, lightColor, ambientColor, fogColor, fogParams;
};
}  // namespace

// 0x0095f680, 0x00597370
TEST_CASE("a PlayerModel has a light of its own, a Model none", "[model_frame]") {
    wowee::ui::Widget w;
    w.objectType = "Model";
    w.takePlayerModelLight();
    mf::Light light = mf::lightOf(w);
    CHECK_FALSE(light.enabled);
    CHECK(light.omni);
    CHECK(light.ambient == glm::vec3(1.0f));
    CHECK(light.diffuse == glm::vec3(1.0f));
    CHECK(mf::lighting(light).ambient == glm::vec3(0.0f));
    CHECK(mf::lighting(light).diffuse == glm::vec3(0.0f));
    for (const char* type : {"PlayerModel", "DressUpModel", "TabardModel"}) {
        w = {};
        w.objectType = type;
        w.takePlayerModelLight();
        light = mf::lightOf(w);
        CHECK(light.enabled);
        CHECK_FALSE(light.omni);
        CHECK(mf::lighting(light).direction == glm::vec3(0.0f, 1.0f, 0.0f));
        for (int i = 0; i < 3; ++i) {
            CHECK(mf::lighting(light).ambient[i] == Catch::Approx(0.7f));
            CHECK(mf::lighting(light).diffuse[i] == Catch::Approx(i < 2 ? 0.8f : 0.64f));
        }
    }
}

TEST_CASE("a model frame's light and fog as the shaders take them", "[model_frame]") {
    mf::Light light{.enabled = true, .omni = true, .vec = {0.0f, 0.0f, 2.0f}, .ambient = glm::vec3(0.1f),
                    .diffuse = {0.2f, 0.3f, 0.4f}};
    // A point light shines from where it is, as far as it reaches, and adds no ambient.
    CHECK(mf::lighting(light).direction == glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(mf::lighting(light).diffuse.g == Catch::Approx(0.3f / (0.7f * 2.0f + 0.03f * 4.0f)));
    CHECK(mf::lighting(light).ambient == glm::vec3(0.0f));
    light.vec = glm::vec3(0.0f);
    CHECK(mf::lighting(light).diffuse == glm::vec3(0.0f));
    light.omni = false;
    light.vec = {0.0f, 0.0f, 1.0f};
    wowee::ui::Widget w;
    w.modelFogEnabled = true;
    w.modelFogColor[1] = 1.0f;
    w.modelFogNear = 0.5f;
    w.modelFogFar = 4.0f;
    PerFrame f{};
    mf::put(light, mf::fogOf(w), f);
    CHECK(f.lightDir == glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
    for (int i = 0; i < 3; ++i) {
        CHECK(f.lightColor[i] == Catch::Approx(light.diffuse[i]));
        CHECK(f.ambientColor[i] == Catch::Approx(0.1f));
    }
    CHECK(f.fogColor == glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
    CHECK(f.fogParams == glm::vec4(0.5f, 4.0f, 0.0f, 0.0f));
    // In a figure's own units, of which there are more to the frame's.
    const float unit = wowee::rendering::figure_camera::unit(1.0f, 0.6f);
    CHECK(unit == Catch::Approx(1.0f));
    mf::put(light, mf::fogOf(w, 2.0f * unit), f);
    CHECK(f.fogParams.x == Catch::Approx(0.25f));
    CHECK(f.fogParams.y == Catch::Approx(2.0f));
    w.modelFogEnabled = false;
    mf::put(mf::Light{}, mf::fogOf(w), f);
    CHECK(f.fogParams.x > 1000.0f);
    CHECK(f.lightColor == glm::vec4(0.0f));
}

// 0x00834dc0, 0x00835280
TEST_CASE("directional lights are merged into one", "[model_frame]") {
    namespace ml = wowee::rendering::model_lights;
    // One, along a vector of 1: itself.
    ml::Sum one;
    one.addAmbient(glm::vec3(0.25f));
    one.addDirectional({1.0f, 0.5f, 0.0f}, {0.0f, -1.0f, 0.0f});
    CHECK(one.merged().direction == glm::vec3(0.0f, -1.0f, 0.0f));
    CHECK(one.merged().diffuse.r == Catch::Approx(1.0f));
    CHECK(one.merged().diffuse.g == Catch::Approx(0.5f));
    CHECK(one.merged().ambient.r == Catch::Approx(0.25f));
    // Two at a right angle, one red and one green: the direction is by their brightness, nearly
    // the green one's, and a quarter of what goes another way is ambient.
    ml::Sum two;
    two.addDirectional({1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
    two.addDirectional({0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    const ml::Lighting l = two.merged();
    const float len = std::sqrt(0.212671f * 0.212671f + 0.71516f * 0.71516f);
    const float red = 0.212671f / len, green = 0.71516f / len;
    CHECK(l.direction.x == Catch::Approx(red));
    CHECK(l.direction.y == Catch::Approx(green));
    CHECK(l.diffuse.r == Catch::Approx(red * 1.25f - 0.25f));
    CHECK(l.diffuse.g == Catch::Approx(green * 1.25f - 0.25f));
    CHECK(l.diffuse.b == 0.0f);
    CHECK(l.ambient.r == Catch::Approx((1.0f - red) * 0.25f));
    CHECK(l.ambient.g == Catch::Approx((1.0f - green) * 0.25f));
    // None: as 0x00835280 leaves it.
    CHECK(ml::Sum{}.merged().direction == glm::vec3(0.0f, 0.0f, -1.0f));
}

// 0x00616bc0: 0.45 all about, and white along (-1, 0, -1), which is longer than 1.
TEST_CASE("a unit's portrait has a light of its own", "[model_frame]") {
    const auto l = wowee::rendering::model_lights::portrait();
    CHECK(l.direction.x == Catch::Approx(-0.70710678f));
    CHECK(l.direction.y == 0.0f);
    CHECK(l.direction.z == Catch::Approx(-0.70710678f));
    CHECK(l.diffuse.r == Catch::Approx(1.5177670f));
    CHECK(l.ambient.b == Catch::Approx(0.3464466f));
}
