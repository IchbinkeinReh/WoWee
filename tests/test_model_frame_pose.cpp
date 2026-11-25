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
        CHECK(mf::lighting(light).ambient == glm::vec3(0.7f));
        CHECK(mf::lighting(light).diffuse == glm::vec3(0.8f, 0.8f, 0.64f));
    }
}

TEST_CASE("a model frame's light and fog as the shaders take them", "[model_frame]") {
    mf::Light light{.enabled = true, .omni = true, .vec = {0.0f, 0.0f, 2.0f}, .ambient = glm::vec3(0.1f),
                    .diffuse = {0.2f, 0.3f, 0.4f}};
    // A point light shines from where it is.
    CHECK(mf::lighting(light).direction == glm::vec3(0.0f, 0.0f, -1.0f));
    wowee::ui::Widget w;
    w.modelFogEnabled = true;
    w.modelFogColor[1] = 1.0f;
    w.modelFogNear = 0.5f;
    w.modelFogFar = 4.0f;
    PerFrame f{};
    mf::put(light, mf::fogOf(w), f);
    CHECK(f.lightColor == glm::vec4(0.2f, 0.3f, 0.4f, 0.0f));
    CHECK(f.ambientColor == glm::vec4(0.1f, 0.1f, 0.1f, 0.0f));
    CHECK(f.fogColor == glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
    CHECK(f.fogParams == glm::vec4(0.5f, 4.0f, 0.0f, 0.0f));
    // In a figure's own units, of which there are more to the frame's.
    const float unit = wowee::rendering::figure_camera::unit(1.0f, 0.6f);
    CHECK(unit == Catch::Approx(1.0f));
    mf::put(light, mf::fogOf(w, 2.0f * unit), f);
    CHECK(f.fogParams.x == Catch::Approx(0.25f));
    CHECK(f.fogParams.y == Catch::Approx(2.0f));
    w.modelFogEnabled = false;
    mf::put({}, mf::fogOf(w), f);
    CHECK(f.fogParams.x > 1000.0f);
    CHECK(f.lightColor == glm::vec4(0.0f));
}
