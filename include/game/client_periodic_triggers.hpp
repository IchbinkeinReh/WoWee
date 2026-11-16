#pragma once

// SPELL_AURA_PERIODIC_TRIGGER_SPELL_FROM_CLIENT (48): an aura on a unit the
// player controls whose trigger spell the client itself casts, by that unit,
// every period ("AUPeriodicClientTrigger", 0x0080df10 adds one, 0x0080ddd0
// runs them, 0x0080dfe0 takes them off).

#include <cstdint>
#include <vector>

namespace wowee::game {

inline constexpr uint32_t kAuraPeriodicTriggerFromClient = 48;

struct ClientTrigger {
    uint64_t unit = 0;
    uint32_t spellId = 0;       ///< the aura's spell
    uint32_t triggerSpell = 0;  ///< EffectTriggerSpell, what is cast
    uint32_t periodMs = 0;      ///< EffectAmplitude after spell mods (op 0x13)
    uint32_t nextMs = 0;
};

class ClientTriggerTimers {
public:
    /// 0x0080df10: a timer for one effect, first due a period from now.
    void add(uint64_t unit, uint32_t spellId, uint32_t triggerSpell, uint32_t periodMs, uint32_t nowMs) {
        timers_.push_back({unit, spellId, triggerSpell, periodMs, nowMs + periodMs});
    }
    /// 0x0080dfe0: the unit's timers of a spell, or all of the unit's for 0.
    void remove(uint64_t unit, uint32_t spellId) {
        std::erase_if(timers_, [&](const ClientTrigger& t) {
            return t.unit == unit && (spellId == 0 || t.spellId == spellId);
        });
    }
    /// 0x0080ddd0, each frame: every timer due casts its spell once (`cast`
    /// with the unit and the trigger spell) and is due again a period from
    /// now.
    template <class Cast>
    void fire(uint32_t nowMs, const Cast& cast) {
        // Cast may add or remove timers; run over a copy of what is due.
        std::vector<ClientTrigger> due;
        for (ClientTrigger& t : timers_) {
            if (static_cast<int32_t>(nowMs - t.nextMs) < 0) continue;
            due.push_back(t);
            t.nextMs = nowMs + t.periodMs;
        }
        for (const ClientTrigger& t : due) cast(t.unit, t.triggerSpell);
    }
    [[nodiscard]] const std::vector<ClientTrigger>& timers() const { return timers_; }
    void clear() { timers_.clear(); }

private:
    std::vector<ClientTrigger> timers_;
};

}  // namespace wowee::game
