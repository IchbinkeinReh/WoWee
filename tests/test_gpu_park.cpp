// The background park of GPU submits (rendering/gpu_park.hpp, U25).
#include "catch_amalgamated.hpp"
#include "rendering/gpu_park.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using wowee::rendering::GpuPark;

TEST_CASE("a loader's submit waits while the app is in the background", "[gpu_park]") {
    GpuPark park;
    CHECK_FALSE(park.waitIfParked());   // foreground: nobody waits
    park.park();                        // the main thread, at WILL_ENTER_BACKGROUND
    CHECK(park.parked());
    CHECK_FALSE(park.waitIfParked());   // the main thread itself is never held
    std::atomic<bool> submitted{false};
    std::atomic<bool> waited{false};
    std::thread loader([&] {
        waited = park.waitIfParked();
        submitted = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(submitted.load());      // held in the background
    park.unpark();                      // DID_ENTER_FOREGROUND
    loader.join();
    CHECK(submitted.load());
    CHECK(waited.load());
    CHECK_FALSE(park.parked());
}

TEST_CASE("the park answers for the process's queue submits", "[gpu_park]") {
    // vk_context.cpp's queueSubmit asks wowee::rendering::gpuPark(); a park
    // left on would hold every loader for good (shutdown unparks).
    GpuPark park;
    park.park();
    park.unpark();
    std::thread t([&] { CHECK_FALSE(park.waitIfParked()); });
    t.join();
}
