#pragma once

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>

// Keeping what is no number out of the mix: one such sample is added to every
// other, and one in a filter's line stays there for good.
namespace wowee::audio::finite_audio {

/// Whether a place or a direction is one.
inline bool place(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

/// Silences the samples that are no numbers; how many there were.
inline uint32_t silenceBad(float* samples, size_t count) {
    uint32_t bad = 0;
    for (size_t i = 0; i < count; ++i) {
        if (std::isfinite(samples[i])) continue;
        samples[i] = 0.0f;
        ++bad;
    }
    return bad;
}

}  // namespace wowee::audio::finite_audio
