#pragma once

// The achievements the watch frame follows, as the client keeps them: an
// ordered list of at most ten ids (0x005b23e0 appends, 0x005b24d0 removes),
// saved to the per-character CVar trackedAchievements as the player logs out
// (0x005b1fa0) and read back when the character's settings are loaded
// (0x005b3840).
//
// The CVar holds the client's packed number list (0x00766720 writes,
// 0x00766f70 reads): the two bytes 'v' and 0x01, then each id as three
// characters, base 93 from '#'. An id of 0xc4605 or more is not written, and
// a value without that header reads as empty.

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::game::achievement_tracking {

/// The most the list holds (0x005b23e0 stops looking after ten).
constexpr size_t kMaxTracked = 10;

enum class TrackResult { Added, AlreadyTracked, Full };

/// 0x005b23e0: an id already there is left where it is, a full list is left
/// alone, anything else goes on the end. The client fires
/// TRACKED_ACHIEVEMENT_UPDATE for the first two and not for a full list.
inline TrackResult track(std::vector<uint32_t>& list, uint32_t id) {
    for (size_t i = 0; i < list.size() && i < kMaxTracked; ++i) {
        if (list[i] == id) return TrackResult::AlreadyTracked;
    }
    if (list.size() >= kMaxTracked) return TrackResult::Full;
    list.push_back(id);
    return TrackResult::Added;
}

/// 0x005b24d0: every entry with that id goes; true when one did, which is
/// when the client fires TRACKED_ACHIEVEMENT_UPDATE.
inline bool untrack(std::vector<uint32_t>& list, uint32_t id) {
    bool removed = false;
    for (auto it = list.begin(); it != list.end();) {
        if (*it == id) { it = list.erase(it); removed = true; }
        else ++it;
    }
    return removed;
}

/// The CVar's value for the list (0x005b1fa0 through 0x00766720).
inline std::string encode(const std::vector<uint32_t>& list) {
    std::string out = "v\x01";
    for (uint32_t id : list) {
        if (id >= 0xc4605u || out.size() >= 0xfdu) continue;
        out.push_back(static_cast<char>(id / 0x21c9u + '#'));
        const uint32_t rest = id % 0x21c9u;
        out.push_back(static_cast<char>(rest / 0x5du + '#'));
        out.push_back(static_cast<char>(rest % 0x5du + '#'));
    }
    return out;
}

/// The ids in the CVar's value, in order (0x00766f70); none without the
/// header, and reading stops at the first id cut short or reading 0.
inline std::vector<uint32_t> decode(const std::string& value) {
    std::vector<uint32_t> out;
    if (value.size() < 2 || value[0] != 'v' || value[1] != '\x01') return out;
    for (size_t at = 2; at + 3 <= value.size(); at += 3) {
        const int a = static_cast<unsigned char>(value[at]);
        const int b = static_cast<unsigned char>(value[at + 1]);
        const int c = static_cast<unsigned char>(value[at + 2]);
        if (a == 0 || b == 0 || c == 0) break;
        const int id = ((a - '#') * 0x5d + (b - '#')) * 0x5d + (c - '#');
        // The restore loop (0x005b3840) stops at the first zero it reads.
        if (id == 0) break;
        out.push_back(static_cast<uint32_t>(id));
    }
    return out;
}

}  // namespace wowee::game::achievement_tracking
