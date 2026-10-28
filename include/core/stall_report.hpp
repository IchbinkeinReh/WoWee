#pragma once

// What the main loop was doing when it stopped, written by the stall
// watchdog without going through the logger.
//
// A main-loop stall used to say only that it happened, through the logger -
// and a client that froze for good said nothing at all: the log is flushed
// from the main loop, so its last lines, the watchdog's warning among them,
// never reached the file. This writes straight to a file of its own
// (stall_report.txt beside the executable, or WOWEE_STALL_REPORT): the stage
// the main loop last entered, and on Windows every thread's stack as return
// addresses into the executable (for addr2line -e wowee.exe), taken by
// suspending each thread in turn and unwinding it with the image's own
// unwind tables.

#include <cstdint>

namespace wowee::core::stall_report {

/// Call once on the main thread: whose stack a stall is about.
void initMainThread();

/// The main loop's current stage, a string with static storage (a literal).
void setStage(const char* stage);
[[nodiscard]] const char* stage();

/// A stage for the length of a scope, the one before it back afterwards -
/// so work between stages is reported as such, not as the last one run.
class StageScope {
public:
    explicit StageScope(const char* name) : previous_(stage()) { setStage(name); }
    ~StageScope() { setStage(previous_); }
    StageScope(const StageScope&) = delete;
    StageScope& operator=(const StageScope&) = delete;

private:
    const char* previous_;
};

/// Write a report for a stall of `stallMs`; `snapshot` numbers the reports of
/// one stall. Called from the watchdog thread; takes no lock the main thread
/// could hold, allocates nothing while a thread is suspended.
void write(int64_t stallMs, int snapshot);

}  // namespace wowee::core::stall_report
