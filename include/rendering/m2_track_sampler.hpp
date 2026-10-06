#pragma once

#include "pipeline/m2_loader.hpp"

#include <glm/common.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace wowee::rendering::m2_track {

struct SampleTime {
    int sequenceIndex = -1;
    float timeMs = 0.0f;
};

inline SampleTime resolveTime(const pipeline::M2AnimationTrack& track,
                              int animationSequenceIndex, float animationTimeMs,
                              float globalTimeMs,
                              const std::vector<uint32_t>& globalSequenceDurations) {
    if (track.globalSequence >= 0 &&
        static_cast<size_t>(track.globalSequence) < globalSequenceDurations.size()) {
        const float duration =
            static_cast<float>(globalSequenceDurations[track.globalSequence]);
        float time = duration > 0.0f ? std::fmod(globalTimeMs, duration) : 0.0f;
        if (time < 0.0f) time += duration;
        return {.sequenceIndex = 0, .timeMs = time};
    }
    return {.sequenceIndex = animationSequenceIndex, .timeMs = animationTimeMs};
}

inline size_t lowerKeyIndex(const std::vector<uint32_t>& timestamps,
                            size_t keyCount, float timeMs) {
    keyCount = std::min(keyCount, timestamps.size());
    if (keyCount <= 1 || timeMs <= static_cast<float>(timestamps[0])) return 0;
    const auto end = timestamps.begin() + static_cast<std::ptrdiff_t>(keyCount);
    const auto upper = std::upper_bound(
        timestamps.begin(), end, timeMs,
        [](float time, uint32_t timestamp) {
            return time < static_cast<float>(timestamp);
        });
    if (upper == end) return keyCount - 1;
    return static_cast<size_t>(upper - timestamps.begin() - 1);
}

inline float interpolationFraction(const pipeline::M2AnimationTrack& track,
                                   const std::vector<uint32_t>& timestamps,
                                   size_t lower, size_t keyCount, float timeMs) {
    // Type 0 is discrete. The loader currently stores values but not the tangent
    // pairs required by Hermite/Bezier tracks, so types 1-3 use the stable linear
    // fallback that the renderers historically used.
    if (track.interpolationType == 0 || lower + 1 >= keyCount) return 0.0f;
    const float t0 = static_cast<float>(timestamps[lower]);
    const float t1 = static_cast<float>(timestamps[lower + 1]);
    return t1 > t0 ? glm::clamp((timeMs - t0) / (t1 - t0), 0.0f, 1.0f) : 0.0f;
}

inline float sampleFloat(const pipeline::M2AnimationTrack& track,
                         int animationSequenceIndex, float animationTimeMs,
                         float globalTimeMs,
                         const std::vector<uint32_t>& globalSequenceDurations,
                         float defaultValue) {
    const auto sampleTime = resolveTime(track, animationSequenceIndex,
                                        animationTimeMs, globalTimeMs,
                                        globalSequenceDurations);
    if (sampleTime.sequenceIndex < 0 ||
        static_cast<size_t>(sampleTime.sequenceIndex) >= track.sequences.size()) {
        return defaultValue;
    }
    const auto& keys = track.sequences[static_cast<size_t>(sampleTime.sequenceIndex)];
    const size_t count = std::min(keys.timestamps.size(), keys.floatValues.size());
    if (count == 0) return defaultValue;
    const size_t lower = lowerKeyIndex(keys.timestamps, count, sampleTime.timeMs);
    const float fraction = interpolationFraction(track, keys.timestamps, lower,
                                                 count, sampleTime.timeMs);
    return lower + 1 < count
        ? glm::mix(keys.floatValues[lower], keys.floatValues[lower + 1], fraction)
        : keys.floatValues[lower];
}

