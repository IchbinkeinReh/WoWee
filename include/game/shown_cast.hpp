#pragma once

#include <cstdint>
#include <unordered_map>

namespace wowee::game {

/// The cast each unit's cast bar shows, as the client keeps it on the unit:
/// the spell (+0xa6c) with the count it was begun with (+0xa5c), or the
/// channel (+0xa80). Packets begin and end it and nothing else - a bar whose
/// time is over is still that cast's until its SPELL_GO or failure names it.
class ShownCasts {
public:
    struct Cast {
        uint8_t count = 0;
        uint32_t spell = 0;
        uint32_t channel = 0;
    };

    /// 0x0072ab60, the count as 0x00715a80 sets it. The unit's channel is over.
    void castStarted(uint64_t unit, uint8_t count, uint32_t spell) { casts_[unit] = Cast{count, spell, 0}; }
    /// 0x0072ad20. The unit's cast is over.
    void channelStarted(uint64_t unit, uint32_t spell) { casts_[unit] = Cast{0, 0, spell}; }
    /// Whether this is the cast shown, by its count and its spell: the end
    /// that UNIT_SPELLCAST_STOP follows (0x007fecc0). A channel is never
    /// ended so.
    [[nodiscard]] bool isShown(uint64_t unit, uint8_t count, uint32_t spell) const {
        const Cast* cast = of(unit);
        return cast && cast->spell != 0 && cast->spell == spell && cast->count == count;
    }
    /// 0x00726200.
    void castEnded(uint64_t unit) {
        if (const auto it = casts_.find(unit); it != casts_.end() && it->second.spell != 0) casts_.erase(it);
    }
    /// 0x00726220.
    void channelEnded(uint64_t unit) {
        if (const auto it = casts_.find(unit); it != casts_.end() && it->second.channel != 0) casts_.erase(it);
    }
    [[nodiscard]] const Cast* of(uint64_t unit) const {
        const auto it = casts_.find(unit);
        return it != casts_.end() ? &it->second : nullptr;
    }
    /// A unit gone from the world (0x00734fd0).
    void forget(uint64_t unit) { casts_.erase(unit); }
    void clear() { casts_.clear(); }

private:
    std::unordered_map<uint64_t, Cast> casts_;
};

}  // namespace wowee::game
