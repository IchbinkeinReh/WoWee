#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wowee::game::macro_options {

/// The client's macro conditional language (UIMacroOptions.cpp), which
/// SecureCmdOptionParse answers and a macro's commands go through:
///
///     [cond, nocond:arg/arg, @unit][...] action; [..] action; action
///
/// The clauses between ';' are tried in order (0x005f0df0). A clause with no
/// bracket group always applies; otherwise its groups are tried until one
/// passes. A group is a comma list (0x005f0bb0) of conditions and at most one
/// unit, "target=unit" or "@unit" - the unit the conditions ask about, the
/// current target when none is named. A condition is a name from the table at
/// 0x00ad14d8, "no" in front to negate it, and arguments after ':' split at
/// '/' (0x005f0040); each is answered by its handler (0x00ad1560) and all must
/// hold (0x005ef5c0). The applying clause's text after its groups is the
/// result, and the passing group's unit its target.

/// What the conditions ask of the client.
class State {
public:
    virtual ~State() = default;
    /// 0x0060abf0 with the empty string meaning the target: the unit a token
    /// or a name stands for, 0 for none.
    virtual uint64_t unitGuid(const std::string& unit) = 0;

    virtual bool inCombat() = 0;                       ///< player's or pet's UNIT_FLAG_IN_COMBAT
    virtual bool exists(uint64_t unit) = 0;            ///< 0x005eed70
    virtual bool canAssist(uint64_t unit) = 0;         ///< 0x007293d0
    virtual bool canAttack(uint64_t unit) = 0;         ///< 0x00729740
    virtual bool inPartyAsMember(uint64_t unit) = 0;   ///< 0x0052c8c0
    virtual bool inRaidAsMember(uint64_t unit) = 0;    ///< 0x00573200
    virtual bool dead(uint64_t unit) = 0;              ///< 0x005ef8b0
    virtual bool flyable(uint64_t unit) = 0;           ///< 0x005eee70
    virtual bool outdoors() = 0;                       ///< 0x0071b7f0; false with no player
    virtual bool hasPlayer() = 0;
    virtual bool swimming() = 0;
    virtual bool flying() = 0;
    virtual bool mounted() = 0;
    virtual bool stealthed() = 0;
    virtual bool inParty() = 0;                        ///< 0x0052bd10
    virtual bool inRaid() = 0;                         ///< 0xbeb608
    virtual int shapeshiftForm() = 0;                  ///< 0x0053d4b0(1): 0 for none
    /// The player's pet (charm, else summon) when there is one: its name and
    /// its family's name.
    virtual bool pet(std::string& name, std::string& family) = 0;
    /// 0x0055f940: with an empty name any modifier, else the named one.
    virtual bool modifier(const std::string& name) = 0;
    /// The mouse button of the click being handled (0xb499a8 +0x1234) and the
    /// button the running macro was run with (0xbeaf48); empty for none.
    virtual std::string clickButton() = 0;
    virtual std::string macroButton() = 0;
    /// A mouse button's name by number (0x005eec20).
    virtual std::string mouseButtonName(int number) = 0;
    virtual int actionBarPage() = 0;                   ///< 0xc1e598 + 1
    virtual int bonusBarOffset() = 0;                  ///< 0xc1e59c
    virtual bool equipped(const std::string& type) = 0; ///< 0x005efd70's test of one argument
    /// The spell the player is channelling, empty for none.
    virtual bool channeling(std::string& spellName) = 0;
    virtual int activeSpec() = 0;                      ///< 0x005c57a0(0) + 1
    virtual bool vehicleUi(uint64_t unit) = 0;         ///< the unit's vehicle shows its own bar
    virtual bool playerVehicleUi() = 0;
    /// What the cursor holds as 0x005ef490 names it, empty for nothing.
    virtual std::string cursorType() = 0;
    /// An unknown condition name: the client reports it (0x005216f0(0x23e))
    /// and lets the condition hold.
    virtual void unknownOption(const std::string& name) = 0;
};

/// SecureCmdOptionParse's core (0x005f0df0). False when no clause applies;
/// else the clause's text and the unit its passing group names (empty when
/// none - the caller's nil).
bool parse(const std::string& options, State& state, std::string& result, std::string& target);

}  // namespace wowee::game::macro_options
