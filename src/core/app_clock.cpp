#include "core/app_clock.hpp"

#include <chrono>
#include <optional>

namespace wowee::core {

namespace {
// Zero in the live client - advanceTestClock is only ever called by the
// headless harness. Added to every reading so a --tick loop can move "now"
// forward without waiting on real time.
double& testClockOffset() {
    static double off = 0.0;
    return off;
}
std::optional<std::chrono::steady_clock::time_point>& heldAt() {
    static std::optional<std::chrono::steady_clock::time_point> at;
    return at;
}
}  // namespace

std::chrono::steady_clock::time_point steadyNow() {
    using clock = std::chrono::steady_clock;
    const auto real = heldAt() ? *heldAt() : clock::now();
    return real + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(testClockOffset()));
}

double appTimeSeconds() {
    // Fixed on the first call, so every later caller measures from the same
    // point however late it starts asking.
    static const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(steadyNow() - start).count();
}

void holdTestClock(bool held) {
    if (held) heldAt() = std::chrono::steady_clock::now();
    else heldAt().reset();
}

void advanceTestClock(double seconds) {
    testClockOffset() += seconds;
}

} // namespace wowee::core
