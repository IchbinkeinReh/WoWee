#pragma once

// Which step of a long main-loop stage took the time, without the profiler.
//
// 'SLOW update stage renderer->update: 518 ms' (U25, the iOS log) named the
// stage and nothing inside it; the frame profiler would have, but it is off
// unless asked for, and nobody asks before the frame they did not expect.
// A StepTimer around each step keeps the slowest one of the current stage, on
// this thread: the caller resets it when the stage begins and names it when
// the stage was slow.
//
// Nested steps: the innermost one that holds nearly all of the time is kept -
// an enclosing step replaces it only when it took more than 10% longer, so
// 'lighting' does not hide 'lighting: wmo interior pass' when that was all of
// it.

#include <chrono>

namespace wowee::core {

struct SlowestStep {
    const char* name = nullptr;
    float ms = 0.0f;
};

namespace detail {
inline thread_local SlowestStep t_slowestStep{};
}

/// Offer a finished step's time. True when it became the slowest.
inline bool offerStep(const char* name, float ms) {
    SlowestStep& s = detail::t_slowestStep;
    if (s.name == nullptr || ms > s.ms * 1.1f) {
        s = SlowestStep{name, ms};
        return true;
    }
    return false;
}

/// The slowest step since the last take, cleared.
inline SlowestStep takeSlowestStep() {
    const SlowestStep s = detail::t_slowestStep;
    detail::t_slowestStep = SlowestStep{};
    return s;
}

class StepTimer {
public:
    explicit StepTimer(const char* name) : name_(name), start_(std::chrono::steady_clock::now()) {}
    ~StepTimer() {
        offerStep(name_, std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start_).count());
    }
    StepTimer(const StepTimer&) = delete;
    StepTimer& operator=(const StepTimer&) = delete;

private:
    const char* name_;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace wowee::core

#define WOWEE_STEP_CAT2(a, b) a##b
#define WOWEE_STEP_CAT(a, b) WOWEE_STEP_CAT2(a, b)
/// Times the enclosing scope as a step of the current stage.
#define WOWEE_STEP_TIMER(name) ::wowee::core::StepTimer WOWEE_STEP_CAT(wowee_step_, __LINE__)(name)
