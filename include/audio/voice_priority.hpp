#pragma once

// Which of the sounds playing are heard, when more play than there are
// voices to hear them on.
//
// The client's sound engine is FMOD Ex, set up (0x0087c710) with 512
// virtual channels over Sound_NumChannels real ones - "32" by default,
// held to 12..128. Past the real ones FMOD keeps the least important
// sounds running silent, virtual, and gives them a voice back as others
// end: most important is the lowest priority number, then the most
// audible. Every play leaves its priority at FMOD's default unless asked
// (0x004c5990 sets -1); the plays that involve the active mover - a melee
// swing's whoosh and impacts with it as attacker or target (0x004d0720,
// 0x004d0660, 0x004d0540, 0x004d0850), its own sound slots (0x00747310)
// - ask for 110 (+0x20 = 0x6e), and are heard over a crowd's.

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

namespace wowee::audio::voice_priority {

/// FMOD's default channel priority, what a play left at -1 has.
inline constexpr int kDefault = 128;
/// A play involving the active mover (0x6e).
inline constexpr int kActiveMover = 110;

/// Sound_NumChannels: its default (0x009e21dc, "32") and its range
/// (0x0087c710).
inline constexpr int kDefaultRealVoices = 32;
constexpr int clampRealVoices(int n) { return std::clamp(n, 12, 128); }

/// A sound playing: its priority and how loud it reaches the listener.
struct Voice {
    int priority = kDefault;
    float audibility = 1.0f;
};

/// Which of `voices` get one of `real` voices: by priority, then
/// audibility; among equals the one that started first.
inline std::vector<bool> heard(const std::vector<Voice>& voices, uint32_t real) {
    std::vector<size_t> order(voices.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (voices[a].priority != voices[b].priority) return voices[a].priority < voices[b].priority;
        return voices[a].audibility > voices[b].audibility;
    });
    std::vector<bool> out(voices.size(), false);
    for (size_t i = 0; i < order.size() && i < real; ++i) out[order[i]] = true;
    return out;
}

/// How loud a 3D sound reaches the listener, the engine's inverse model:
/// full within `minDistance`, falling as minDistance / distance past it,
/// nothing past `maxDistance`.
constexpr float distanceGain(float distance, float minDistance, float maxDistance) {
    if (maxDistance > 0.0f && distance > maxDistance) return 0.0f;
    if (distance <= minDistance || minDistance <= 0.0f) return 1.0f;
    return minDistance / distance;
}

}  // namespace wowee::audio::voice_priority
