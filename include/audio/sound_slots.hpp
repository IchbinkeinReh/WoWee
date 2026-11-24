#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

// Where each sound with a handle is in a list whose last entry takes the place
// of one removed. Asked for a handle by the plays of a crowd and by every unit's
// loop each frame, a walk of the list each time cost as much as all of them
// together.
namespace wowee::audio {

class SoundSlots {
public:
    void added(uint32_t id, size_t slot) {
        if (id != 0) slots_[id] = slot;
    }
    /// The handle let go of, its sound playing on.
    void forget(uint32_t id) { slots_.erase(id); }
    /// The entry at `slot` gone, and the last of the list, `lastId` at
    /// `lastSlot`, in its place.
    void removed(uint32_t id, size_t slot, uint32_t lastId, size_t lastSlot) {
        if (id != 0) slots_.erase(id);
        if (slot != lastSlot && lastId != 0) slots_[lastId] = slot;
    }
    [[nodiscard]] std::optional<size_t> find(uint32_t id) const {
        const auto it = id != 0 ? slots_.find(id) : slots_.end();
        if (it == slots_.end()) return std::nullopt;
        return it->second;
    }
    void clear() { slots_.clear(); }

private:
    std::unordered_map<uint32_t, size_t> slots_;
};

}  // namespace wowee::audio
