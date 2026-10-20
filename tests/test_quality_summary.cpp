// The settings line /perf shows (rendering/quality_summary.hpp).
#include <catch_amalgamated.hpp>

#include "rendering/quality_summary.hpp"

using wowee::rendering::QualitySummary;
using wowee::rendering::formatQualitySummary;

namespace {
bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
}  // namespace

TEST_CASE("a phone on Low with FSR 1 says so", "[quality_summary]") {
    QualitySummary q;
    q.preset = "Low";
    q.outputWidth = 2400;
    q.outputHeight = 1080;
    q.sceneWidth = 1848;
    q.sceneHeight = 832;
    q.upscaler = 1;
    q.msaaSamples = 1;
    q.shadows = false;
    q.viewDistance = 177.0f;
    q.lowCostShading = true;
    const std::string line = formatQualitySummary(q);
    INFO(line);
    CHECK(has(line, "preset Low"));
    CHECK(has(line, "1848x832 of 2400x1080 (77%)"));
    CHECK(has(line, "upscaling FSR 1"));
    CHECK(has(line, "shadows off"));
    CHECK(has(line, "view 177 yd"));
    CHECK(has(line, "low-cost shading on"));
}

TEST_CASE("native resolution and shadows on read as such", "[quality_summary]") {
    QualitySummary q;
    q.preset = "Custom (from Low)";
    q.outputWidth = q.sceneWidth = 1920;
    q.outputHeight = q.sceneHeight = 1080;
    q.upscaler = 0;
    q.msaaSamples = 4;
    q.shadows = true;
    q.shadowCascades = 2;
    q.shadowDistance = 300.0f;
    const std::string line = formatQualitySummary(q);
    INFO(line);
    CHECK(has(line, "preset Custom (from Low)"));
    CHECK(has(line, "(100%)"));
    CHECK(has(line, "upscaling off"));
    CHECK(has(line, "MSAA 4x"));
    CHECK(has(line, "shadows on, 2 cascades to 300 yd"));
}

TEST_CASE("nothing known yet still makes a line", "[quality_summary]") {
    const std::string line = formatQualitySummary(QualitySummary{});
    CHECK(has(line, "preset ?"));
    CHECK(has(line, "(?)"));
}
