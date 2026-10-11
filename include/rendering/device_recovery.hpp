#pragma once

#include <cstdint>

namespace wowee {
namespace rendering {

/// What a VkResult asks of the client, from the one place that decides it.
///
/// Taken as the plain integer so this header, and the tests that read it, need
/// no Vulkan. vk_context.cpp checks each value against the real enumerant at
/// compile time, so the two cannot drift apart.
enum class GpuResultClass : uint8_t {
    Ok,           ///< VK_SUCCESS, or an answer that is not a failure
    NotYet,       ///< VK_NOT_READY / VK_TIMEOUT: ask again later
    Swapchain,    ///< VK_SUBOPTIMAL_KHR / VK_ERROR_OUT_OF_DATE_KHR: rebuild the swapchain
    SurfaceLost,  ///< VK_ERROR_SURFACE_LOST_KHR: rebuild the surface (SurfaceLifecycle)
    DeviceLost,   ///< VK_ERROR_DEVICE_LOST: rebuild the device and everything made from it
    Failed,       ///< any other error: the call failed, the device is still there
};

namespace vk_result_code {
inline constexpr int32_t kSuccess = 0;
inline constexpr int32_t kNotReady = 1;
inline constexpr int32_t kTimeout = 2;
inline constexpr int32_t kSuboptimal = 1000001003;
inline constexpr int32_t kDeviceLost = -4;
inline constexpr int32_t kSurfaceLost = -1000000000;
inline constexpr int32_t kOutOfDate = -1000001004;
}  // namespace vk_result_code

[[nodiscard]] constexpr GpuResultClass classifyGpuResult(int32_t result) {
    using namespace vk_result_code;
    switch (result) {
        case kNotReady:
        case kTimeout:      return GpuResultClass::NotYet;
        case kSuboptimal:
        case kOutOfDate:    return GpuResultClass::Swapchain;
        case kSurfaceLost:  return GpuResultClass::SurfaceLost;
        case kDeviceLost:   return GpuResultClass::DeviceLost;
        default:            return result < 0 ? GpuResultClass::Failed : GpuResultClass::Ok;
    }
}

/// Whether a lost device is rebuilt, waited on, or given up on.
///
/// VK_ERROR_DEVICE_LOST is the driver saying the device and every object made
/// from it are gone: on Android after a long spell in the background or a GPU
/// reset, anywhere after a driver crash. Nothing made on the old device can be
/// used again, but a new one can be made on the same window, and the client
/// rebuilds everything it drew with (Application::recoverFromDeviceLoss) while
/// the session carries on.
///
/// Not without limit. A device lost because of something this client records
/// is lost again a frame after every rebuild, and rebuilding that forever is a
/// client that hangs behind a loading screen; past kMaxLossesInWindow losses
/// within kLossWindowSeconds it gives up, and ends as it always did. A rebuild
/// that fails - the driver not ready again yet - is retried with a growing
/// delay, kMaxRebuildAttempts times. The delay is a time to try again at, read
/// against the main loop's clock, not a sleep: the loop goes on reading the
/// connection meanwhile, so the server does not see the session go quiet.
///
/// Nothing is rebuilt while there is no window to build on: the app in the
/// background, or Android not having handed over its native window yet. That
/// is waiting, not failing, and costs no attempt.
///
/// No Vulkan in here, so the decisions can be tested on their own; the
/// application asks next() and carries out the answer.
class DeviceRecovery {
public:
    enum class Action : uint8_t {
        None,     ///< the device is fine
        Wait,     ///< lost, and nothing to build on yet, or not yet time to try again
        Rebuild,  ///< lost: tear down and rebuild now
        GiveUp,   ///< lost too often, or would not come back
    };

    static constexpr uint32_t kMaxLossesInWindow = 3;
    static constexpr double kLossWindowSeconds = 300.0;
    static constexpr uint32_t kMaxRebuildAttempts = 5;
    static constexpr double kFirstRetryDelaySeconds = 0.25;
    static constexpr double kMaxRetryDelaySeconds = 4.0;

    /// The device was lost. Every report of the same loss after the first
    /// is the same loss, so only the first counts.
    void deviceLost(double nowSeconds) {
        if (lost_) return;
        lost_ = true;
        attempts_ = 0;
        retryAt_ = 0.0;
        // The oldest loss leaves the window once it is far enough back.
        if (lossesInWindow_ > 0 && nowSeconds - windowStart_ > kLossWindowSeconds) {
            lossesInWindow_ = 0;
        }
        if (lossesInWindow_ == 0) windowStart_ = nowSeconds;
        ++lossesInWindow_;
    }

    /// What to do now, given whether there is a window to build a device on
    /// and the time on the clock deviceLost and rebuildFailed were given.
    [[nodiscard]] Action next(bool canPresent, double nowSeconds) const {
        if (!lost_) return Action::None;
        if (lossesInWindow_ > kMaxLossesInWindow) return Action::GiveUp;
        if (attempts_ >= kMaxRebuildAttempts) return Action::GiveUp;
        if (!canPresent || nowSeconds < retryAt_) return Action::Wait;
        return Action::Rebuild;
    }

    /// A rebuild did not take. Answers how long until the next one may be
    /// tried, doubling each time up to kMaxRetryDelaySeconds; next() answers
    /// Wait until then.
    double rebuildFailed(double nowSeconds) {
        ++attempts_;
        double delay = kFirstRetryDelaySeconds;
        for (uint32_t i = 1; i < attempts_ && delay < kMaxRetryDelaySeconds; ++i) delay *= 2.0;
        if (delay > kMaxRetryDelaySeconds) delay = kMaxRetryDelaySeconds;
        retryAt_ = nowSeconds + delay;
        return delay;
    }

    /// The device is back.
    void rebuilt() {
        lost_ = false;
        attempts_ = 0;
        retryAt_ = 0.0;
        ++recoveries_;
    }

    [[nodiscard]] bool lost() const { return lost_; }
    [[nodiscard]] uint32_t attempts() const { return attempts_; }
    /// When the next rebuild may be tried, after a failed one.
    [[nodiscard]] double retryAt() const { return retryAt_; }
    [[nodiscard]] uint32_t lossesInWindow() const { return lossesInWindow_; }
    /// Rebuilds that worked, over the whole run.
    [[nodiscard]] uint32_t recoveries() const { return recoveries_; }

private:
    bool lost_ = false;
    uint32_t attempts_ = 0;
    double retryAt_ = 0.0;
    uint32_t lossesInWindow_ = 0;
    double windowStart_ = 0.0;
    uint32_t recoveries_ = 0;
};

} // namespace rendering
} // namespace wowee
