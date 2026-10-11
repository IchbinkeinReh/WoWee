#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wowee {
namespace audio {

/// The ambience's sounds still running, by their handles.
///
/// The client keeps its ambience on a channel object of its own (0xb4ac50)
/// and stops it at once when the world unloads (0x00409550 -> 0x004c85f0 ->
/// 0x004c6450(1)). These are the loops that stand for that channel here, so
/// leaving the world can stop them rather than let them play out over the
/// login screen.
class AmbienceLoops {
public:
    /// Enough for every loop at once; a handle past this is long finished.
    static constexpr size_t kMaxTracked = 16;

    /// Remembers a started loop. 0 (nothing started) is not kept.
    void add(uint32_t handle) {
        if (handle == 0) return;
        if (handles_.size() >= kMaxTracked) handles_.erase(handles_.begin());
        handles_.push_back(handle);
    }

    /// Every remembered handle, oldest first, and forgets them.
    [[nodiscard]] std::vector<uint32_t> takeAll() {
        std::vector<uint32_t> out;
        out.swap(handles_);
        return out;
    }

    [[nodiscard]] size_t size() const { return handles_.size(); }

private:
    /// Sound handles, not samples.
    using Handles = std::vector<uint32_t>;
    Handles handles_;
};

}  // namespace audio
}  // namespace wowee
