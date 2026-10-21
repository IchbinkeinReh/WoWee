#pragma once

/// What fills the combat log: the builders the log packets call
/// (UnitCombatLog_C.cpp, 0x00750400-0x00753100), each deciding as the client
/// does whether the event is logged and with which suffix, and the world the
/// log asks about units and names.
///
/// The guids, names and flags are worked out when an entry is made. A name
/// the caches do not have is asked for, and the entry waits for it -
/// combat_log::Log holds every later entry behind it until it comes.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "game/combat_log.hpp"

namespace wowee::game {

class GameHandler;
struct AttackerStateUpdateData;
struct SpellGoData;

class CombatLogFeed final : public combat_log::UnitWorld {
public:
    explicit CombatLogFeed(GameHandler& owner);

    combat_log::Log& log() { return log_; }
    const combat_log::Log& log() const { return log_; }

    /// 0x007538a0, when a character enters the world.
    void enterWorld();

    /// The arguments an entry is announced and answered with.
    [[nodiscard]] std::vector<combat_log::Arg> argsOf(const combat_log::Entry& entry) const;

    /// SMSG_SEND_ALL_COMBAT_LOG's replay: the builders date what they log
    /// this far back while it is set (the delay each handler is handed).
    void setReplayDelay(uint32_t ms) { replayDelay_ = ms; }

    // The builders, by the client's function.
    void swing(const AttackerStateUpdateData& d);                                  // 0x007512b0
    void environmental(uint64_t victim, uint32_t type, int32_t damage,
                       int32_t absorbed, int32_t resisted);                         // 0x00751150
    void spellMissed(uint64_t src, uint64_t dst, uint32_t spellId, uint32_t missInfo,
                     bool damageShield, bool periodic);                             // 0x00751b80
    void spellDamage(uint64_t src, uint64_t dst, uint32_t spellId, int32_t damage, int32_t overkill,
                     uint32_t school, int32_t absorbed, int32_t resisted, int32_t blocked,
                     uint32_t hitFlags, bool periodic);                             // 0x00751c40
    void damageSplit(uint64_t src, uint64_t dst, uint32_t spellId, int32_t damage, int32_t overkill,
                     uint32_t school, int32_t absorbed, int32_t resisted, int32_t blocked,
                     uint32_t hitFlags);                                            // 0x00752de0
    void spellHeal(uint64_t src, uint64_t dst, uint32_t spellId, int32_t heal, int32_t overheal,
                   int32_t absorbed, bool critical, bool periodic);                 // 0x00751f70
    void spellEnergize(uint64_t src, uint64_t dst, uint32_t spellId, uint32_t powerType,
                       int32_t amount, bool periodic);                              // 0x00752040
    void spellDrain(uint64_t src, uint64_t dst, uint32_t spellId, uint32_t powerType,
                    uint32_t amount, float multiplier, bool periodic);              // 0x00752100
    void instakill(uint64_t src, uint64_t dst, uint32_t spellId);                  // 0x00752210
    void summon(uint64_t src, uint64_t dst, uint32_t spellId);                     // 0x007522a0
    void resurrect(uint64_t src, uint64_t dst, uint32_t spellId);                  // 0x00752340
    void interrupt(uint64_t src, uint64_t dst, uint32_t spellId, uint32_t interrupted);  // 0x007523d0
    void extraAttacks(uint64_t src, uint64_t dst, uint32_t spellId, int32_t count);      // 0x00752520
    void durabilityDamage(uint64_t src, uint64_t dst, uint32_t spellId, uint32_t itemId);  // 0x007525e0
    void durabilityDamageAll(uint64_t src, uint64_t dst, uint32_t spellId);        // 0x00752680
    void aura(uint64_t unit, uint64_t caster, uint32_t spellId, bool debuff, bool applied);  // 0x00752710
    void auraDose(uint64_t unit, uint64_t caster, uint32_t spellId, uint32_t before, uint32_t after,
                  bool debuff);                                                     // 0x00752860
    void auraRefresh(uint64_t unit, uint64_t caster, uint32_t spellId, bool debuff);  // 0x00752ba0
    void dispel(uint64_t src, uint64_t dst, uint32_t spellId, uint32_t auraSpell, bool debuff,
                bool stolen);                                                       // 0x00752930
    void auraBroken(uint64_t src, uint64_t dst, uint32_t breakerSpell, uint32_t auraSpell,
                    bool debuff);                                                   // 0x00752a40
    void damageShield(uint64_t src, uint64_t dst, uint32_t spellId, int32_t damage, int32_t overkill,
                      uint32_t school);                                             // 0x00752c90
    void unitDied(uint64_t unit);                                                   // 0x007561e0 -> 0x00752ed0
    void buildingDamage(uint64_t src, uint64_t building, uint32_t spellId, int32_t amount);  // 0x00753000
    void buildingHeal(uint64_t src, uint64_t building, uint32_t spellId, int32_t amount);    // 0x00753100
    void castStart(uint64_t caster, uint32_t spellId, uint32_t castFlags);         // 0x00751920
    /// SPELL_CAST_SUCCESS, and the cast's misses - at once, or as each
    /// target's missile lands.
    void castGo(const SpellGoData& go);                                             // 0x0080e1b0
    void castFailed(uint32_t spellId, const std::string& text);                    // 0x00751ad0
    void partyKill(uint64_t killer, uint64_t victim);                              // 0x00750c90
    void procResist(uint64_t src, uint64_t dst, uint32_t spellId);                 // 0x00750d40
    void dispelFailed(uint64_t src, uint64_t dst, uint32_t spellId,
                      const std::vector<uint32_t>& failed);                         // 0x00750ea0
    void enchantment(uint64_t target, uint64_t caster, uint32_t itemId, uint32_t enchantId);  // 0x00751050

