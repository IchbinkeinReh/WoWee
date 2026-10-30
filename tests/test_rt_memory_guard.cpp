// Whether there is memory for the ray tracing (rt_memory_guard.hpp).
#include <catch_amalgamated.hpp>

#include "rendering/rt_memory_guard.hpp"

using namespace wowee::rendering::rt_memory;

TEST_CASE("the estimate matches what /mem measured", "[rt_memory]") {
    // Orgrimmar at 1080p, 8x MSAA, Full mode, hardware tracer: 5.75 million
    // triangles, and /mem said 1122 MB on the GPU.
    const Need n = estimate(5'750'000, 1920, 1080, 8, true, true, true);
    CHECK(n.gpu / kMiB > 1050);
    CHECK(n.gpu / kMiB < 1200);
    // Half resolution, no MSAA, no probes: a phone's settings take far less.
    const Need phone = estimate(5'750'000, 1280, 720, 1, false, false, false);
    CHECK(phone.gpu < n.gpu * 6 / 10);
}

TEST_CASE("switching on is refused where the graphics budget will not hold it", "[rt_memory]") {
    Snapshot s;
    s.gpuBudget = 2000 * kMiB;
    s.gpuUsage = 1500 * kMiB;
    s.ramTotal = 16384 * kMiB;
    s.ramAvailable = 8000 * kMiB;
    Need n;
    n.gpu = 600 * kMiB;
    const Verdict v = judgeEnable(s, n);
    CHECK(v.what == Short::Gpu);
    CHECK(v.needed == 600 * kMiB);
    CHECK(v.available == 500 * kMiB);
    n.gpu = 400 * kMiB;
    CHECK(judgeEnable(s, n).what == Short::None);
}

TEST_CASE("on shared memory the GPU's share comes out of the RAM", "[rt_memory]") {
    Snapshot s;  // no budget known
    s.ramTotal = 8192 * kMiB;
    s.ramAvailable = 1600 * kMiB;  // 1024 reserved: 576 to spare
    Need n;
    n.gpu = 500 * kMiB;
    n.ram = 50 * kMiB;
    CHECK(judgeEnable(s, n).what == Short::None);  // a discrete card's memory is its own
    s.unified = true;
    const Verdict v = judgeEnable(s, n);
    CHECK(v.what == Short::None);
    n.gpu = 600 * kMiB;
    const Verdict w = judgeEnable(s, n);
    CHECK(w.what == Short::Ram);
    CHECK(w.needed == 650 * kMiB);
    CHECK(w.available == 1600 * kMiB);
}

TEST_CASE("the system saying it is short refuses and offers", "[rt_memory]") {
    Snapshot s;
    s.ramTotal = 8192 * kMiB;
    s.ramAvailable = 6000 * kMiB;
    s.systemLow = true;
    CHECK(judgeEnable(s, Need{}).what == Short::Ram);
    CHECK(ramCritical(s));
    s.systemLow = false;
    CHECK_FALSE(ramCritical(s));
    s.ramAvailable = 400 * kMiB;  // under half the 1024 reserved
    CHECK(ramCritical(s));
    CHECK(ramReserve(1024 * kMiB) == 256 * kMiB);
}

TEST_CASE("the dialog says what ran out and how much, in German for deDE", "[rt_memory]") {
    Refusal r;
    r.reason = Reason::GpuBudget;
    r.needed = 600 * kMiB;
    r.available = 500 * kMiB;
    const std::string en = message(r, false);
    CHECK(en.find("graphics memory") != std::string::npos);
    CHECK(en.find("600 MB") != std::string::npos);
    CHECK(en.find("500 MB") != std::string::npos);
    const std::string de = message(r, true);
    CHECK(de.find("Grafikspeicher") != std::string::npos);
    CHECK(de.find("600 MB") != std::string::npos);
    for (Reason why : {Reason::RamShort, Reason::OutOfDeviceMemory, Reason::OutOfHostMemory, Reason::RamCritical,
                       Reason::PreviousSession}) {
        r.reason = why;
        CHECK_FALSE(message(r, false).empty());
        CHECK_FALSE(message(r, true).empty());
        CHECK(message(r, true) != message(r, false));
    }
}

#include "core/rt_session_marker.hpp"
#include <cstdio>
#include <filesystem>

TEST_CASE("the session marker keeps the last numbers and a clean exit takes it away", "[rt_memory]") {
    namespace rs = wowee::core::rt_session;
    const std::string file = (std::filesystem::temp_directory_path() / "wowee_rt_session_test.marker").string();
    rs::clear(file);
    CHECK_FALSE(rs::read(file).has_value());
    rs::write(file, {1234, 900, 4321});
    const auto n = rs::read(file);
    REQUIRE(n.has_value());
    CHECK(n->availableMB == 1234);
    CHECK(n->tracingMB == 900);
    CHECK(n->privateMB == 4321);
    rs::clear(file);
    CHECK_FALSE(rs::read(file).has_value());
}
