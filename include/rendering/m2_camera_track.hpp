#pragma once

// A model's camera as its animation moves it, as the client evaluates it.
//
// Each frame the model's camera tracks are sampled at the animation's time
// (0x00826xxx -> 0x0082b460 for the position and the target, 0x0082b8a0 for
// the roll), into the camera's cache; the camera's place in the world is
// then its base plus what the track gives, through the model's matrix
// (0x00826f.. -> 0x004c21b0), and its roll the roll track's value alone.
//
// A track samples the sub-array of the sequence playing, or the first where
// it has fewer (0x0082b460); a sub-array with no keys gives zero. Between
// two keys (0x008284d0: the last key at or before the time, the one after
// it, and how far between them the time is) by the track's interpolation:
//   0  the earlier key's value;
//   1  linear;
//   2  Bezier: v0 (1-t)^3 + out0 3t(1-t)^2 + in1 3t^2(1-t) + v1 t^3;
//   3  Hermite: v0 h00 + out0 h10 + v1 h01 + in1 h11;
// any other leaves the cache as it was. Past the last key, the last key.
//
// The cinematic camera (0x00606570) places the model at CinematicCamera's
// origin turned by its facing about the vertical (0x00984450, counter-
// clockwise), and the world camera looks from the camera's position to its
// target, its up taken from (0, sin roll, cos roll) (0x005ff440 ->
// 0x00607f40).

#include "pipeline/m2_loader.hpp"

#include <cmath>
#include <cstdint>
#include <optional>

#include <glm/glm.hpp>

