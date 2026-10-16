// Whether the ray traced lighting can run on a device, and what it lacks when
// it cannot (rendering/rt_capability.hpp). The settings grey the ray tracing
// rows out with this reason and the renderer keeps the lighting off.
#include <catch_amalgamated.hpp>

#include "rendering/rt_capability.hpp"

namespace rc = wowee::rendering::rt_capability;

namespace {

// What the RTX 2070 SUPER here reports.
rc::Input desktop() {
    rc::Input in;
    in.vulkan12 = true;
    in.accelerationStructureExt = in.rayQueryExt = in.deferredHostOpsExt = true;
    in.bufferDeviceAddress = in.accelerationStructure = in.rayQuery = true;
    in.dualSrcBlend = in.independentBlend = in.sampleRateShading = true;
    in.gbufferSampleCounts = 1 | 2 | 4 | 8;
    in.storageImageRgba16f = true;
    in.maxPerStageStorageBuffers = 1048576;
    in.maxPerStageStorageImages = 1048576;
    in.maxComputeWorkGroupInvocations = 1024;
    return in;
}

// A phone GPU with no ray queries and no dual-source blending.
rc::Input phone() {
    rc::Input in = desktop();
    in.accelerationStructureExt = in.rayQueryExt = in.deferredHostOpsExt = false;
    in.bufferDeviceAddress = in.accelerationStructure = in.rayQuery = false;
    in.dualSrcBlend = false;
    in.gbufferSampleCounts = 1 | 4;
    in.maxPerStageStorageBuffers = 35;
    in.maxPerStageStorageImages = 16;
    in.maxComputeWorkGroupInvocations = 512;
    return in;
}

}  // namespace

TEST_CASE("A device with everything traces in hardware at every sample count", "[rt_capability]") {
    for (uint32_t s : {1u, 2u, 4u, 8u}) {
        const rc::Result r = rc::evaluate(desktop(), s);
        CHECK(r.available);
        CHECK(r.tracer == rc::Tracer::Hardware);
        CHECK(r.missing.empty());
        CHECK_FALSE(r.blockedByMsaa);
    }
    CHECK(rc::describe(rc::evaluate(desktop(), 1)) == "available (hardware ray queries)");
}

TEST_CASE("Without ray queries the compute tracer runs", "[rt_capability]") {
    rc::Input in = desktop();
    in.rayQuery = false;
    const rc::Result r = rc::evaluate(in, 1);
    CHECK(r.available);
    CHECK(r.tracer == rc::Tracer::Software);
    CHECK(r.hardwareWhyNot == "no rayQuery feature");
    CHECK(rc::describe(r) == "available (compute tracer: no rayQuery feature)");
}

TEST_CASE("Listed extensions are not enough without their features", "[rt_capability]") {
    rc::Input in = desktop();
    in.accelerationStructure = false;
    std::string why;
    CHECK_FALSE(rc::hardwareTracerUsable(in, &why));
    CHECK(why == "no accelerationStructure feature");
    in = desktop();
    in.bufferDeviceAddress = false;
    CHECK_FALSE(rc::hardwareTracerUsable(in, &why));
    CHECK(why == "no bufferDeviceAddress");
    in = desktop();
    in.vulkan12 = false;
    CHECK_FALSE(rc::hardwareTracerUsable(in, &why));
}

TEST_CASE("WOWEE_RT_FORCE_SOFTWARE keeps the hardware tracer off", "[rt_capability]") {
    rc::Input in = desktop();
    in.forceSoftware = true;
    const rc::Result r = rc::evaluate(in, 4);
    CHECK(r.available);
    CHECK(r.tracer == rc::Tracer::Software);
    CHECK(r.hardwareWhyNot == "WOWEE_RT_FORCE_SOFTWARE");
}

