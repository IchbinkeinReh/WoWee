// Where the client puts a unit's name and plate, and which plates it shows
// and dims (0x0071fef0, 0x00715720, 0x0072b060, 0x0098e9f0).
#include <catch_amalgamated.hpp>

#include "rendering/unit_name_anchor.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace una = wowee::rendering::unit_name_anchor;

TEST_CASE("the name's attachments are PlayerName and PlayerNameMounted", "[unit_name_anchor]") {
    CHECK(una::kAttachmentPlayerName == 18);
    CHECK(una::kAttachmentPlayerNameMounted == 29);
}

TEST_CASE("without an attachment the name is 1.25 heights up", "[unit_name_anchor]") {
    const glm::vec3 p = una::fallbackNamePosition({1.0f, 2.0f, 3.0f}, 2.0f, 1.5f);
    CHECK(p.x == 1.0f);
    CHECK(p.y == 2.0f);
    CHECK(p.z == Catch::Approx(3.0f + 2.0f * 1.5f * 1.25f));
}

TEST_CASE("a plate stands two thirds of a yard over the name", "[unit_name_anchor]") {
    CHECK(una::kPlateLift == Catch::Approx(2.0f / 3.0f));
}

TEST_CASE("plates show within 41 yards of the player", "[unit_name_anchor]") {
    const glm::vec3 player(10.0f, 10.0f, 0.0f);
    CHECK(una::plateInRange(player, player + glm::vec3(41.0f, 0.0f, 0.0f)));
    CHECK_FALSE(una::plateInRange(player, player + glm::vec3(41.01f, 0.0f, 0.0f)));
    CHECK_FALSE(una::plateInRange(player, player + glm::vec3(30.0f, 30.0f, 0.0f)));
    CHECK(una::plateInRange(player, player + glm::vec3(0.0f, 20.0f, 30.0f)));
}

TEST_CASE("with a target the other plates are at half alpha", "[unit_name_anchor]") {
    CHECK(una::plateAlpha(false, false) == 1.0f);
    CHECK(una::plateAlpha(true, true) == 1.0f);
    CHECK(una::plateAlpha(true, false) == Catch::Approx(127.0f / 255.0f));
}

TEST_CASE("a name's glyph stands in the world where it was laid out", "[unit_name_anchor]") {
    // The camera's projection, the Vulkan turn of y included (camera.cpp).
    const float screenW = 1920.0f, screenH = 1080.0f;
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), screenW / screenH, 0.2f, 1000.0f);
    proj[1][1] *= -1.0f;
    const glm::mat4 view = glm::lookAt(glm::vec3(3.0f, -12.0f, 4.0f), glm::vec3(0.0f, 0.0f, 2.0f),
                                       glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::vec3 anchor(0.5f, 0.25f, 2.2f);
    auto toScreen = [&](const glm::vec4& clip) {
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        return glm::vec2((ndc.x * 0.5f + 0.5f) * screenW, (ndc.y * 0.5f + 0.5f) * screenH);
    };
    const glm::vec4 anchorClip = proj * view * glm::vec4(anchor, 1.0f);
    const glm::vec2 anchorPx = toScreen(anchorClip);
    const float k = una::pixelsPerYard(proj[1][1], screenH, anchorClip.w);

    // A corner up and to the right of the anchor, as the font lays one out.
    const glm::vec2 cornerPx = anchorPx + glm::vec2(37.0f, -21.0f);
    const glm::vec2 offset = una::billboardOffset(cornerPx, anchorPx, k);
    CHECK(offset.x > 0.0f);
    CHECK(offset.y > 0.0f);  // up the screen is up the view

    // What world_text.vert does with it.
    glm::vec4 viewPos = view * glm::vec4(anchor, 1.0f);
    viewPos.x += offset.x;
    viewPos.y += offset.y;
    const glm::vec4 clip = proj * viewPos;
    const glm::vec2 back = toScreen(clip);
    CHECK(back.x == Catch::Approx(cornerPx.x).margin(0.01));
    CHECK(back.y == Catch::Approx(cornerPx.y).margin(0.01));
    // At the anchor's depth, so the scene's depth decides it as it does the
    // unit's.
    CHECK(clip.z / clip.w == Catch::Approx(anchorClip.z / anchorClip.w).margin(1e-6));
}

TEST_CASE("a yard of name is as many pixels as its height says", "[unit_name_anchor]") {
    // The projection's [1][1] is negative under the Vulkan turn; the size is not.
    CHECK(una::pixelsPerYard(-1.5f, 1000.0f, 10.0f) == Catch::Approx(75.0f));
    CHECK(una::pixelsPerYard(1.5f, 1000.0f, 10.0f) == Catch::Approx(75.0f));
    CHECK(una::pixelsPerYard(1.5f, 1000.0f, 20.0f) == Catch::Approx(37.5f));
}
