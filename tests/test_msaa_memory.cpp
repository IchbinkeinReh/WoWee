// Whether the multisampled scene fits before the anti-aliasing is raised
// (msaa_memory.hpp, U10).
#include <catch_amalgamated.hpp>

#include "rendering/msaa_memory.hpp"

using namespace wowee::rendering;
using rt_memory::kMiB;

namespace {
// The user's phone: 2622x1206, the app's own headroom as iOS says it.
rt_memory::Snapshot phone(uint64_t headroomMiB) {
    rt_memory::Snapshot s;
    s.unified = true;
    s.ramTotal = 11722 * kMiB;
    s.ramAvailable = headroomMiB * kMiB;
    s.ramIsAppHeadroom = true;
    return s;
}
}  // namespace

TEST_CASE("the multisampled targets by size, samples and G-buffer", "[msaa_memory]") {
    // 2622x1206 at 8x: some 193 MB; with the traced lighting's G-buffer three times that.
    CHECK(msaa_memory::targetBytes(2622, 1206, 8, false) / kMiB == 193);
    CHECK(msaa_memory::targetBytes(2622, 1206, 8, true) / kMiB == 579);
    CHECK(msaa_memory::targetBytes(2622, 1206, 1, false) == msaa_memory::targetBytes(2622, 1206, 0, false));
}

TEST_CASE("a raise is held to what fits, a step down always taken", "[msaa_memory]") {
    // Plenty: 8x as asked.
    CHECK(msaa_memory::samplesThatFit(8, 1, 2622, 1206, false, phone(2000)) == 8);
    // 300 MB of headroom, 140 usable above the margin: 8x adds 168 MB, 4x 72.
    CHECK(msaa_memory::samplesThatFit(8, 1, 2622, 1206, false, phone(300)) == 4);
    // With the G-buffer 4x does not fit either; 2x adds 72 MB.
    CHECK(msaa_memory::samplesThatFit(8, 1, 2622, 1206, true, phone(300)) == 2);
    // Nothing usable: kept where it is.
    CHECK(msaa_memory::samplesThatFit(8, 2, 2622, 1206, false, phone(100)) == 2);
    // A step down whatever the memory.
    CHECK(msaa_memory::samplesThatFit(2, 8, 2622, 1206, true, phone(0)) == 2);
}

TEST_CASE("no raise while the system says it is short", "[msaa_memory]") {
    auto s = phone(4000);
    s.systemLow = true;
    CHECK(msaa_memory::samplesThatFit(8, 1, 2622, 1206, false, s) == 1);
    CHECK(msaa_memory::samplesThatFit(8, 4, 2622, 1206, false, s) == 4);
}

TEST_CASE("a discrete card answers by its own budget", "[msaa_memory]") {
    rt_memory::Snapshot s;
    s.gpuBudget = 8192 * kMiB;
    s.gpuUsage = 8100 * kMiB;  // 92 MB left: 2x adds 63, 4x 190
    s.ramTotal = 32768 * kMiB;
    s.ramAvailable = 100 * kMiB;  // not the card's memory
    CHECK(msaa_memory::samplesThatFit(8, 1, 3840, 2160, false, s) == 2);
    s.gpuUsage = 1000 * kMiB;
    CHECK(msaa_memory::samplesThatFit(8, 1, 3840, 2160, false, s) == 8);
}