inline glm::vec3 sampleVec3(const pipeline::M2AnimationTrack& track,
                            int animationSequenceIndex, float animationTimeMs,
                            float globalTimeMs,
                            const std::vector<uint32_t>& globalSequenceDurations,
                            const glm::vec3& defaultValue) {
    const auto sampleTime = resolveTime(track, animationSequenceIndex,
                                        animationTimeMs, globalTimeMs,
                                        globalSequenceDurations);
    if (sampleTime.sequenceIndex < 0 ||
        static_cast<size_t>(sampleTime.sequenceIndex) >= track.sequences.size()) {
        return defaultValue;
    }
    const auto& keys = track.sequences[static_cast<size_t>(sampleTime.sequenceIndex)];
    const size_t count = std::min(keys.timestamps.size(), keys.vec3Values.size());
    if (count == 0) return defaultValue;
    const auto safe = [&](const glm::vec3& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z)
            ? value : defaultValue;
    };
    const size_t lower = lowerKeyIndex(keys.timestamps, count, sampleTime.timeMs);
    const float fraction = interpolationFraction(track, keys.timestamps, lower,
                                                 count, sampleTime.timeMs);
    return lower + 1 < count
        ? safe(glm::mix(safe(keys.vec3Values[lower]), safe(keys.vec3Values[lower + 1]), fraction))
        : safe(keys.vec3Values[lower]);
}

inline glm::quat sampleQuat(const pipeline::M2AnimationTrack& track,
                            int animationSequenceIndex, float animationTimeMs,
                            float globalTimeMs,
                            const std::vector<uint32_t>& globalSequenceDurations) {
    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
    const auto sampleTime = resolveTime(track, animationSequenceIndex,
                                        animationTimeMs, globalTimeMs,
                                        globalSequenceDurations);
    if (sampleTime.sequenceIndex < 0 ||
        static_cast<size_t>(sampleTime.sequenceIndex) >= track.sequences.size()) {
        return identity;
    }
    const auto& keys = track.sequences[static_cast<size_t>(sampleTime.sequenceIndex)];
    const size_t count = std::min(keys.timestamps.size(), keys.quatValues.size());
    if (count == 0) return identity;
    const auto safe = [&](const glm::quat& value) {
        const float lengthSquared = glm::dot(value, value);
        return std::isfinite(lengthSquared) && lengthSquared >= 0.000001f
            ? glm::normalize(value) : identity;
    };
    const size_t lower = lowerKeyIndex(keys.timestamps, count, sampleTime.timeMs);
    const float fraction = interpolationFraction(track, keys.timestamps, lower,
                                                 count, sampleTime.timeMs);
    return lower + 1 < count
        ? glm::normalize(glm::slerp(safe(keys.quatValues[lower]),
                                   safe(keys.quatValues[lower + 1]), fraction))
        : safe(keys.quatValues[lower]);
}


/// A texture transform's matrix as the 3.3.5a client builds it (FUN_0082da40):
/// from identity, the rotation about the texture's centre, then the scale
/// about it, then the translation - each only where its track has keys.
/// Applied to a UV, the translation acts first.
inline glm::mat4 textureTransformMatrix(const pipeline::M2TextureTransform& tt, int seq,
                                        float animTime, float globalTime,
                                        const std::vector<uint32_t>& globalSeqDurations) {
    const glm::vec3 centre(0.5f, 0.5f, 0.0f);
    glm::mat4 m(1.0f);
    if (!tt.rotation.sequences.empty()) {
        const glm::quat q = sampleQuat(tt.rotation, seq, animTime, globalTime, globalSeqDurations);
        m = m * glm::translate(glm::mat4(1.0f), centre) * glm::mat4_cast(q) *
            glm::translate(glm::mat4(1.0f), -centre);
    }
    if (!tt.scale.sequences.empty()) {
        const glm::vec3 sc = sampleVec3(tt.scale, seq, animTime, globalTime, globalSeqDurations,
                                        glm::vec3(1.0f));
        m = m * glm::translate(glm::mat4(1.0f), centre) * glm::scale(glm::mat4(1.0f), sc) *
            glm::translate(glm::mat4(1.0f), -centre);
    }
    if (!tt.translation.sequences.empty()) {
        const glm::vec3 tr = sampleVec3(tt.translation, seq, animTime, globalTime,
                                        globalSeqDurations, glm::vec3(0.0f));
        m = m * glm::translate(glm::mat4(1.0f), tr);
    }
    return m;
}