    /// AttributesEx3 0x02000000: a heal or a power gain the log calls
    /// periodic (0x006d3c30, 0x006d3df0, 0x006d3220).
    [[nodiscard]] bool periodicByAttribute(uint32_t spellId) const;

    /// Missiles whose misses wait for them to land.
    void update();

    // The answers entries wait for.
    void playerNameAnswered(uint64_t guid, const std::string& name, const std::string& realm, bool found);
    void creatureAnswered(uint32_t entry, const std::string& name, bool found);
    void gameObjectAnswered(uint32_t entry, const std::string& name, bool found);
    void petNameAnswered(uint64_t guid, const std::string& name, bool found);
    void itemAnswered(uint32_t entry, const std::string& name, bool found);

    // combat_log::UnitWorld
    [[nodiscard]] uint64_t activePlayer() const override;
    [[nodiscard]] uint64_t target() const override;
    [[nodiscard]] uint64_t focus() const override;
    [[nodiscard]] bool inRaid() const override;
    [[nodiscard]] bool hasPartyMembers() const override;
    [[nodiscard]] bool partyMember(uint64_t guid) const override;
    [[nodiscard]] bool raidMember(uint64_t guid) const override;
    [[nodiscard]] uint32_t memberFlags(uint64_t guid, bool raid) const override;
    [[nodiscard]] uint32_t raidTarget(uint64_t guid) const override;
    [[nodiscard]] Object object(uint64_t guid) const override;
    [[nodiscard]] std::optional<int> reactionToPlayer(uint64_t unit) const override;

private:
    struct SpellRow;
    [[nodiscard]] std::optional<SpellRow> spell(uint32_t spellId) const;
    /// 0x00750400's start: the entry, its sides and its spell.
    combat_log::Entry& create(combat_log::Event event, uint64_t src, uint64_t dst, const SpellRow* rec);
    void setSide(combat_log::Party& side, uint64_t guid);
    /// 0x0074fd40 / 0x0074f720: the name, or what to wait for.
    void nameOf(combat_log::Party& side);
    void itemNameOf(combat_log::Entry& entry, uint32_t itemId);
    void commit(uint32_t delayMs = 0);
    [[nodiscard]] std::string unknownName() const;

    GameHandler& owner_;
    combat_log::Log log_;
    uint32_t replayDelay_ = 0;

    struct PendingMiss {
        int64_t landsAt = 0;
        uint64_t src = 0, dst = 0;
        uint32_t spellId = 0, missInfo = 0;
    };
    std::vector<PendingMiss> pendingMisses_;
};

}  // namespace wowee::game
