// A lost Vulkan device, as decisions.
//
// No device can be lost here - there is no GPU to reset and no driver to
// crash. What can be checked is what the client decides when one is: which
// results mean the device is gone, when it is rebuilt, when it waits for a
// window to build on, and when it stops trying and ends the run as it used to
// on every loss.
#include <catch_amalgamated.hpp>

#include "rendering/device_recovery.hpp"

using wowee::rendering::DeviceRecovery;
using wowee::rendering::GpuResultClass;
using wowee::rendering::classifyGpuResult;
using Action = DeviceRecovery::Action;
namespace code = wowee::rendering::vk_result_code;

TEST_CASE("only VK_ERROR_DEVICE_LOST asks for a new device", "[device_recovery]") {
    CHECK(classifyGpuResult(code::kDeviceLost) == GpuResultClass::DeviceLost);

    CHECK(classifyGpuResult(code::kSuccess) == GpuResultClass::Ok);
    CHECK(classifyGpuResult(code::kNotReady) == GpuResultClass::NotYet);
    CHECK(classifyGpuResult(code::kTimeout) == GpuResultClass::NotYet);
    // The swapchain's and the surface's own troubles are theirs to fix; the
    // device under them is fine.
    CHECK(classifyGpuResult(code::kSuboptimal) == GpuResultClass::Swapchain);
    CHECK(classifyGpuResult(code::kOutOfDate) == GpuResultClass::Swapchain);
    CHECK(classifyGpuResult(code::kSurfaceLost) == GpuResultClass::SurfaceLost);
    // VK_ERROR_OUT_OF_DEVICE_MEMORY: a failed call, not a lost device.
    CHECK(classifyGpuResult(-2) == GpuResultClass::Failed);
    // VK_EVENT_SET: a positive answer is never a failure.
    CHECK(classifyGpuResult(3) == GpuResultClass::Ok);
}

TEST_CASE("a lost device is rebuilt, and the loss is counted once", "[device_recovery]") {
    DeviceRecovery r;
    CHECK(r.next(true, 0.0) == Action::None);
    CHECK_FALSE(r.lost());

    r.deviceLost(10.0);
    // Every call after the first fails the same way and reports the same
    // loss; the main loop asks every frame.
    r.deviceLost(10.1);
    r.deviceLost(10.2);
    CHECK(r.lost());
    CHECK(r.lossesInWindow() == 1);
    CHECK(r.next(true, 10.2) == Action::Rebuild);

    r.rebuilt();
    CHECK_FALSE(r.lost());
    CHECK(r.recoveries() == 1);
    CHECK(r.next(true, 10.3) == Action::None);
}

TEST_CASE("nothing is rebuilt without a window to build on", "[device_recovery]") {
    DeviceRecovery r;
    r.deviceLost(0.0);
    // In the background, or back before Android has handed over a window:
    // waiting, which costs no attempt however long it lasts.
    for (int i = 0; i < 100; ++i) CHECK(r.next(false, i * 60.0) == Action::Wait);
    CHECK(r.attempts() == 0);
    CHECK(r.next(true, 6000.0) == Action::Rebuild);
}

TEST_CASE("a rebuild that fails is tried again later, then given up", "[device_recovery]") {
    DeviceRecovery r;
    r.deviceLost(0.0);

    double t = 0.0;
    double last = 0.0;
    for (uint32_t i = 0; i + 1 < DeviceRecovery::kMaxRebuildAttempts; ++i) {
        const double delay = r.rebuildFailed(t);
        // Growing, and never past the ceiling.
        CHECK(delay >= last);
        CHECK(delay <= DeviceRecovery::kMaxRetryDelaySeconds);
        last = delay;
        t += delay;
        CHECK(r.next(true, t) == Action::Rebuild);
    }
    CHECK(r.attempts() == DeviceRecovery::kMaxRebuildAttempts - 1);
    (void)r.rebuildFailed(t);
    CHECK(r.next(true, t) == Action::GiveUp);
    CHECK(r.next(false, t) == Action::GiveUp);
    CHECK(r.next(true, t + 1000.0) == Action::GiveUp);
}

TEST_CASE("the retry is a time to try at, not a sleep", "[device_recovery]") {
    // The main loop keeps reading the connection between attempts, and asks
    // next() every time round; until the delay has passed it is told to wait.
    DeviceRecovery r;
    r.deviceLost(100.0);
    const double delay = r.rebuildFailed(100.0);
    CHECK(r.retryAt() == Catch::Approx(100.0 + delay));
    CHECK(r.next(true, 100.0) == Action::Wait);
    CHECK(r.next(true, 100.0 + delay * 0.5) == Action::Wait);
    CHECK(r.next(true, 100.0 + delay) == Action::Rebuild);
    // Waiting, however often it is asked, is not another attempt.
    CHECK(r.attempts() == 1);

    // The delay passing does not make a window: in the background it waits
    // on, and is tried the first time round there is one.
    CHECK(r.next(false, 100.0 + delay + 30.0) == Action::Wait);
    CHECK(r.next(true, 100.0 + delay + 30.0) == Action::Rebuild);
}

TEST_CASE("the retry delay doubles from the first", "[device_recovery]") {
    DeviceRecovery r;
    r.deviceLost(0.0);
    CHECK(r.rebuildFailed(0.0) == Catch::Approx(DeviceRecovery::kFirstRetryDelaySeconds));
    CHECK(r.rebuildFailed(0.0) == Catch::Approx(DeviceRecovery::kFirstRetryDelaySeconds * 2.0));
    CHECK(r.rebuildFailed(0.0) == Catch::Approx(DeviceRecovery::kFirstRetryDelaySeconds * 4.0));
    for (int i = 0; i < 10; ++i) {
        CHECK(r.rebuildFailed(0.0) <= DeviceRecovery::kMaxRetryDelaySeconds);
    }
}

TEST_CASE("a new loss starts its attempts over", "[device_recovery]") {
    DeviceRecovery r;
    r.deviceLost(0.0);
    (void)r.rebuildFailed(0.0);
    (void)r.rebuildFailed(0.25);
    r.rebuilt();
    CHECK(r.attempts() == 0);

    // Nor does the last loss's retry time hold the next one back.
    r.deviceLost(0.3);
    CHECK(r.attempts() == 0);
    CHECK(r.next(true, 0.3) == Action::Rebuild);
}

TEST_CASE("a device lost again and again is given up on", "[device_recovery]") {
    // Something this client records that kills the device kills the rebuilt
    // one too, a frame later, every time. Rebuilding that for ever is a
    // client stuck behind a loading screen.
    DeviceRecovery r;
    double t = 0.0;
    for (uint32_t i = 0; i < DeviceRecovery::kMaxLossesInWindow; ++i) {
        r.deviceLost(t);
        CHECK(r.next(true, t) == Action::Rebuild);
        r.rebuilt();
        t += 5.0;
    }
    r.deviceLost(t);
    CHECK(r.next(true, t) == Action::GiveUp);
}

TEST_CASE("losses far apart are each recovered", "[device_recovery]") {
    // A phone left in a pocket for an afternoon may lose its device more than
    // once in a long session. Each of those is its own trouble.
    DeviceRecovery r;
    double t = 0.0;
    for (int i = 0; i < 10; ++i) {
        r.deviceLost(t);
        CHECK(r.next(true, t) == Action::Rebuild);
        r.rebuilt();
        t += DeviceRecovery::kLossWindowSeconds + 1.0;
    }
    CHECK(r.recoveries() == 10);
}
