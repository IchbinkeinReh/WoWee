#pragma once

// The cast a unit shows: the spell whose visual it plays, as the client keeps
// it on the unit - its cast count (+0xa5c) and spell (+0xa60), and two bits
// of +0xa38: 0x400 while the visual's precast kit animates the unit, 0x200
// once an auto-repeating spell's visual has begun.
//
// A cast begins (0x00805330) as the player sends it (0x0080ac90, "sent
// here", +0x138 of its cast object) and as SMSG_SPELL_START arrives for any
// caster unit without cast flag 0x1 (0x00806700). It ends (0x007fecc0 ->
// 0x007fec00 -> 0x0072e430) as SMSG_SPELL_GO without flag 0x1 (0x0080e1b0),
// SMSG_SPELL_FAILURE (0x00809c70), SMSG_SPELL_FAILED_OTHER (0x00806ad0),
// SMSG_CAST_FAILED (0x00809af0 -> 0x00808200), SMSG_PET_CAST_FAILED
// (0x00806c30) or the player's own cancel (0x00806200) names its count and
// spell. Showing (0x007fa2e0): an AttributesEx2 0x20 spell shows nothing
// more once 0x200 is up, and sets it; else 0x400 where the precast kit has
// a StartAnimID or AnimID above 0. 0x200 goes as SMSG_CANCEL_AUTO_REPEAT
// names the unit (0x0072d130) and as the player's auto-repeat stops
// (0x00807560).
//
// Read by 0x00722180 (a vehicle casting plays no wound) and 0x00756240 (a
// player's attack landing with nothing kept sounds the shown spell's
// AnimEventSoundID, 0x00763570).

#include <cstdint>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>

namespace wowee::game::cast_visual {

/// What 0x00805330 and 0x007fa2e0 read of a spell.
struct Spell {
    uint32_t attributes = 0;    ///< Spell.dbc Attributes (+0x10)
    uint32_t attributesEx2 = 0; ///< +0x18
    uint32_t attributesEx4 = 0; ///< +0x20
    /// Its SpellVisual's PrecastKit has a StartAnimID or AnimID above 0.
    bool precastKitAnimates = false;
};

struct State {
    uint8_t castCount = 0;          ///< +0xa5c
    uint32_t spellId = 0;           ///< +0xa60, 0 for none
    bool precastAnimating = false;  ///< +0xa38 0x400
    bool autoRepeatShown = false;   ///< +0xa38 0x200
};

constexpr uint32_t kAttrEx2AutoRepeat = 0x20;

/// 0x00715a80, then 0x007fa2e0.
inline void show(State& s, uint8_t castCount, uint32_t spellId, const Spell& spell) {
    s.castCount = castCount;
    s.spellId = spellId;
    if ((spell.attributesEx2 & kAttrEx2AutoRepeat) != 0) {
        if (s.autoRepeatShown) return;
        s.autoRepeatShown = true;
    }
    if (spell.precastKitAnimates) s.precastAnimating = true;
}

/// 0x0072e430 with the spell shown.
inline void hide(State& s) {
    s.precastAnimating = false;
    s.castCount = 0;
    s.spellId = 0;
}

/// A cast beginning (0x00805330).
struct Begin {
    uint8_t castCount = 0;
    uint32_t spellId = 0;
    int32_t castTimeMs = 0;            ///< 0 for the player's send
    bool sentHere = false;             ///< from the player's send (param 3)
    bool aimed = false;                ///< its cast object's +0x139
    bool casterIsActivePlayer = false;
    Spell spell;
};

/// `showingSentHere`: whether the cast object of the cast shown - found by
/// its count, spell and caster (0x00805180), once this cast's own +0x138 is
/// set - is one the player sent and the server has not yet started.
inline void begin(State& s, const Begin& b, bool showingSentHere) {
    // The player's send shown, ended by another cast the server starts.
    if (showingSentHere && !b.sentHere) hide(s);
    if (s.spellId == 0 && !b.aimed) show(s, b.castCount, b.spellId, b.spell);
    if (b.castTimeMs > 0) {
        if ((b.spell.attributesEx4 & 0x8000000u) == 0) {
            const bool othersSkip = !b.casterIsActivePlayer &&
                                    ((b.spell.attributes & 0x180u) != 0 || (b.spell.attributesEx4 & 0x1u) != 0);
            if (othersSkip || (b.spell.attributes & 0x2u) != 0) return;
        }
        if (b.spellId != s.spellId) {
            hide(s);
            show(s, b.castCount, b.spellId, b.spell);
        }
    }
}

/// 0x007fecc0: a cast ending hides what is shown where it is that cast.
inline void end(State& s, uint8_t castCount, uint32_t spellId) {
    if (castCount == s.castCount && spellId == s.spellId) hide(s);
}

/// Every unit's, and the cast objects the player sent still waiting on the
/// server's start.
class Roster {
public:
    void sent(uint64_t caster, const Begin& b) {
        sentHere_.insert({caster, b.castCount, b.spellId});
        Begin sent = b;
        sent.sentHere = true;
        sent.castTimeMs = 0;
        apply(caster, sent);
    }
    void started(uint64_t caster, const Begin& b) {
        // The cast's own object - the player's sent one where there is one -
        // is no longer one sent here (0x00805330 sets its +0x138 first).
        sentHere_.erase({caster, b.castCount, b.spellId});
        Begin started = b;
        started.sentHere = false;
        apply(caster, started);
    }
    void ended(uint64_t caster, uint8_t castCount, uint32_t spellId) {
        sentHere_.erase({caster, castCount, spellId});
        if (auto it = states_.find(caster); it != states_.end()) end(it->second, castCount, spellId);
    }
    void autoRepeatStopped(uint64_t unit) {
        if (auto it = states_.find(unit); it != states_.end()) it->second.autoRepeatShown = false;
    }
    /// A unit gone from the world (0x00734fd0).
    void forget(uint64_t unit) {
        states_.erase(unit);
        for (auto it = sentHere_.begin(); it != sentHere_.end();) {
            if (std::get<0>(*it) == unit) it = sentHere_.erase(it);
            else ++it;
        }
    }
    void clear() {
        states_.clear();
        sentHere_.clear();
    }
    [[nodiscard]] const State* state(uint64_t unit) const {
        const auto it = states_.find(unit);
        return it != states_.end() ? &it->second : nullptr;
    }

private:
    void apply(uint64_t caster, const Begin& b) {
        State& s = states_[caster];
        const bool showingSentHere = s.spellId != 0 && sentHere_.count({caster, s.castCount, s.spellId}) != 0;
        begin(s, b, showingSentHere);
    }
    std::unordered_map<uint64_t, State> states_;
    std::set<std::tuple<uint64_t, uint8_t, uint32_t>> sentHere_;
};

}  // namespace wowee::game::cast_visual
