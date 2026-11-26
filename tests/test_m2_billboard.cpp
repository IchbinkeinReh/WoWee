// M2 billboard bones (0x0082f0f0's switch on flags & 0x78). U15: the mage
// portal's starry plane (MagePortal_Ogrimmar.m2, bone 0 flagged 0x40) stayed
// a fixed quad seen at an angle; a cylindrical bone turns about its own axis
// to the camera.
#include <catch_amalgamated.hpp>

#include "rendering/m2_billboard.hpp"

using namespace wowee::rendering;

namespace {
bool near(const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b) < 1e-4f; }
}  // namespace

TEST_CASE("a Z-cylindrical bone keeps its Z and faces the camera round it", "[m2_billboard]") {
    const glm::mat3 authored(1.0f);
    const glm::vec3 pivot(0.0f);
    // A camera off to the side and above: the card's +X turns to it in the
    // ground plane, Z stays up.
    const glm::mat3 m = billboardCylinder(kM2BoneCylindricalZ, authored, glm::vec3(0.0f, 10.0f, 5.0f), pivot);
    CHECK(near(m[2], {0, 0, 1}));
    CHECK(near(m[0], {0, 1, 0}));
    CHECK(near(m[1], {-1, 0, 0}));
    // Right handed, as the authored frame is.
    CHECK(glm::determinant(m) == Catch::Approx(1.0f));
    // Walking round it: always facing.
    for (float a = 0.0f; a < 6.28f; a += 0.7f) {
        const glm::vec3 cam(std::cos(a) * 20.0f, std::sin(a) * 20.0f, 3.0f);
        const glm::mat3 r = billboardCylinder(kM2BoneCylindricalZ, authored, cam, pivot);
        CHECK(near(r[0], glm::normalize(glm::vec3(cam.x, cam.y, 0.0f))));
    }
}

TEST_CASE("the X and Y cylinders keep their own axis", "[m2_billboard]") {
    const glm::mat3 authored(1.0f);
    const glm::vec3 cam(3.0f, 4.0f, 12.0f);
    const glm::mat3 x = billboardCylinder(kM2BoneCylindricalX, authored, cam, glm::vec3(0.0f));
    CHECK(near(x[0], {1, 0, 0}));
    CHECK(glm::determinant(x) == Catch::Approx(1.0f));
    const glm::mat3 y = billboardCylinder(kM2BoneCylindricalY, authored, cam, glm::vec3(0.0f));
    CHECK(near(y[1], {0, 1, 0}));
    CHECK(glm::determinant(y) == Catch::Approx(1.0f));
}

TEST_CASE("a camera along the axis, or no cylinder flag, keeps the authored axes", "[m2_billboard]") {
    const glm::mat3 authored(1.0f);
    CHECK(billboardCylinder(kM2BoneCylindricalZ, authored, glm::vec3(0, 0, 9), glm::vec3(0.0f)) == authored);
    CHECK(billboardCylinder(kM2BoneSphericalBillboard, authored, glm::vec3(5, 0, 0), glm::vec3(0.0f)) == authored);
}
