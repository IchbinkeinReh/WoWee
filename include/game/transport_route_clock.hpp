// include/game/transport_route_clock.hpp
// A transport's place on its path as the server last gave it, carried on
// the client's steady clock between the server's samples.
#pragma once

#include <cstdint>

namespace wowee::game {

/// Where a transport's path time came from, for the resync's weight and its
/// log line.
enum class RouteClockSource : uint8_t {
    CreateBlock,     ///< a create block's UPDATEFLAG_TRANSPORT time
    MovementBlock,   ///< a movement block's UPDATEFLAG_TRANSPORT time
    DynamicField,    ///< GAMEOBJECT_DYNAMIC's share of GAMEOBJECT_LEVEL
};

/// Milliseconds from `earlier` to `later` on a 32-bit millisecond clock.
/// The server's clocks (getMSTime, a transport's path progress) are uint32
/// and wrap every 2^32 ms, about 49.7 days; the difference read this way
/// stays right across one wrap, where a plain comparison of the two goes
/// the wrong way.
[[nodiscard]] constexpr int32_t wrapMsDiff(uint32_t later, uint32_t earlier) {
    return static_cast<int32_t>(later - earlier);
}

/// A signed 64-bit time modulo a period, in [0, period). Zero for a zero
/// period.
[[nodiscard]] constexpr uint32_t positiveModMs(int64_t timeMs, uint32_t periodMs) {
    if (periodMs == 0) return 0u;
    const int64_t m = static_cast<int64_t>(periodMs);
    int64_t r = timeMs % m;
    if (r < 0) r += m;
    return static_cast<uint32_t>(r);
}

/// A signed difference between two path times, taken the short way round
/// the loop: in [-period/2, period/2).
[[nodiscard]] double shortestPathDeltaMs(double toMs, double fromMs, uint32_t periodMs);

/// The server's path progress for one transport, anchored to the client's
/// steady clock.
///
/// The client takes a transport's path time from its create block: the
/// UPDATEFLAG_TRANSPORT time less the client's own clock, kept as an offset
/// (0x00714250: GO +0x200), and adds that offset to its clock each frame
/// before taking it modulo the path's period (0x00710640). This keeps the
/// same thing: the progress the server sent, the steady-clock moment it
/// stood for, and the progress now as that plus the time since - in 32-bit
/// arithmetic, so it wraps where the server's own counter wraps.
///
/// On top of that it re-anchors whenever the server sends the progress
/// again. A small disagreement is slewed out over a short time, so the hull
/// neither jumps nor stalls; a large one (more than kSnapThresholdMs of
/// path time) is snapped to at once.
class TransportRouteClock {
public:
    /// More than this much path time apart and a resync snaps instead of
    /// slewing.
    static constexpr int32_t kSnapThresholdMs = 1000;
    /// A slewed correction is run out at a quarter of the path's own rate:
    /// the hull moves at most 25% faster or slower while it catches up.
    static constexpr int32_t kSlewTimePerErrorMs = 4;
    static constexpr int32_t kMinSlewMs = 100;
    static constexpr int32_t kMaxSlewMs = 4000;
    /// How far the drawn path time may stand from the server's time base
    /// before the periodic check snaps it back.
    static constexpr int32_t kSanityBoundMs = 1500;

    enum class Outcome : uint8_t {
        Anchored,   ///< the first sample
        Slewed,     ///< re-anchored; a small error is being run out
        Snapped,    ///< re-anchored; a large error, or a new period, taken at once
        Ignored,    ///< a weak sample against an authoritative anchor: compared only
    };

    struct Result {
        Outcome outcome = Outcome::Ignored;
        /// The path time drawn just before the sample, and the server's path
        /// time the sample says, both in [0, period).
        double oldPathMs = 0.0;
        double newPathMs = 0.0;
        /// newPathMs - oldPathMs the short way round the loop.
        double errorMs = 0.0;
        /// How long the slew runs (Slewed only).
        int32_t slewMs = 0;
        bool periodChanged = false;
    };

    /// Takes a sample: the server's path progress in ms as the wire has it
    /// (uint32, not wrapped to the period), the period, and the steady-clock
    /// moment the progress stands for. An authoritative sample is a create
    /// or movement block's UPDATEFLAG_TRANSPORT time; a weak one is a share
    /// of the period a values update carries, which some servers compute
    /// from an unwrapped progress and so get wrong (see
    /// EntityController::applyTransportRouteClock). A weak sample only
    /// anchors a clock no authoritative sample has.
    Result apply(uint32_t progressMs, uint32_t periodMs, int64_t steadyNowMs, bool authoritative);

    [[nodiscard]] bool valid() const { return valid_; }
    [[nodiscard]] bool authoritative() const { return authoritative_; }
    [[nodiscard]] uint32_t periodMs() const { return periodMs_; }
    [[nodiscard]] int64_t anchorSteadyMs() const { return anchorSteadyMs_; }

    /// The server's path progress at a steady-clock moment, wrapping at 2^32
    /// as the server's counter does.
    [[nodiscard]] uint32_t serverProgressAt(int64_t steadyNowMs) const;
    /// The server's path time at that moment, in [0, period).
    [[nodiscard]] uint32_t serverPathTimeAt(int64_t steadyNowMs) const;
    /// The path time the client draws: the server's, plus whatever is left of
    /// a correction being slewed out. In [0, period).
    [[nodiscard]] double pathTimeAt(int64_t steadyNowMs) const;
    /// What is left of the slewed correction at that moment.
    [[nodiscard]] double slewRemainingMs(int64_t steadyNowMs) const;

    /// The periodic check: if the drawn path time stands more than boundMs
    /// from the server's time base, the slew is dropped and the drawn time
    /// snaps to the server's. Returns true when it snapped; outErrorMs is
    /// the distance found either way.
    bool enforceBound(int64_t steadyNowMs, int32_t boundMs, double* outErrorMs = nullptr);

    void reset() { *this = TransportRouteClock{}; }

private:
    bool valid_ = false;
    bool authoritative_ = false;
    uint32_t periodMs_ = 0;
    uint32_t anchorProgressMs_ = 0;
    int64_t anchorSteadyMs_ = 0;
    double slewStartOffsetMs_ = 0.0;
    int64_t slewStartSteadyMs_ = 0;
    int32_t slewDurationMs_ = 0;
};

[[nodiscard]] const char* routeClockOutcomeName(TransportRouteClock::Outcome outcome);

} // namespace wowee::game