namespace wowee::rendering::m2_camera {

/// 0x008284d0: the keys around `timeMs` and how far between them it is.
struct KeySpan {
    uint32_t lower = 0;
    uint32_t upper = 0;
    float fraction = 0.0f;
};

inline KeySpan findKeys(const std::vector<uint32_t>& times, uint32_t timeMs) {
    KeySpan span;
    const auto n = static_cast<uint32_t>(times.size());
    if (n < 2) return span;
    // The last key at or before the time; before the first key, the first
    // (the client's own would wrap the difference - no camera track starts
    // after zero).
    uint32_t i = 0;
    while (i + 1 < n && times[i + 1] <= timeMs) ++i;
    span.lower = i;
    if (i + 1 >= n || timeMs < times[i]) {
        span.upper = i;
        return span;
    }
    span.upper = i + 1;
    const float into = static_cast<float>(timeMs - times[i]);
    const float between = static_cast<float>(times[i + 1] - times[i]);
    span.fraction = between > 0.0f ? into / between : 0.0f;
    return span;
}

/// One component of a spline key's value between two keys (0x0082b460).
/// `k0`/`k1` point at the keys: value, in-tangent, out-tangent, each
/// `components` floats.
inline std::optional<float> interpolate(uint16_t type, const float* k0, const float* k1, uint32_t components,
                                        uint32_t c, float t) {
    const float v0 = k0[c];
    const float v1 = k1[c];
    const float in1 = k1[components + c];
    const float out0 = k0[2 * components + c];
    switch (type) {
    case 0: return v0;
    case 1: return (v1 - v0) * t + v0;
    case 2: {
        const float t2 = t * t;
        const float t3 = t2 * t;
        const float b0 = ((t2 * 3.0f - t3) - t * 3.0f) + 1.0f;  // (1-t)^3
        const float b1 = (t3 * 3.0f - t2 * 6.0f) + t * 3.0f;     // 3t(1-t)^2
        const float b2 = t2 * 3.0f - t3 * 3.0f;                  // 3t^2(1-t)
        return v0 * b0 + in1 * b2 + v1 * t3 + out0 * b1;
    }
    case 3: {
        const float t2 = t * t;
        const float t3 = t2 * t;
        const float h00 = (t3 * 2.0f - t2 * 3.0f) + 1.0f;
        const float h10 = (t3 - t2 * 2.0f) + t;
        const float h11 = t3 - t2;
        const float h01 = t2 * 3.0f - t3 * 2.0f;
        return v0 * h00 + v1 * h01 + in1 * h11 + out0 * h10;
    }
    default: return std::nullopt;
    }
}

/// The track at `timeMs` of `sequence` into `out` (`components` floats).
/// False where the interpolation is none the client knows, which leaves
/// `out` as it was.
inline bool sample(const pipeline::M2SplineTrack& track, uint32_t sequence, uint32_t timeMs, float* out) {
    const uint32_t comps = track.components;
    if (track.sequences.empty()) {
        for (uint32_t c = 0; c < comps; ++c) out[c] = 0.0f;
        return true;
    }
    const auto& keys = track.sequences[sequence < track.sequences.size() ? sequence : 0];
    if (keys.timestamps.empty() || keys.keys.size() < keys.timestamps.size() * comps * 3) {
        for (uint32_t c = 0; c < comps; ++c) out[c] = 0.0f;
        return true;
    }
    const KeySpan span = findKeys(keys.timestamps, timeMs);
    const float* k0 = keys.keys.data() + static_cast<size_t>(span.lower) * comps * 3;
    const float* k1 = keys.keys.data() + static_cast<size_t>(span.upper) * comps * 3;
    float values[3] = {0.0f, 0.0f, 0.0f};
    for (uint32_t c = 0; c < comps && c < 3; ++c) {
        const auto v = interpolate(track.interpolationType, k0, k1, comps, c, span.fraction);
        if (!v) return false;
        values[c] = *v;
    }
    for (uint32_t c = 0; c < comps && c < 3; ++c) out[c] = values[c];
    return true;
}

/// A camera's pose in its model's space: where it is, what it looks at, and
/// its roll.
struct Pose {
    glm::vec3 position{0.0f};
    glm::vec3 target{0.0f};
    float roll = 0.0f;
};

/// What the camera's tracks give at `timeMs` of `sequence`, kept in `cache`
/// (the animated parts only, as the client's camera keeps them), and the
/// pose they make with the bases.
inline Pose evaluate(const pipeline::M2Camera& camera, uint32_t sequence, uint32_t timeMs, Pose& cache) {
    sample(camera.positionTrack, sequence, timeMs, &cache.position.x);
    sample(camera.targetTrack, sequence, timeMs, &cache.target.x);
    sample(camera.rollTrack, sequence, timeMs, &cache.roll);
    return {.position = camera.positionBase + cache.position,
            .target = camera.targetBase + cache.target,
            .roll = cache.roll};
}

/// The model's place: at `origin`, turned `facing` radians about the
/// vertical, counter-clockwise (0x00984450).
inline glm::vec3 placeInWorld(const glm::vec3& local, const glm::vec3& origin, float facing) {
    const float c = std::cos(facing);
    const float s = std::sin(facing);
    return origin + glm::vec3(local.x * c - local.y * s, local.x * s + local.y * c, local.z);
}

/// The view a camera at `position` looking at `target` with `roll` has
/// (0x005ff440 -> 0x00607f40): its forward, and its up from the reference
/// (0, sin roll, cos roll) - straight up where the two are parallel, the
/// left (1, 0, 0).
struct View {
    glm::vec3 forward{1.0f, 0.0f, 0.0f};
    glm::vec3 up{0.0f, 0.0f, 1.0f};
};

inline View viewOf(const glm::vec3& position, const glm::vec3& target, float roll) {
    View v;
    glm::vec3 dir = target - position;
    const float len2 = glm::dot(dir, dir);
    if (len2 > 2.3841858e-07f) dir /= std::sqrt(len2);
    v.forward = dir;
    const glm::vec3 ref(0.0f, std::sin(roll), std::cos(roll));
    glm::vec3 left(1.0f, 0.0f, 0.0f);
    if (std::abs(std::abs(glm::dot(ref, dir)) - 1.0f) >= 2.3841858e-07f) {
        left = glm::normalize(glm::cross(ref, dir));
    }
    v.up = glm::cross(dir, left);
    return v;
}

}  // namespace wowee::rendering::m2_camera
