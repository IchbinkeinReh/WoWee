#pragma once

/// Frame timing, in integer nanoseconds, and the wait that holds a frame to
/// its cap.
///
/// Both halves exist because of high refresh rates. At 60Hz a frame is 16.7ms
/// and a millisecond of slop in either is noise; at 144Hz the whole budget is
/// 6.94ms, and the same millisecond is fourteen per cent of it. Two things
/// were producing that slop:
///
///   - delta came from high_resolution_clock, which is an alias for
///     system_clock on libstdc++. That clock is not monotonic: an NTP
///     correction steps it, and a frame either jumps or comes out negative.
///   - the cap was a plain sleep_for to the target. Sleep overshoots by
///     around a millisecond on a good platform, and on Windows by the timer
///     resolution, which defaults to 15.6ms - more than two whole frames at
///     144Hz.
///
/// Delta is kept as integer nanoseconds and converted to seconds once, at
/// the end, so nothing accumulates in a float.

#include <cstdint>

namespace wowee::core {

/// A limit of nothing, which is what a player who set none leaves behind: the
/// client turns a 0 into 0xffffffff before comparing the two limits, so an
/// unset background cap can never be the stricter of the pair.
constexpr int kNoCap = 0x7fffffff;

/// A frame held to fewer than this is held for over an eighth of a second,
/// which the client refuses to do: a limit under nine is lifted to eight, both
/// when the cvar is written (0x00769830, 0x00769860) and again where the wait
/// is worked out (0x006836d0).
constexpr int kLowestFrameCap = 8;

/// The cap a frame is held to, out of the two the client keeps - a limit for
/// when the window is the active one and a usually lower one for when it is
/// not (0x006836d0). Each limit of 0 asks for no cap at all, and the
/// background limit is only taken where the window is not active *and* its
/// limit is at least as strict; anything under kLowestFrameCap is lifted to
/// it. 0 comes back for "hold nothing", which is what FramePacer waits for.
constexpr int capInEffect(int foregroundLimit, int backgroundLimit, bool windowIsActive) {
    const int foreground = foregroundLimit > 0 ? foregroundLimit : kNoCap;
    const int background = backgroundLimit > 0 ? backgroundLimit : kNoCap;
    const int chosen = !windowIsActive && background <= foreground ? background : foreground;
    if (chosen == kNoCap) return 0;
    return chosen < kLowestFrameCap ? kLowestFrameCap : chosen;
}

class FramePacer {
public:
    /// Nanoseconds since the last call, from a monotonic clock. The first
    /// call establishes the baseline and returns 0.
    [[nodiscard]] std::int64_t tickNs();

    /// Seconds since the last tickNs, capped so that a stall - a breakpoint,
    /// a swapchain rebuild, a machine coming back from sleep - does not
    /// teleport everything that integrates over it.
    [[nodiscard]] static float toSeconds(std::int64_t deltaNs, float capSeconds = 0.1f);

    /// Hold until one frame at `fps` after the last tickNs. Sleeps for most
    /// of the wait and spins the last of it, because a sleep cannot be
    /// trusted to the precision a short frame needs. Does nothing when
    /// fps <= 0.
    ///
    /// How much to spin is learned rather than guessed: a sleep overshoots
    /// by an amount that differs by platform, by timer resolution and by
    /// load - half a millisecond on a good macOS run, over fifteen on
    /// Windows at the default tick - and a fixed margin is either late
    /// every frame or burns a core for nothing.
    void waitForCap(int fps);

    /// The clock reading the last tickNs took. The frame's start, without
    /// asking the clock a second time and getting a slightly different
    /// answer than the delta was measured against.
    [[nodiscard]] std::int64_t lastTickNs() const { return lastNs_; }

    /// The monotonic clock these all read, in nanoseconds.
    [[nodiscard]] static std::int64_t nowNs();

private:
    std::int64_t lastNs_ = 0;
    bool started_ = false;
    /// The running estimate of how late a sleep returns, which is what the
    /// spin has to cover. Starts at half a millisecond and climbs to
    /// whatever the platform is actually doing.
    std::int64_t sleepSlackNs_ = 500'000;
};

/// Ask the platform for the finest sleep granularity it will give, for as
/// long as this object lives.
///
/// Windows only in substance: timeBeginPeriod. Without it a sleep rounds up
/// to the system timer tick, which defaults to 15.6ms - so a 144Hz cap
/// cannot be held at all, and even a 60Hz one is paced in lumps. Elsewhere
/// this is an empty object, since POSIX nanosleep already does better.
class SleepPrecisionScope {
public:
    SleepPrecisionScope();
    ~SleepPrecisionScope();
    SleepPrecisionScope(const SleepPrecisionScope&) = delete;
    SleepPrecisionScope& operator=(const SleepPrecisionScope&) = delete;

private:
#ifdef _WIN32
    // Only Windows has anything to raise or put back, and an unused member
    // is an error under this build's warning settings.
    bool raised_ = false;
#endif
};

}  // namespace wowee::core