// ---- Blending between sequences ----
//
// The 3.3.5a client does not snap from one sequence to the next. Each bone's
// animation state (CM2Model +0x94, 0xac bytes a bone) holds two time states:
// the sequence playing (+0x40) and the one it left (+0x64), with the blend's
// end time (+0x9c), its reciprocal length (+0xa0), a ceiling (+0xa4) and the
// weight of the old pose (+0xa8).
//
// Starting a sequence (FUN_00826c40, reached from FUN_00832ab0 - which nearly
// every unit animation call passes blend=1 - and from FUN_00831fc0, which
// picks the next variation when one ends) copies the playing state into the
// old one and sets the end to now + the NEW sequence's blendTime (M2Sequence
// +0x1c), with a ceiling of one. It does neither when a blend is already
// running and its old pose still weighs more than a half: that blend runs on
// and the new sequence takes the playing slot. It does not blend at all when
// the same sequence is asked for again after it has stopped on its last frame.
// No sequence flag gates it - flag 0x80 is only the loader's copy of 0x01
// (FUN_00835c70), which holds a sequence at its end instead of looping - and
// model flag 0x800 is the one switch that turns it off.
//
// Each frame (FUN_0082f0f0) the old sequence's time keeps running from where
// it was, wrapping at its length (or holding at its end), and the old pose's
// weight is smoothstep(remaining / blendTime), zero once the end has passed or
// when both slots hold the same sequence at the same time. Every track sampler
// then lerps (translation, scale, floats: FUN_0082b0a0, FUN_0082af40) or
// slerps (rotation: FUN_00828680 with FUN_00982460) from the new value toward
// the old by that weight - except a global-sequence track, and a track with no
// interpolation, which return their own value first.

/// Weight of the old pose with `remainingMs` of a `blendTimeMs` blend left:
/// smoothstep, one at the switch and zero at the end (FUN_0082f0f0).
inline float sequenceBlendWeight(float remainingMs, float blendTimeMs) {
    if (remainingMs < 1.0f || blendTimeMs <= 0.0f) return 0.0f;
    const float f = std::clamp(remainingMs / blendTimeMs, 0.0f, 1.0f);
    return (3.0f - 2.0f * f) * f * f;
}

/// The sequence an instance is blending out of. fromSequence -1: none.
struct SequenceBlend {
    int fromSequence = -1;
    float fromTimeMs = 0.0f;    // its time at the switch
    bool fromLoops = true;      // wraps at its length, or holds at its end
    float startClockMs = 0.0f;  // the instance clock at the switch
    float blendTimeMs = 0.0f;   // the new sequence's blendTime
};

/// What a frame samples the old pose at, and how much of it there is.
struct BlendSample {
    int sequenceIndex = -1;
    float timeMs = 0.0f;
    float weight = 0.0f;
};

/// The old pose for this frame, from a blend begun at blend.startClockMs.
inline BlendSample currentBlend(const SequenceBlend& blend, float clockMs,
                                const std::vector<pipeline::M2Sequence>& sequences,
                                int currentSequence, float currentTimeMs) {
    if (blend.fromSequence < 0 ||
        static_cast<size_t>(blend.fromSequence) >= sequences.size()) {
        return {};
    }
    const float elapsed = clockMs - blend.startClockMs;
    const float weight = sequenceBlendWeight(blend.blendTimeMs - elapsed, blend.blendTimeMs);
    if (weight <= 0.0f) return {};
    float time = blend.fromTimeMs + std::max(0.0f, elapsed);
    const float duration =
        static_cast<float>(sequences[static_cast<size_t>(blend.fromSequence)].duration);
    if (duration > 0.0f) {
        time = blend.fromLoops ? std::fmod(time, duration) : std::min(time, duration);
    }
    if (blend.fromSequence == currentSequence && time == currentTimeMs) return {};
    return {.sequenceIndex = blend.fromSequence, .timeMs = time, .weight = weight};
}

/// Record a switch away from `currentSequence` at `currentTimeMs`, to a
/// sequence whose blendTime is `newBlendTimeMs` (FUN_00826c40). Call before
/// the instance's own sequence and time change.
inline void beginSequenceBlend(SequenceBlend& blend, float clockMs,
                               int currentSequence, float currentTimeMs,
                               bool currentLoops, bool currentFinished,
                               int newSequence, uint32_t newBlendTimeMs,
                               const std::vector<pipeline::M2Sequence>& sequences) {
    if (currentSequence < 0) return;
    if (currentFinished && newSequence == currentSequence) return;
    const BlendSample running = currentBlend(blend, clockMs, sequences, -1, 0.0f);
    if (running.weight > 0.5f) return;
    blend = {.fromSequence = currentSequence, .fromTimeMs = currentTimeMs,
             .fromLoops = currentLoops, .startClockMs = clockMs,
             .blendTimeMs = static_cast<float>(newBlendTimeMs)};
}

