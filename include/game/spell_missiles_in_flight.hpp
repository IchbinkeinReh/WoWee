#pragma once

/// The casts whose missile the client still has flying (Spell_C.cpp, the list
/// at 0x00af5274): a dynamic object of such a cast keeps its model hidden
/// until the missile would have arrived (DynamicObject_C, 0x00804cc0).
///
/// SMSG_SPELL_GO (0x0080e1b0) adds one for a spell with a missile Speed
/// (Spell.dbc, +0xbc) other than 0 when the casting unit is known: the cast's
/// caster unit, spell and the packet's time (the u32 after the cast flags),
/// arriving after the flight from the caster to the destination (target flag
/// 0x40) or else to the first unit hit - distance over speed, in ms. Each
/// frame drops those arrived (0x00804c10, from the world's update); leaving
/// the world drops all (0x008047a0). Times are the client's clock
/// (0x0086ae20), in ms.

#include <cmath>
#include <cstdint>
#include <list>

namespace wowee::game::spell_missiles {

/// 0x00804b20's entry: +8/+0xc the caster unit, +0x10 the spell, +0x14 the
/// SMSG_SPELL_GO time, +0x18 when it arrives.
struct InFlight {
    uint64_t caster = 0;
    uint32_t spellId = 0;
    uint32_t castTime = 0;
    int64_t arrivesAt = 0;
};

/// 0x0080e1b0: how long the missile flies, ms - round(distance / speed *
/// -1000), the arrival being now less that.
inline int64_t flightMs(float distance, float speed) {
    return -static_cast<int64_t>(std::nearbyint(distance / speed * -1000.0f));
}

class InFlightList {
public:
    /// 0x00804b20: in order of arrival, after those arriving earlier.
    void add(const InFlight& entry) {
        auto it = entries_.begin();
        while (it != entries_.end() && it->arrivesAt - entry.arrivesAt < 0) ++it;
        entries_.insert(it, entry);
    }
    /// 0x00804c10: from the front, those whose time has come.
    void expire(int64_t now) {
        while (!entries_.empty() && now - entries_.front().arrivesAt >= 0) entries_.pop_front();
    }
    /// 0x00804cc0: whether a missile of this cast still flies - matched on
    /// the dynamic object's DYNAMICOBJECT_CASTER, SPELLID and CASTTIME.
    [[nodiscard]] bool contains(uint64_t caster, uint32_t spellId, uint32_t castTime) const {
        for (const InFlight& e : entries_) {
            if (e.caster == caster && e.spellId == spellId && e.castTime == castTime) return true;
        }
        return false;
    }
    /// 0x008047a0.
    void clear() { entries_.clear(); }
    [[nodiscard]] size_t size() const { return entries_.size(); }

private:
    std::list<InFlight> entries_;
};

}  // namespace wowee::game::spell_missiles
