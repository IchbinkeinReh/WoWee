#pragma once

#include <cstddef>
#include <utility>
#include <vector>

// Effects whose model is not there yet: each starts once it is (the client
// loads a model in the background and its effect begins with it, 0x008251b0).

namespace wowee::rendering {

enum class EffectModelState : unsigned char { Loading, Ready, Failed };

template <typename Effect>
class EffectWait {
public:
    /// Past this many a new one is left out (a crowd's breath on a slow disk).
    static constexpr size_t kMost = 64;

    void add(Effect effect) {
        if (waiting_.size() < kMost) waiting_.push_back(std::move(effect));
    }
    [[nodiscard]] bool empty() const { return waiting_.empty(); }

    /// The ones whose model is there, in the order they came, to `play`; those
    /// whose model cannot be loaded go.
    template <typename StateOf, typename Play>
    void step(StateOf&& stateOf, Play&& play) {
        std::vector<Effect> now;
        std::erase_if(waiting_, [&](const Effect& e) {
            const EffectModelState state = stateOf(e);
            if (state == EffectModelState::Ready) now.push_back(e);
            return state != EffectModelState::Loading;
        });
        for (const Effect& e : now) play(e);
    }

private:
    std::vector<Effect> waiting_;
};

}  // namespace wowee::rendering
