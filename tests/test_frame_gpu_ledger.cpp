// What a frame handed the GPU, the phone's upload cap (rendering/frame_gpu_ledger.hpp)
// and the slowest step of a stage (core/slow_step.hpp) - U25.
#include "catch_amalgamated.hpp"
#include "core/slow_step.hpp"
#include "rendering/frame_gpu_ledger.hpp"

#include <string>
#include <thread>

using wowee::rendering::FrameGpuLedger;

TEST_CASE("the ledger closes a frame's counts, from any thread", "[frame_gpu_ledger]") {
    FrameGpuLedger ledger;
    ledger.addUploadBytes(3u << 20);
    std::thread worker([&] {
        ledger.addUploadBytes(1u << 20);
        ledger.addSubmit();
    });
    worker.join();
    ledger.addUploadBatch();
    ledger.addRtTriangles(150000);
    ledger.addRtPoolGrow();
    CHECK(ledger.uploadBytesThisFrame() == (4u << 20));
    const auto closed = ledger.closeFrame();
    CHECK(closed.uploadBytes == (4u << 20));
    CHECK(closed.submits == 1);
    CHECK(ledger.uploadBytesThisFrame() == 0);
    CHECK(ledger.lastFrame().rtTriangles == 150000);
    CHECK(ledger.lastFrame().describe() ==
          "4 MB staged in 1 upload batches, 1 submits, 150000 traced-lighting triangles, 1 pool grows");
    ledger.closeFrame();
    CHECK(ledger.frameBeforeLast().uploadBatches == 1);
    CHECK(ledger.lastFrame().uploadBatches == 0);
}

TEST_CASE("the upload cap: a phone's default, the override, none on a desktop", "[frame_gpu_ledger]") {
    CHECK(FrameGpuLedger::defaultCapBytes(true, nullptr) == (48ull << 20));
    CHECK(FrameGpuLedger::defaultCapBytes(false, nullptr) == 0);
    CHECK(FrameGpuLedger::defaultCapBytes(false, "") == 0);
    CHECK(FrameGpuLedger::defaultCapBytes(true, "0") == 0);
    CHECK(FrameGpuLedger::defaultCapBytes(false, "16") == (16ull << 20));
    CHECK(FrameGpuLedger::defaultCapBytes(true, "x") == (48ull << 20));

    FrameGpuLedger ledger;
    CHECK_FALSE(ledger.overUploadCap());  // no cap
    ledger.addUploadBytes(1ull << 30);
    CHECK_FALSE(ledger.overUploadCap());
    ledger.closeFrame();
    ledger.setCapBytes(8u << 20);
    ledger.addUploadBytes(7u << 20);
    CHECK_FALSE(ledger.overUploadCap());
    ledger.addUploadBytes(1u << 20);
    CHECK(ledger.overUploadCap());
    ledger.closeFrame();  // a new frame starts under the cap again
    CHECK_FALSE(ledger.overUploadCap());
}

TEST_CASE("a long GPU wait is said the first time, when worse, or 30 s on", "[frame_gpu_ledger]") {
    FrameGpuLedger ledger;
    CHECK_FALSE(ledger.shouldReport(62.0, 1.0));   // every frame of the U25 log
    CHECK(ledger.shouldReport(844.0, 2.0));         // the frame before the loss
    CHECK_FALSE(ledger.shouldReport(400.0, 3.0));
    CHECK(ledger.shouldReport(900.0, 4.0));         // worse
    CHECK_FALSE(ledger.shouldReport(300.0, 20.0));
    CHECK(ledger.shouldReport(300.0, 40.0));        // 30 s after the last line
}

TEST_CASE("the slowest step of a stage, the inner one when it holds the time", "[slow_step]") {
    using namespace wowee::core;
    (void)takeSlowestStep();
    offerStep("sky", 1.0f);
    offerStep("lighting: wmo interior pass", 500.0f);
    offerStep("lighting", 510.0f);  // encloses it, hardly longer
    offerStep("footsteps", 0.5f);
    SlowestStep s = takeSlowestStep();
    REQUIRE(s.name != nullptr);
    CHECK(std::string(s.name) == "lighting: wmo interior pass");
    CHECK(s.ms == 500.0f);
    CHECK(takeSlowestStep().name == nullptr);  // taken clears it

    offerStep("a", 10.0f);
    offerStep("b", 100.0f);
    CHECK(std::string(takeSlowestStep().name) == "b");
    {
        StepTimer t("timed");
    }
    CHECK(std::string(takeSlowestStep().name) == "timed");
}