/// Whether a track takes the old pose: not a global sequence, not discrete.
inline bool trackBlends(const pipeline::M2AnimationTrack& track, const BlendSample& blend) {
    return blend.weight > 0.0f && track.globalSequence < 0 && track.interpolationType != 0;
}

/// A bone's local translation, rotation and scale.
struct BoneTRS {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

/// A bone's tracks at a sequence and time, blended toward the old pose the way
/// the client's samplers do: new value first, then mix/slerp to the old one.
inline BoneTRS sampleBone(const pipeline::M2Bone& bone, int sequenceIndex, float timeMs,
                          float globalTimeMs, const std::vector<uint32_t>& globalSequenceDurations,
                          const BlendSample& blend = {}) {
    BoneTRS out;
    out.translation = sampleVec3(bone.translation, sequenceIndex, timeMs, globalTimeMs,
                                 globalSequenceDurations, glm::vec3(0.0f));
    out.rotation = sampleQuat(bone.rotation, sequenceIndex, timeMs, globalTimeMs,
                              globalSequenceDurations);
    out.scale = sampleVec3(bone.scale, sequenceIndex, timeMs, globalTimeMs,
                           globalSequenceDurations, glm::vec3(1.0f));
    if (trackBlends(bone.translation, blend)) {
        out.translation = glm::mix(out.translation,
                                   sampleVec3(bone.translation, blend.sequenceIndex, blend.timeMs,
                                              globalTimeMs, globalSequenceDurations,
                                              glm::vec3(0.0f)),
                                   blend.weight);
    }
    if (trackBlends(bone.rotation, blend)) {
        // glm::slerp takes the short way round, as FUN_00982460 does.
        out.rotation = glm::normalize(glm::slerp(
            out.rotation,
            sampleQuat(bone.rotation, blend.sequenceIndex, blend.timeMs, globalTimeMs,
                       globalSequenceDurations),
            blend.weight));
    }
    if (trackBlends(bone.scale, blend)) {
        out.scale = glm::mix(out.scale,
                             sampleVec3(bone.scale, blend.sequenceIndex, blend.timeMs,
                                        globalTimeMs, globalSequenceDurations, glm::vec3(1.0f)),
                             blend.weight);
    }
    return out;
}

/// The transform a batch names through its model's lookup table, or null.
inline const pipeline::M2TextureTransform* batchTextureTransform(const pipeline::M2Model& model,
                                                                 uint16_t textureAnimIndex) {
    if (textureAnimIndex == 0xFFFF || textureAnimIndex >= model.textureTransformLookup.size())
        return nullptr;
    const uint16_t idx = model.textureTransformLookup[textureAnimIndex];
    return idx < model.textureTransforms.size() ? &model.textureTransforms[idx] : nullptr;
}

// Which variation of a sequence plays, as the client picks it
// (FUN_00826e60): a roll in 0..0x7FFF walks the chain from the primary
// through nextAnimation, taking away each sequence's frequency; the one the
// roll runs out on plays, and the primary if the chain ends first. The
// caller supplies the roll so the pick can be tested.
inline int pickSequenceVariation(const std::vector<pipeline::M2Sequence>& seqs, int primary,
                                 uint32_t roll) {
    if (primary < 0 || static_cast<size_t>(primary) >= seqs.size()) return primary;
    int idx = primary;
    for (size_t guard = 0; guard < seqs.size(); ++guard) {
        const uint32_t freq = static_cast<uint16_t>(seqs[idx].frequency);
        if (roll < freq) return idx;
        roll -= freq;
        const int next = seqs[idx].nextAnimation;
        if (next < 0 || static_cast<size_t>(next) >= seqs.size()) break;
        idx = next;
    }
    return primary;
}

} // namespace wowee::rendering::m2_track
