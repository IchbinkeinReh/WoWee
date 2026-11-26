#pragma once

// Whether the multisampled scene fits the memory there is, before the
// anti-aliasing is raised.
//
// On a phone the GPU's memory is the app's own RAM, and iOS ends an app that
// goes past its limit without a word to it (U10: the client vanished as
// Super-Ultra was chosen, with 1666 MB of the app's limit left and the system
// twice short of memory before). The scene's depth and colour are kept per
// sample, and with the ray traced lighting its G-buffer too: at 2622x1206 and
// 8x that is some 200 MB, and 600 MB more with the G-buffer - asked for in one
// frame. So a raise is held to what fits, as the ray tracing's own guard holds
// the tracing (rt_memory_guard.hpp), and none is made while the system says it
// is short.
//
// Free of the renderer and the platform, so the test states each case.

#include "rendering/rt_memory_guard.hpp"

#include <cstdint>

namespace wowee::rendering::msaa_memory {

/// Bytes kept per sample of the scene: depth (D32) and colour (4 each), and
/// the ray traced lighting's G-buffer (two 8-byte targets) where it runs.
/// The colour is counted though a tiler may keep it on chip: whether the
/// driver gives lazily allocated memory is the driver's, and an estimate on
/// the high side refuses a raise that fits rather than kills the app.
inline uint64_t perSampleBytes(bool rtGbuffer) { return 8 + (rtGbuffer ? 16 : 0); }

/// What the multisampled targets take at this size.
inline uint64_t targetBytes(uint32_t width, uint32_t height, uint32_t samples, bool rtGbuffer) {
    return uint64_t(width) * height * (samples > 1 ? samples : 1) * perSampleBytes(rtGbuffer);
}

/// What may still be taken: the device-local budget's rest, and where that
/// memory is the system's, the RAM's rest above the reserve. 0 when the
/// system says it is short. UINT64_MAX where nothing is known.
inline uint64_t takeable(const rt_memory::Snapshot& s) {
    if (s.systemLow) return 0;
    uint64_t free = UINT64_MAX;
    if (s.gpuBudget > 0) free = s.gpuBudget > s.gpuUsage ? s.gpuBudget - s.gpuUsage : 0;
    if (s.unified && (s.ramTotal > 0 || s.ramIsAppHeadroom)) {
        const uint64_t ram = rt_memory::usableRam(s);
        if (ram < free) free = ram;
    }
    return free;
}

/// The most samples, from `wanted` down by halves, that fit: a step down or
/// none is always taken; a step up only where what it adds over `current` can
/// be taken.
inline uint32_t samplesThatFit(uint32_t wanted, uint32_t current, uint32_t width, uint32_t height, bool rtGbuffer,
                               const rt_memory::Snapshot& s) {
    if (wanted < 1) wanted = 1;
    if (current < 1) current = 1;
    if (wanted <= current) return wanted;
    const uint64_t free = takeable(s);
    const uint64_t have = targetBytes(width, height, current, rtGbuffer);
    for (uint32_t samples = wanted; samples > current; samples /= 2) {
        const uint64_t need = targetBytes(width, height, samples, rtGbuffer);
        if (need - have <= free) return samples;
    }
    return current;
}

}  // namespace wowee::rendering::msaa_memory
