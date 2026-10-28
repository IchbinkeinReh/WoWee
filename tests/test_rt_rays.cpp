// Rays a pixel of each kind (rt_rays.hpp): one by default, held to 1-5.
#include <catch_amalgamated.hpp>
#include "rendering/rt_rays.hpp"

using namespace wowee::rendering;

TEST_CASE("one ray of each kind by default, the trace as it always ran", "[rt_rays]") {
    const RtRaysPerPixel rays;
    CHECK(rays.sun == 1);
    CHECK(rays.occlusion == 1);
    CHECK(rays.pointLights == 1);
    CHECK(rays.bounce == 1);
}

TEST_CASE("a ray count is held to one to five", "[rt_rays]") {
    CHECK(kMaxRtRaysPerPixel == 5);
    CHECK(clampRtRays(0) == 1);
    CHECK(clampRtRays(-3) == 1);
    CHECK(clampRtRays(1) == 1);
    CHECK(clampRtRays(3) == 3);
    CHECK(clampRtRays(5) == 5);
    CHECK(clampRtRays(9) == 5);
}