TEST_CASE("No dual-source blending: unavailable, named", "[rt_capability]") {
    const rc::Result r = rc::evaluate(phone(), 1);
    CHECK_FALSE(r.available);
    CHECK(r.tracer == rc::Tracer::None);
    CHECK(r.missing == "dualSrcBlend");
    CHECK_FALSE(r.blockedByMsaa);
    CHECK(rc::describe(r) == "unavailable - missing dualSrcBlend");
}

TEST_CASE("Every missing piece is named, not just the first", "[rt_capability]") {
    rc::Input in = phone();
    in.independentBlend = false;
    in.storageImageRgba16f = false;
    const rc::Result r = rc::evaluate(in, 1);
    CHECK(r.missing == "rgba16f storage images, dualSrcBlend, independentBlend");
}

TEST_CASE("Shading per sample matters only under MSAA", "[rt_capability]") {
    rc::Input in = desktop();
    in.sampleRateShading = false;
    CHECK(rc::evaluate(in, 1).available);
    const rc::Result r = rc::evaluate(in, 4);
    CHECK_FALSE(r.available);
    CHECK(r.blockedByMsaa);
    CHECK(r.missing == "sampleRateShading (needed with MSAA)");
    CHECK(rc::describe(r) ==
          "unavailable - missing sampleRateShading (needed with MSAA) (available with anti-aliasing off)");
}

TEST_CASE("The G-buffer format at the scene's sample count", "[rt_capability]") {
    rc::Input in = desktop();
    in.gbufferSampleCounts = 1 | 4;
    CHECK(rc::evaluate(in, 4).available);
    const rc::Result at8 = rc::evaluate(in, 8);
    CHECK_FALSE(at8.available);
    CHECK(at8.blockedByMsaa);
    CHECK(at8.missing == "G-buffer format at 8x MSAA");

    in.gbufferSampleCounts = 0;
    const rc::Result none = rc::evaluate(in, 1);
    CHECK_FALSE(none.available);
    CHECK_FALSE(none.blockedByMsaa);
    CHECK(none.missing == "G-buffer format (rgba16 unorm) as a colour attachment");
}

TEST_CASE("The software BVH needs more storage buffers than the hardware tracer", "[rt_capability]") {
    rc::Input in = desktop();
    in.maxPerStageStorageBuffers = rc::kStorageBuffersHardware;
    CHECK(rc::evaluate(in, 1).tracer == rc::Tracer::Hardware);

    in.rayQuery = false;  // now the software tracer, which needs ten
    const rc::Result r = rc::evaluate(in, 1);
    CHECK_FALSE(r.available);
    CHECK(r.missing == "10 storage buffers per stage (software BVH)");

    in.maxPerStageStorageBuffers = rc::kStorageBuffersSoftware;
    CHECK(rc::evaluate(in, 1).available);
}

TEST_CASE("The compute limits", "[rt_capability]") {
    rc::Input in = desktop();
    in.maxComputeWorkGroupInvocations = 128;  // the spec's minimum
    in.maxPerStageStorageImages = 4;          // likewise
    const rc::Result r = rc::evaluate(in, 1);
    CHECK_FALSE(r.available);
    CHECK(r.missing == "5 storage images per stage, 256-invocation compute workgroups");
}

TEST_CASE("What the settings say under a greyed row", "[rt_capability]") {
    CHECK(rc::settingsReason(rc::evaluate(desktop(), 8)).empty());
    CHECK(rc::settingsReason(rc::evaluate(phone(), 1)) ==
          "Unavailable on this graphics card: it lacks dualSrcBlend.");
    rc::Input in = desktop();
    in.sampleRateShading = false;
    CHECK(rc::settingsReason(rc::evaluate(in, 2)) ==
          "Unavailable with anti-aliasing (MSAA) on: this graphics card lacks sampleRateShading "
          "(needed with MSAA). Turn anti-aliasing off to use it.");
}

TEST_CASE("A sample count of 0 reads as 1", "[rt_capability]") {
    CHECK(rc::evaluate(desktop(), 0).available);
}
