#pragma once

#include <cmath>
#include <cstdint>

namespace wowee::game {

/// A cast's start and end as UnitCastingInfo / UnitChannelInfo answer them
/// (0x00611df0, 0x00612090): the unit's stored whole milliseconds (+0xa78,
/// +0xa7c), fixed for the cast - moved only when the cast is delayed or a new
/// one begins. This client keeps a remaining time that runs down by frames,
/// so the times worked out from it moved a few ms from one call to the next
/// (T405); they are kept here once taken.
struct CastTimes {
    uint32_t spellId = 0;
    bool channel = false;
    float total = 0.0f;
    double start = 0.0;  ///< ms, GetTime()'s clock
    double end = 0.0;
    bool set = false;
};

/// The times for a cast now `remaining` s from its end of `total` s, at
/// `nowMs`. A new cast - another spell or kind, or a start more than
/// `slackMs` from the one kept - takes its own. A delay (SMSG_SPELL_DELAYED
/// adds to the total and the remaining alike, so the start stays) moves the
/// end to start + total; an end otherwise moved by more than the slack (a
/// channel cut short) is taken as it now is.
inline CastTimes castTimesNow(CastTimes kept, uint32_t spellId, bool channel, float total, float remaining,
                              double nowMs, double slackMs = 50.0) {
    const double start = nowMs - (static_cast<double>(total) - static_cast<double>(remaining)) * 1000.0;
    const double end = nowMs + static_cast<double>(remaining) * 1000.0;
    const bool same = kept.set && kept.spellId == spellId && kept.channel == channel &&
                      std::fabs(start - kept.start) <= slackMs;
    if (same && kept.total != total) {
        kept.total = total;
        kept.end = kept.start + std::round(static_cast<double>(total) * 1000.0);
    } else if (!same) {
        kept.spellId = spellId;
        kept.channel = channel;
        kept.total = total;
        kept.start = std::floor(start);
        kept.end = kept.start + std::round(static_cast<double>(total) * 1000.0);
        kept.set = true;
    } else if (std::fabs(end - kept.end) > slackMs) {
        kept.end = std::floor(end);
    }
    return kept;
}

}  // namespace wowee::game
