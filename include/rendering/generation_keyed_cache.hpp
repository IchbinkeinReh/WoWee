#pragma once

/// What is made for a set of Vulkan handles - a framebuffer over the scene's
/// colour and depth views, a view of its depth image - kept for as long as
/// those handles name the objects it was made for.
///
/// A handle is not an identity. Destroy an image view and make another, and
/// the driver may give the new one the very value the old one had: on NVIDIA
/// (610.88) turning FXAA off and on again under 8x MSAA handed back both of
/// its target views at their old values, while the images behind them were
/// new allocations. A cache keyed by the handle alone then returns the
/// framebuffer made over the destroyed images, and the pass that draws
/// through it writes into memory that is no longer theirs - a GPU page fault,
/// "invalid write", and a lost device, whenever that memory has gone back to
/// the driver in the meantime.
///
/// So the cache is told the generation of the targets its keys come from,
/// and a new generation empties it: whatever was made before is handed back
/// for the caller to release once no frame uses it, and is never found
/// again, whatever values the new handles have.
///
/// No Vulkan here, so a test can read it.

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace wowee::rendering {

template <class Key, class Value>
class GenerationKeyedCache {
public:
    /// Moves the cache to `generation`. Everything made under an earlier one
    /// is returned for the caller to release; none of it is found again.
    [[nodiscard]] std::vector<Value> advance(uint64_t generation) {
        if (generation == generation_) return {};
        generation_ = generation;
        return takeAll();
    }

    [[nodiscard]] const Value* find(const Key& key) const {
        const auto it = entries_.find(key);
        return it == entries_.end() ? nullptr : &it->second;
    }

    void insert(const Key& key, Value value) { entries_[key] = std::move(value); }

    /// Everything held, which the cache forgets.
    [[nodiscard]] std::vector<Value> takeAll() {
        std::vector<Value> out;
        out.reserve(entries_.size());
        for (auto& [key, value] : entries_) out.push_back(std::move(value));
        entries_.clear();
        return out;
    }

    [[nodiscard]] size_t size() const { return entries_.size(); }
    [[nodiscard]] uint64_t generation() const { return generation_; }

private:
    uint64_t generation_ = 0;
    std::map<Key, Value> entries_;
};

}  // namespace wowee::rendering
