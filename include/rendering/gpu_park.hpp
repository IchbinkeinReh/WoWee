#pragma once

// GPU work held while the app is in the background (iOS, Android).
//
// iOS forbids GPU work in the background: a command buffer committed there is
// refused and the device is lost (the user's U25 log: 'Device lost - first
// seen by upload batch fence status' five seconds after
// WILL_ENTER_BACKGROUND, twice). The frame stops drawing there, but the
// loaders and workers kept uploading - "immediate submits from 12 threads".
//
// While parked, a submit from any thread but the one that parked (the main
// thread, which handles the app's lifecycle) waits here until the app is back
// in the foreground; the main thread itself is never held - it has to see the
// foreground event that lifts the park, and the main loop does no world work
// while in the background (Application, runWhileDeviceLost).

#include <condition_variable>
#include <mutex>
#include <thread>

namespace wowee::rendering {

class GpuPark {
public:
    /// From the thread that handles the app's lifecycle.
    void park() {
        const std::lock_guard<std::mutex> lock(mutex_);
        parked_ = true;
        owner_ = std::this_thread::get_id();
    }
    void unpark() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            parked_ = false;
        }
        cv_.notify_all();
    }
    [[nodiscard]] bool parked() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return parked_;
    }
    /// Before a submit: held while parked, unless on the parking thread.
    /// True when it waited.
    bool waitIfParked() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!parked_ || owner_ == std::this_thread::get_id()) return false;
        cv_.wait(lock, [this] { return !parked_; });
        return true;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool parked_ = false;
    std::thread::id owner_{};
};

/// The process's one park, for every submit (vk_context.cpp's queueSubmit).
GpuPark& gpuPark();

}  // namespace wowee::rendering
