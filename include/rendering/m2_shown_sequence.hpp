#pragma once

#include "pipeline/m2_loader.hpp"
#include "rendering/animation/animation_data.hpp"
#include "rendering/m2_track_sampler.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace wowee::rendering::m2_sequence {

/// 0x00832ab0 -> 0x00826350: the sequence a model plays for an animation - the first with
/// its id, else the first of the one along AnimationData's Fallbacks that stands in for it
/// - and how that plays: forward, backwards from its end, or held on its first or last
/// frame. Without AnimationData (the editor) only the model's own.
struct Shown {
    int index = -1;
    anim::PlayMode mode = anim::PlayMode::Forward;
};

[[nodiscard]] inline Shown shown(const anim::AnimationDataTable& data, const std::vector<pipeline::M2Sequence>& seqs,
                                 uint32_t animationId) {
    const auto has = [&seqs](uint32_t id) {
        return std::any_of(seqs.begin(), seqs.end(), [id](const pipeline::M2Sequence& s) { return s.id == id; });
    };
    const anim::ModelAnimation found =
        data.empty() ? anim::ModelAnimation{animationId, anim::PlayMode::Forward}
                     : anim::modelAnimation(data, animationId, has, seqs.empty() ? 0u : seqs[0].id);
    for (int i = 0; i < static_cast<int>(seqs.size()); ++i) {
        if (seqs[static_cast<size_t>(i)].id == found.id) return {i, found.mode};
    }
    return {};
}

/// What a model plays that nothing is asked of: Stand, any of its variations (a doodad is
/// asked it as it is made, 0x007bda70, 0x007becd0), or what stands in for it - Closed, or
/// what AnimationData leads to - never just its first sequence, which is Open on a crate.
/// `roll` is 0..0x7fff.
[[nodiscard]] inline Shown idle(const anim::AnimationDataTable& data, const std::vector<pipeline::M2Sequence>& seqs,
                                uint32_t roll) {
    Shown s = shown(data, seqs, 0);
    if (s.index < 0) return {seqs.empty() ? -1 : 0, anim::PlayMode::Forward};
    if (s.mode == anim::PlayMode::Forward) s.index = m2_track::pickSequenceVariation(seqs, s.index, roll);
    return s;
}

/// Whether a sequence a model goes on with by itself ends where it ends (flag 0x1,
/// 0x00831fc0's +0x4a), instead of going round.
[[nodiscard]] inline bool playsOnce(const pipeline::M2Sequence& sequence) { return (sequence.flags & 0x1u) != 0; }

}  // namespace wowee::rendering::m2_sequence
