#pragma once

#include <cstdint>

namespace wowee {
namespace rendering {

/// Where the presentation surface stands against the app's lifecycle, and
/// what has to happen to it next.
///
/// Android takes the native window away from an activity that leaves the
/// foreground and hands it a new one when it comes back. The VkSurfaceKHR
/// built on the old window - and the swapchain built on that - die with it,
/// and nothing drawn to them reaches the screen again: a client that kept
/// them came back to black and stayed there.
///
/// Three things can say the surface is gone: the app going to the background
/// (SDL_EVENT_WILL_ENTER_BACKGROUND / DID_ENTER_BACKGROUND), and the driver
/// answering an acquire or a present with VK_ERROR_SURFACE_LOST_KHR, which is
/// what a window taken away under a running frame looks like. Only one thing
/// brings it back: the app in the foreground with a native window to build on.
///
/// No Vulkan in here, so the decisions can be tested on their own; VkContext
/// asks next() and carries out the answer.
class SurfaceLifecycle {
public:
    enum class Action : uint8_t {
        None,     ///< nothing to do
        Release,  ///< destroy the swapchain and the surface
        Restore,  ///< build the surface again; the swapchain follows
    };

    /// The app has left the foreground. Idempotent: Android sends WILL and
    /// DID together, and either may be the first one seen.
    /// Answers whether this was a change.
    bool enterBackground() {
        if (background_) return false;
        background_ = true;
        return true;
    }
    /// The app is back. Answers whether this was a change.
    bool enterForeground() {
        if (!background_) return false;
        background_ = false;
        failedRestores_ = 0;
        return true;
    }

    /// The driver said VK_ERROR_SURFACE_LOST_KHR. The surface is released and
    /// rebuilt whether or not the app has been told anything.
    void surfaceLostByDriver() { driverLost_ = true; }

    void surfaceReleased() {
        live_ = false;
        driverLost_ = false;
    }
    void surfaceRestored() {
        live_ = true;
        driverLost_ = false;
        failedRestores_ = 0;
    }
    /// A restore was tried and did not take - no native window yet, or the
    /// driver refused it. It is tried again on the next frame.
    void restoreFailed() { ++failedRestores_; }

    /// What to do now, given whether there is a native window to build on.
    [[nodiscard]] Action next(bool nativeWindowPresent) const {
        if (live_ && (background_ || driverLost_)) return Action::Release;
        if (!live_ && !background_ && nativeWindowPresent) return Action::Restore;
        return Action::None;
    }

    /// Whether a frame can be drawn: a surface that is still good, in the
    /// foreground. Nothing renders in the background.
    [[nodiscard]] bool canRender() const { return live_ && !background_ && !driverLost_; }
    [[nodiscard]] bool inBackground() const { return background_; }
    [[nodiscard]] bool surfaceLive() const { return live_; }
    [[nodiscard]] uint32_t failedRestores() const { return failedRestores_; }

    /// Whether a failed restore is worth a line in the log: the first, and
    /// then one in a hundred, so a phone left without a window does not fill
    /// the log at frame rate.
    [[nodiscard]] bool shouldLogFailedRestore() const {
        return failedRestores_ == 1 || (failedRestores_ % 100) == 0;
    }

private:
    bool live_ = true;        // a surface is made with the context
    bool background_ = false;
    bool driverLost_ = false;
    uint32_t failedRestores_ = 0;
};

} // namespace rendering
} // namespace wowee
