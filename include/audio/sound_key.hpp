#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// What a sound file is known by in the cache of decoded ones.
namespace wowee::audio::sound_key {

/// FNV-1a over the first and the last 256 bytes, sixty-four stretches of
/// sixteen between them, and the size - not over it all, which a play would
/// pay for each time. The ends alone are not enough: two of one length that
/// begin and end in silence were one sound.
inline uint64_t of(const std::vector<uint8_t>& file) {
    uint64_t h = 14695981039346656037ull;
    const auto mix = [&](uint8_t b) {
        h ^= b;
        h *= 1099511628211ull;
    };
    constexpr size_t kEnd = 256, kStretches = 64, kStretch = 16;
    const size_t size = file.size();
    if (size <= 2 * kEnd + kStretches * kStretch) {
        for (const uint8_t b : file) mix(b);
    } else {
        for (size_t i = 0; i < kEnd; ++i) mix(file[i]);
        const size_t within = size - 2 * kEnd;
        for (size_t s = 0; s < kStretches; ++s) {
            const size_t at = kEnd + (within - kStretch) * s / (kStretches - 1);
            for (size_t i = 0; i < kStretch; ++i) mix(file[at + i]);
        }
        for (size_t i = size - kEnd; i < size; ++i) mix(file[i]);
    }
    for (int i = 0; i < 8; ++i) mix(static_cast<uint8_t>(size >> (i * 8)));
    return h;
}

}  // namespace wowee::audio::sound_key
