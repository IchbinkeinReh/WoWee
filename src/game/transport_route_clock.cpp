// src/game/transport_route_clock.cpp
// A transport's place on its path as the server last gave it, carried on
// the client's steady clock between the server's samples.
#include "game/transport_route_clock.hpp"

#include <algorithm>
#include <cmath>

namespace wowee::game {

double shortestPathDeltaMs(double toMs, double fromMs, uint32_t periodMs) {
    if (periodMs == 0) return toMs - fromMs;
    const double period = static_cast<double>(periodMs);
    double d = std::fmod(toMs - fromMs, period);
    if (d < -period * 0.5) d += period;
    if (d >= period * 0.5) d -= period;
    return d;
}

uint32_t TransportRouteClock::serverProgressAt(int64_t steadyNowMs) const {
    // The time since the anchor, never negative: a steady clock does not run
    // backwards, but a sample stamped a moment ahead of the frame reading it
    // would. Truncated to 32 bits, as the server's own counter is, so the
    // sum wraps where the server's does.
    const int64_t since = std::max<int64_t>(0, steadyNowMs - anchorSteadyMs_);
    return anchorProgressMs_ + static_cast<uint32_t>(static_cast<uint64_t>(since));
}

uint32_t TransportRouteClock::serverPathTimeAt(int64_t steadyNowMs) const {
    if (!valid_ || periodMs_ == 0) return 0u;
    return serverProgressAt(steadyNowMs) % periodMs_;
}

double TransportRouteClock::slewRemainingMs(int64_t steadyNowMs) const {
    if (slewDurationMs_ <= 0 || slewStartOffsetMs_ == 0.0) return 0.0;
    const int64_t since = steadyNowMs - slewStartSteadyMs_;
    if (since <= 0) return slewStartOffsetMs_;
    if (since >= slewDurationMs_) return 0.0;
    const double left = 1.0 - static_cast<double>(since) / static_cast<double>(slewDurationMs_);
    return slewStartOffsetMs_ * left;
}

double TransportRouteClock::pathTimeAt(int64_t steadyNowMs) const {
    if (!valid_ || periodMs_ == 0) return 0.0;
    const double period = static_cast<double>(periodMs_);
    double t = static_cast<double>(serverPathTimeAt(steadyNowMs)) + slewRemainingMs(steadyNowMs);
    t = std::fmod(t, period);
    if (t < 0.0) t += period;
    // fmod of a value a hair under the period can round up to it.
    if (t >= period) t = 0.0;
    return t;
}

TransportRouteClock::Result TransportRouteClock::apply(uint32_t progressMs, uint32_t periodMs,
                                                       int64_t steadyNowMs, bool authoritative) {
    Result r;
    if (periodMs == 0) return r;
    r.newPathMs = static_cast<double>(progressMs % periodMs);

    auto anchor = [&] {
        anchorProgressMs_ = progressMs;
        anchorSteadyMs_ = steadyNowMs;
        periodMs_ = periodMs;
        valid_ = true;
        authoritative_ = authoritative_ || authoritative;
    };
    auto dropSlew = [&] {
        slewStartOffsetMs_ = 0.0;
        slewStartSteadyMs_ = steadyNowMs;
        slewDurationMs_ = 0;
    };

    if (!valid_) {
        anchor();
        dropSlew();
        r.outcome = Outcome::Anchored;
        r.oldPathMs = r.newPathMs;
        return r;
    }

    r.oldPathMs = pathTimeAt(steadyNowMs);
    r.periodChanged = periodMs != periodMs_;
    r.errorMs = r.periodChanged ? r.newPathMs - r.oldPathMs
                                : shortestPathDeltaMs(r.newPathMs, r.oldPathMs, periodMs);

    if (!authoritative && authoritative_) {
        r.outcome = Outcome::Ignored;
        return r;
    }

    // The server's progress is the base from here on either way; what is
    // left to decide is whether the hull catches up at once or over a moment.
    anchor();
    if (r.periodChanged || std::abs(r.errorMs) > static_cast<double>(kSnapThresholdMs)) {
        dropSlew();
        r.outcome = Outcome::Snapped;
        return r;
    }
    // Drawn where it was a moment ago, the difference running out to nothing.
    slewStartOffsetMs_ = -r.errorMs;
    slewStartSteadyMs_ = steadyNowMs;
    const double slew = std::abs(r.errorMs) * static_cast<double>(kSlewTimePerErrorMs);
    slewDurationMs_ = static_cast<int32_t>(
        std::clamp(slew, static_cast<double>(kMinSlewMs), static_cast<double>(kMaxSlewMs)));
    r.slewMs = slewDurationMs_;
    r.outcome = Outcome::Slewed;
    return r;
}

bool TransportRouteClock::enforceBound(int64_t steadyNowMs, int32_t boundMs, double* outErrorMs) {
    if (!valid_ || periodMs_ == 0) {
        if (outErrorMs) *outErrorMs = 0.0;
        return false;
    }
    const double drawn = pathTimeAt(steadyNowMs);
    const double base = static_cast<double>(serverPathTimeAt(steadyNowMs));
    const double error = shortestPathDeltaMs(drawn, base, periodMs_);
    if (outErrorMs) *outErrorMs = error;
    if (std::abs(error) <= static_cast<double>(boundMs)) return false;
    slewStartOffsetMs_ = 0.0;
    slewStartSteadyMs_ = steadyNowMs;
    slewDurationMs_ = 0;
    return true;
}

const char* routeClockOutcomeName(TransportRouteClock::Outcome outcome) {
    switch (outcome) {
        case TransportRouteClock::Outcome::Anchored: return "anchored";
        case TransportRouteClock::Outcome::Slewed: return "slewed";
        case TransportRouteClock::Outcome::Snapped: return "snapped";
        case TransportRouteClock::Outcome::Ignored: return "ignored";
    }
    return "?";
}

} // namespace wowee::game
