#include "game/combat_handler.hpp"
#include "game/spell_handler.hpp"
#include "audio/ui_sound_manager.hpp"
#include "addons/lua_api_registrations.hpp"
#include "game/cast_failure_error.hpp"
#include "game/combat_text_filter.hpp"
#include "game/unit_combat.hpp"
#include "ui/framexml_takeover.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/packet_parsers.hpp"
#include "game/entity.hpp"
#include "game/click_to_move.hpp"
#include "game/nearest_enemy_filter.hpp"
#include "game/update_field_table.hpp"
#include "game/spell_unit_target.hpp"
#include "game/opcode_table.hpp"
#include "rendering/renderer.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/npc_voice_manager.hpp"
#include "core/application.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "network/world_socket.hpp"
#include "game/interface_text.hpp"
#include "pipeline/asset_manager.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iterator>
#include <random>

namespace wowee {
namespace game {

namespace {

/// The client's clock (0x00cd76ac).
int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

float distanceSqTo(const MovementInfo& from, const Entity& to) {
    const float dx = from.x - to.getLatestX();
    const float dy = from.y - to.getLatestY();
    const float dz = from.z - to.getLatestZ();
    return dx * dx + dy * dy + dz * dz;
}

/// The on-screen line for the pet's failed cast, as 0x00805610 words it: its
/// own ERR_PET_SPELL_* lines for the results it has one for, the cooldown and
/// power lines, and otherwise the SPELL_FAILED_ text through
/// ERR_SPELL_FAILED_S. An area (0x65) or focus object (0x66) is named from
/// the first id after the result, as SMSG_CAST_FAILED's are, and nothing is
/// shown when it is not known. Where the result names something not looked up
/// here (an item class, a form, a mechanic, a custom error) the English this
/// client had is kept rather than an unfilled %s.
std::string petCastFailureMessage(const GameHandler& owner, uint32_t spellId, uint8_t result,
                                  uint32_t miscArg) {
    switch (result) {
        case 0x65: case 0x66: {
            const std::string name = result == 0x65 ? owner.getAreaName(miscArg)
                                                    : owner.getSpellFocusName(miscArg);
            if (miscArg == 0 || name.empty()) return {};
            const std::string key = cast_failure::spellFailedKey(result);
            const char* enUS = getSpellCastResultString(result);
            const std::string text = chat_channels::luaFormat(
                interfaceString(key.c_str(), enUS ? enUS : "%s"), {name});
            return chat_channels::luaFormat(interfaceString("ERR_SPELL_FAILED_S", "%s"), {text});
        }
        case 0x01: return interfaceString("ERR_PET_SPELL_AFFECTING_COMBAT", "Your pet is in combat");
        case 0x17: return interfaceString("ERR_PET_SPELL_DEAD", "Your pet is dead");
        case 0x38: return interfaceStringAny({"ERR_PET_SPELL_NOPATH", "PET_SPELL_NOPATH"}, "No path available");
        case 0x39: return interfaceString("ERR_PET_SPELL_NOT_BEHIND", "Your pet must be behind its target");
        case 0x43: {
            const auto attrs = owner.getSpellAttributes(spellId);
            if (attrs && (*attrs & 0x10u) != 0)
                return interfaceString("ERR_ABILITY_COOLDOWN", "Ability is not ready yet.");
            return interfaceString("ERR_SPELL_COOLDOWN", "Spell is not ready yet.");
        }
        case 0x55: {  // by the spell's power type (table 0x00a439c8)
            static constexpr const char* kOutOf[][2] = {
                {"ERR_OUT_OF_MANA", "Not enough mana"},   {"ERR_OUT_OF_RAGE", "Not enough rage"},
                {"ERR_OUT_OF_FOCUS", "Not enough focus"}, {"ERR_OUT_OF_ENERGY", "Not enough energy"},
                {"ERR_NOT_HAPPY_ENOUGH", "Your pet is not happy enough"},
                {"ERR_OUT_OF_RUNES", "Not enough runes"}, {"ERR_OUT_OF_RUNIC_POWER", "Not enough runic power"},
            };
            const uint8_t power = owner.getSpellData(spellId).powerType;
            if (power < 7) return interfaceString(kOutOf[power][0], kOutOf[power][1]);
            break;
        }
        case 0x61: return interfaceString("ERR_PET_SPELL_OUT_OF_RANGE", "Your pet is out of range");
        case 0x67: return interfaceString("ERR_PET_SPELL_ROOTED", "Your pet can't move");
        case 0x6d: return interfaceString("ERR_PET_SPELL_TARGETS_DEAD", "Your pet's target is dead");
        case 0x1d: case 0x1e: case 0x1f: case 0x5e: case 0x93: case 0xac: {
            const char* enUS = getSpellCastResultString(result);
            return enUS ? std::string(enUS) : std::string();
        }
        default:
            break;
    }
    const std::string key = cast_failure::spellFailedKey(result);
    const char* enUS = getSpellCastResultString(result);
    return chat_channels::luaFormat(interfaceString("ERR_SPELL_FAILED_S", "%s"),
                                    {interfaceString(key.c_str(), enUS ? enUS : "Unknown reason")});
}

/// A pet spell's name as its learn lines give it, "Name (Rank)" where it has
/// a rank (0x005d4c30).
std::string petSpellNameWithRank(const GameHandler& owner, uint32_t spellId) {
    const std::string& name = owner.getSpellName(spellId);
    const std::string& rank = owner.getSpellRank(spellId);
    return rank.empty() ? name : name + " (" + rank + ")";
}

}  // namespace

CombatHandler::CombatHandler(GameHandler& owner)
    : owner_(owner) {}

/// A unit event the client queues for a unit (0x006143f0): raised once for
/// each unit token that names it, and not at all for a unit without one.
void CombatHandler::fireUnitEvent(const char* event, uint64_t guid) {
    if (!owner_.addonEventCallbackRef() || guid == 0) return;
    for (const auto& unitId : owner_.unitTokensForGuid(guid))
        owner_.addonEventCallbackRef()(event, {unitId});
}

/// The same rule the threat indicator uses, kept in step with it.
///
/// threatWarning is 0 never, 1 in instances, 2 in a party, 3 always - and
/// IsThreatWarningEnabled answers the interface from exactly that. Reading it
/// here rather than inventing a second rule is what stops the sound and the
/// picture disagreeing about when a warning is wanted.
bool CombatHandler::threatWarningWanted() const {
    int setting = 3;
    const std::string stored = addons::storedCVarValue("threatWarning", "3");
    try { setting = std::stoi(stored); } catch (const std::exception&) {}
    switch (setting) {
        case 1:  return owner_.isInInstance();
        case 2:  return !owner_.getPartyData().members.empty();
        case 3:  return true;
        default: return false;   // 0, and anything unrecognised
    }
}

void CombatHandler::registerOpcodes(DispatchTable& table) {
    // ---- Combat clearing ----
    // 0x00756800 hands both of these to 0x006e1660.
    table[Opcode::SMSG_ATTACKSWING_DEADTARGET] = [this](network::Packet& /*packet*/) { stopAutoAttack(); };
    table[Opcode::SMSG_THREAT_CLEAR] = [this](network::Packet& packet) {
        // 0x00734b00 -> 0x007345c0: the unit's own list, and only while the
        // unit is in the world and its list has anyone on it. Each victim's
        // situation is reset and UNIT_THREAT_SITUATION_UPDATE queued for it,
        // then UNIT_THREAT_LIST_UPDATE for the unit and its threat display
        // refreshed (0x00512ab0).
        if (!packet.hasRemaining(1)) return;
        const uint64_t unitGuid = packet.readPackedGuid();
        if (!owner_.getEntityManager().getEntity(unitGuid)) return;
        auto it = threatLists_.find(unitGuid);
        if (it == threatLists_.end() || it->second.empty()) return;
        const std::vector<ThreatEntry> victims = std::move(it->second);
        threatLists_.erase(it);
        playerTopThreatOn_.erase(unitGuid);
        playerThreatStatus_.erase(unitGuid);  // reset without a floater (0x0052a160)
        for (const ThreatEntry& e : victims) fireUnitEvent("UNIT_THREAT_SITUATION_UPDATE", e.victimGuid);
        fireUnitEvent("UNIT_THREAT_LIST_UPDATE", unitGuid);
    };
    table[Opcode::SMSG_THREAT_REMOVE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(1)) return;
        uint64_t unitGuid   = packet.readPackedGuid();
        if (!packet.hasRemaining(1)) return;
        uint64_t victimGuid = packet.readPackedGuid();
        // 0x00737750: nothing unless the victim is on the unit's list. Its
        // entry goes and its situation is reset (UNIT_THREAT_SITUATION_UPDATE
        // for it); the top is picked again where it was the top (0x00737620,
        // the list here is kept highest first); then UNIT_THREAT_LIST_UPDATE
        // for the unit.
        auto it = threatLists_.find(unitGuid);
        if (it == threatLists_.end()) return;
        auto& list = it->second;
        const auto victim = std::find_if(list.begin(), list.end(),
            [victimGuid](const ThreatEntry& e) { return e.victimGuid == victimGuid; });
        if (victim == list.end()) return;
        list.erase(victim);
        if (list.empty()) threatLists_.erase(it);
        if (victimGuid == owner_.getPlayerGuid()) {
            playerTopThreatOn_.erase(unitGuid);
            playerThreatStatus_.erase(unitGuid);  // reset without a floater (0x0052a160)
        } else if (const auto left = threatLists_.find(unitGuid); left != threatLists_.end()) {
            // The top picked again (0x00737620) may move the player's status.
            std::vector<uint32_t> threats;
            size_t mine = left->second.size();
            for (size_t i = 0; i < left->second.size(); ++i) {
                threats.push_back(left->second[i].threat);
                if (left->second[i].victimGuid == owner_.getPlayerGuid()) mine = i;
            }
            if (mine < threats.size())
                notePlayerThreatStatus(unitGuid, clientThreatStatus(threats.data(), threats.size(), mine));
        }
        fireUnitEvent("UNIT_THREAT_SITUATION_UPDATE", victimGuid);
        fireUnitEvent("UNIT_THREAT_LIST_UPDATE", unitGuid);
    };
    // The attack stopped as the player stops it (0x006e2210 -> 0x006e1660).
    table[Opcode::SMSG_CANCEL_COMBAT] = [this](network::Packet& /*packet*/) {
        stopAutoAttack();
        hostileAttackers_.clear();
    };

    // ---- Attack/combat delegates ----
    table[Opcode::SMSG_ATTACKSTART] = [this](network::Packet& packet) { handleAttackStart(packet); };
    table[Opcode::SMSG_ATTACKSTOP] = [this](network::Packet& packet) { handleAttackStop(packet); };
    // The same packet under another opcode: 0x00756800 compares for 0x261 and
    // jumps into its SMSG_ATTACKSTOP case, so the two read and do alike.
    table[Opcode::SMSG_COMBAT_EVENT_FAILED] = [this](network::Packet& packet) { handleAttackStop(packet); };
    // Kept, and said by updateAutoAttack (0x00756800 -> 0x006cee70).
    table[Opcode::SMSG_ATTACKSWING_NOTINRANGE] = [this](network::Packet& /*packet*/) {
        swingRefusal_.refused(SwingRefusal::Why::Distance, steadyMs());
    };
    // Reported, not corrected: the facing is the player's. Turned to the target
    // here, the character running away from something with the attack still on
    // was spun round to face it each time the server said so.
    table[Opcode::SMSG_ATTACKSWING_BADFACING] = [this](network::Packet& /*packet*/) {
        swingRefusal_.refused(SwingRefusal::Why::Facing, steadyMs());
    };
    // Opcode 0x147, "not standing", has no case in 0x00756800.
    table[Opcode::SMSG_ATTACKSWING_NOTSTANDING] = [](network::Packet& packet) { packet.skipAll(); };
    table[Opcode::SMSG_ATTACKSWING_CANT_ATTACK] = [this](network::Packet& /*packet*/) {
        // The swing stops and nothing is said: 0x00756800 hands this to
        // 0x006e1660, which ends the attack and only plays a voice line.
        // This was an English line.
        stopAutoAttack();
    };
    table[Opcode::SMSG_ATTACKERSTATEUPDATE] = [this](network::Packet& packet) { handleAttackerStateUpdate(packet); };
    table[Opcode::SMSG_AI_REACTION] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(12)) return;
        uint64_t guid = packet.readUInt64();
        uint32_t reaction = packet.readUInt32();
        // The client's handler (0x00716b10) asks one thing per reaction and
        // nothing else: a HATE reaction the unit's PreAggro sound slot, an
        // UNFRIENDLY one its Aggro voice line. Both are asked of the unit, so
        // the packet's own two values are all there is to hand on.
        if (owner_.unitReactionSoundCallbackRef())
            owner_.unitReactionSoundCallbackRef()(guid, reaction);
    };
    table[Opcode::SMSG_SPELLNONMELEEDAMAGELOG] = [this](network::Packet& packet) { handleSpellDamageLog(packet); };
    table[Opcode::SMSG_SPELLHEALLOG] = [this](network::Packet& packet) { handleSpellHealLog(packet); };

    // ---- Environmental damage ----
    table[Opcode::SMSG_ENVIRONMENTAL_DAMAGE_LOG] = [this](network::Packet& packet) {
        // uint64 victimGuid + uint8 envDmgType + uint32 damage + uint32 absorbed + uint32 resisted
        // envDmgType: 0=Exhausted(fatigue), 1=Drowning, 2=Fall, 3=Lava, 4=Slime, 5=Fire
        if (!packet.hasRemaining(21)) { packet.skipAll(); return; }
        uint64_t victimGuid  = packet.readUInt64();
        uint8_t envType      = packet.readUInt8();
        uint32_t dmg         = packet.readUInt32();
        uint32_t envAbs      = packet.readUInt32();
        uint32_t envRes      = packet.readUInt32();
        // 0x00755270 -> 0x00751150: the combat log has it for anyone.
        owner_.combatLogFeed().environmental(victimGuid, envType, static_cast<int32_t>(dmg),
                                             static_cast<int32_t>(envAbs), static_cast<int32_t>(envRes));
        // UNIT_COMBAT when it is the player's own (0x00755270): a WOUND in the
        // school the damage type names (table 0x00a2d394).
        if (victimGuid == owner_.getPlayerGuid() && (dmg != 0 || envAbs != 0 || envRes != 0)) {
            const auto school = static_cast<int32_t>(unit_combat::environmentSchoolMask(envType));
            owner_.fireUnitCombat(victimGuid, "WOUND", static_cast<int32_t>(dmg), school,
                                  unit_combat::hitInfoFromLog(envAbs, envRes, 0, false));
        }
        if (victimGuid == owner_.getPlayerGuid()) {
            // Environmental damage: pass envType via powerType field for display differentiation
            if (dmg > 0)
                addCombatText(CombatTextEntry::ENVIRONMENTAL, static_cast<int32_t>(dmg), 0, false, envType, 0, victimGuid);
            if (envAbs > 0)
                addCombatText(CombatTextEntry::ABSORB, static_cast<int32_t>(envAbs), 0, false, 0, 0, victimGuid);
            if (envRes > 0)
                addCombatText(CombatTextEntry::RESIST, static_cast<int32_t>(envRes), 0, false, 0, 0, victimGuid);
            // Drowning damage → play DROWN one-shot on player
            if (envType == 1 && dmg > 0 && owner_.emoteAnimCallbackRef())
                owner_.emoteAnimCallbackRef()(victimGuid, 131, /*isState=*/false); // anim::DROWN
        }
        packet.skipAll();
    };

    // ---- Threat updates ----
    // The two do NOT share a format, though this read them as if they did.
    // Unit::SendThreatListUpdate writes the unit's packed guid and then the
    // count; SendChangeCurrentVictimOpcode writes the unit, *then the new
    // highest-threat target's packed guid*, and then the count. Reading the
    // second guid from both meant the plain update consumed its count as a
    // guid and then read the first victim's guid as the count, so the common
    // one of the two - the one sent on every threat change - produced a list
    // built from nothing.
    for (auto op : {Opcode::SMSG_HIGHEST_THREAT_UPDATE,
                    Opcode::SMSG_THREAT_UPDATE}) {
        const bool hasVictimGuid = (op == Opcode::SMSG_HIGHEST_THREAT_UPDATE);
        table[op] = [this, hasVictimGuid](network::Packet& packet) {
            if (!packet.hasRemaining(1)) return;
            uint64_t unitGuid = packet.readPackedGuid();
            if (hasVictimGuid) {
                if (!packet.hasRemaining(1)) return;
                (void)packet.readPackedGuid();  // the new highest-threat target
            }
            if (!packet.hasRemaining(4)) return;
            uint32_t cnt = packet.readUInt32();
            if (cnt > 100) { packet.skipAll(); return; } // sanity
            std::vector<ThreatEntry> list;
            list.reserve(cnt);
            for (uint32_t i = 0; i < cnt; ++i) {
                if (!packet.hasRemaining(1)) return;
                ThreatEntry entry;
                entry.victimGuid = packet.readPackedGuid();
                if (!packet.hasRemaining(4)) return;
                entry.threat = packet.readUInt32();
                list.push_back(entry);
            }
            // Sort descending by threat so highest is first
            std::sort(list.begin(), list.end(),
                [](const ThreatEntry& a, const ThreatEntry& b){ return a.threat > b.threat; });
            // Play Aggro Sounds, which the panel offers and nothing read -
            // here or in the interface. The threat indicator FrameXML draws is
            // silent by design; in the real client the warning noise is the
            // client's, which is why nothing in this tree ever made it.
            //
            // The sound marks the moment the player becomes the one being
            // fought, not every update while they are: the list arrives on
            // every threat change and a warning per packet would be a siren.
            const bool topNow = !list.empty() &&
                                list.front().victimGuid == owner_.getPlayerGuid();
            const bool topBefore = playerTopThreatOn_.count(unitGuid) != 0;
            if (topNow != topBefore) {
                if (topNow) playerTopThreatOn_.insert(unitGuid);
                else        playerTopThreatOn_.erase(unitGuid);
                // Registered on (0x00510000 "1"), so losing the top of the
                // threat list makes a noise until the player turns that off.
                if (topNow && threatWarningWanted() &&
                    addons::storedCVarValue("threatPlaySounds", "1") != "0") {
                    // SoundEntries 15262 (0x0071c3b0 plays 0x3b9e).
                    if (auto* ac = owner_.services().audioCoordinator) {
                        if (auto* sfx = ac->getUiSoundManager()) sfx->playById(15262);
                    }
                }
            }

            // The player new to this unit's list (0x007416f0 -> 0x0052b370):
            // with threat warnings on and above level 13, tutorial 0x35.
            {
                const uint64_t me = owner_.getPlayerGuid();
                const auto onList = [me](const std::vector<ThreatEntry>& l) {
                    return std::any_of(l.begin(), l.end(), [me](const ThreatEntry& e) { return e.victimGuid == me; });
                };
                const auto old = threatLists_.find(unitGuid);
                const bool before = old != threatLists_.end() && onList(old->second);
                if (!before && onList(list) && threatWarningWanted() && owner_.getPlayerLevel() > 13)
                    owner_.triggerTutorial(0x35);
            }
            threatLists_[unitGuid] = std::move(list);
            {
                // The player's status on the unit, as the client keeps it, for
                // its threat floater (0x0071c3b0, 0x00737620).
                const auto& l = threatLists_[unitGuid];
                std::vector<uint32_t> threats;
                threats.reserve(l.size());
                size_t mine = l.size();
                for (size_t i = 0; i < l.size(); ++i) {
                    threats.push_back(l[i].threat);
                    if (l[i].victimGuid == owner_.getPlayerGuid()) mine = i;
                }
                notePlayerThreatStatus(unitGuid, clientThreatStatus(threats.data(), threats.size(), mine));
            }
            if (!owner_.addonEventCallbackRef()) return;
            auto fire = owner_.addonEventCallbackRef();
            // The list changed, and separately: whether each unit on it is
            // tanking. They are different events and unit frames read the
            // second one - UnitFrameThreatIndicator_OnEvent is registered for
            // UNIT_THREAT_SITUATION_UPDATE alone, and it is what puts the aggro
            // border on a frame. It was never fired, so no frame ever showed
            // one, and the list it would have been read from was misparsed
            // anyway.
            fire("UNIT_THREAT_LIST_UPDATE", {owner_.guidToUnitId(unitGuid)});
            // Named per unit, because the indicator compares the argument
            // against its own frame's unit and ignores anything else. Every
            // unit on the list that the interface has a token for, plus the mob
            // itself - a target frame's indicator is fed by the player's
            // situation against the target, so both ends have to be told.
            for (const ThreatEntry& e : threatLists_[unitGuid]) {
                const std::string who = owner_.guidToUnitId(e.victimGuid);
                if (!who.empty()) fire("UNIT_THREAT_SITUATION_UPDATE", {who});
            }
            if (const std::string mob = owner_.guidToUnitId(unitGuid); !mob.empty())
                fire("UNIT_THREAT_SITUATION_UPDATE", {mob});
        };
    }

    // ---- Forced faction reactions ----
    table[Opcode::SMSG_SET_FORCED_REACTIONS] = [this](network::Packet& packet) { handleSetForcedReactions(packet); };

    // ---- Entity delta updates: health / power / combo / PvP / proc ----
    table[Opcode::SMSG_HEALTH_UPDATE] = [this](network::Packet& p) { handleHealthUpdate(p); };
    table[Opcode::SMSG_POWER_UPDATE] = [this](network::Packet& p) { handlePowerUpdate(p); };
    table[Opcode::SMSG_UPDATE_COMBO_POINTS] = [this](network::Packet& p) { handleUpdateComboPoints(p); };
    table[Opcode::SMSG_PET_UPDATE_COMBO_POINTS] = [this](network::Packet& p) { handlePetUpdateComboPoints(p); };
    table[Opcode::SMSG_PVP_CREDIT] = [this](network::Packet& p) { handlePvpCredit(p); };
    table[Opcode::SMSG_PROCRESIST] = [this](network::Packet& p) { handleProcResist(p); };

    // SMSG_ENVIRONMENTALDAMAGELOG is an alias for SMSG_ENVIRONMENTAL_DAMAGE_LOG
    // (registered above at line 108 with envType forwarding). No separate handler needed.
    table[Opcode::SMSG_SPELLDAMAGESHIELD] = [this](network::Packet& p) { handleSpellDamageShield(p); };
    table[Opcode::SMSG_SPELLORDAMAGE_IMMUNE] = [this](network::Packet& p) { handleSpellOrDamageImmune(p); };
    table[Opcode::SMSG_RESISTLOG] = [this](network::Packet& p) { handleResistLog(p); };

    // ---- Pet feedback ----
    table[Opcode::SMSG_PET_TAME_FAILURE] = [this](network::Packet& p) { handlePetTameFailure(p); };
    table[Opcode::SMSG_PET_ACTION_FEEDBACK] = [this](network::Packet& p) { handlePetActionFeedback(p); };
    table[Opcode::SMSG_PET_CAST_FAILED] = [this](network::Packet& p) { handlePetCastFailed(p); };
    table[Opcode::SMSG_PET_BROKEN] = [this](network::Packet& p) { handlePetBroken(p); };
    table[Opcode::SMSG_PET_LEARNED_SPELL] = [this](network::Packet& p) { handlePetLearnedSpell(p); };
    table[Opcode::SMSG_PET_UNLEARNED_SPELL] = [this](network::Packet& p) { handlePetUnlearnedSpell(p); };
    table[Opcode::SMSG_PET_MODE] = [this](network::Packet& p) { handlePetMode(p); };

    // ---- Resurrect ----
    table[Opcode::SMSG_RESURRECT_FAILED] = [this](network::Packet& p) { handleResurrectFailed(p); };
}

// ============================================================
// Auto-attack
// ============================================================

void CombatHandler::startAutoAttack(uint64_t targetGuid) {
    // Can't attack yourself
    if (targetGuid == owner_.getPlayerGuid()) return;
    if (targetGuid == 0) return;

    // What the player's own state forbids, said as 0x0072bdb0 says it, in its
    // order: dead (ERR_ATTACK_DEAD); charmed by someone else; then
    // UNIT_FIELD_FLAGS stunned (0x40000), pacified (0x20000), fleeing
    // (0x800000) and confused (0x400000). Each looks for the aura behind it
    // (0x008046c0, 0x00804590, 0x00804610, 0x00804660, 0x00804690) and, where
    // that aura has a mechanic, names it with ERR_ATTACK_PREVENTED_BY_MECHANIC_S
    // - "UNKNOWN" for a mechanic SpellMechanic.dbc has no row for.
    const auto refuse = [&](std::initializer_list<uint32_t> auraTypes, const char* key, const char* enUS) {
        const uint32_t mechanic = owner_.playerAuraMechanic(auraTypes);
        if (mechanic == 0) {
            owner_.addUIError(interfaceString(key, enUS));
            return;
        }
        const std::string* name = owner_.getSpellMechanicName(mechanic);
        owner_.addUIError(chat_channels::luaFormat(
            interfaceString("ERR_ATTACK_PREVENTED_BY_MECHANIC_S", "Can't attack while %s."),
            {name ? *name : std::string("UNKNOWN")}));
    };
    if (owner_.isPlayerDead() || owner_.isPlayerGhost()) {
        owner_.addUIError(interfaceString("ERR_ATTACK_DEAD", "You can't attack while dead."));
        return;
    }
    if (auto self = owner_.getEntityManager().getEntity(owner_.getPlayerGuid())) {
        const uint16_t charmedBy = fieldIndex(UF::UNIT_FIELD_CHARMEDBY);
        const uint64_t charmer = charmedBy == 0xFFFF ? 0
            : (static_cast<uint64_t>(self->getField(static_cast<uint16_t>(charmedBy + 1))) << 32) |
                  self->getField(charmedBy);
        if (charmer != 0 && charmer != owner_.getPlayerGuid()) {
            refuse({6u, 177u, 2u}, "ERR_ATTACK_CHARMED", "You can't attack while charmed.");
            return;
        }
        const uint16_t flagsIdx = fieldIndex(UF::UNIT_FIELD_FLAGS);
        const uint32_t flags = flagsIdx == 0xFFFF ? 0 : self->getField(flagsIdx);
        if ((flags & 0x00040000u) != 0) {
            refuse({12u}, "ERR_ATTACK_STUNNED", "You can't attack while stunned.");
            return;
        }
        if ((flags & 0x00020000u) != 0) {
            refuse({25u, 60u}, "ERR_ATTACK_PACIFIED", "You can't attack while pacified.");
            return;
        }
        if ((flags & 0x00800000u) != 0) {
            refuse({7u}, "ERR_ATTACK_FLEEING", "You can't attack while fleeing.");
            return;
        }
        if ((flags & 0x00400000u) != 0) {
            refuse({5u}, "ERR_ATTACK_CONFUSED", "You can't attack while confused.");
            return;
        }
        // Then a channel in progress (UNIT_CHANNEL_SPELL): no attack starts
        // while one runs, and ERR_ATTACK_CHANNEL says so when the channel is
        // one an attack would break (ChannelInterruptFlags 0x1000).
        const uint16_t channelIdx = fieldIndex(UF::UNIT_CHANNEL_SPELL);
        if (const uint32_t channel = channelIdx == 0xFFFF ? 0 : self->getField(channelIdx); channel != 0) {
            const auto& cache = owner_.spellNameCacheRef();
            if (const auto it = cache.find(channel); it != cache.end()) {
                if ((it->second.channelInterruptFlags & 0x1000u) != 0)
                    owner_.addUIError(interfaceString("ERR_ATTACK_CHANNEL", "You can't attack while channeling."));
                return;
            }
        }
    }

    // Assist Attack: swinging at a friendly unit attacks whatever they are
    // fighting instead. The panel offers this and nothing read it, so an
    // attack on an ally was simply sent and refused by the server - a tank
    // pressing attack with a healer selected got nothing and no reason for it.
    //
    // Only when the friend has a target of their own, and only when that
    // target is hostile: assisting somebody who is fighting nothing, or who
    // has a friendly unit selected, would turn one refusal into another.
    if (addons::storedCVarValue("assistAttack", "0") != "0") {
        auto entity = owner_.getEntityManager().getEntity(targetGuid);
        if (dynamic_cast<Unit*>(entity.get())) {
            // 0x00729740 with the player attacking.
            const uint64_t me = owner_.getPlayerGuid();
            if (!owner_.unitMayAttack(me, targetGuid)) {
                const uint64_t theirTarget = unitTargetGuid(entity);
                auto their = owner_.getEntityManager().getEntity(theirTarget);
                auto* theirUnit = dynamic_cast<Unit*>(their.get());
                if (theirUnit && owner_.unitMayAttack(me, theirTarget)) {
                    targetGuid = theirTarget;
                } else {
                    return;   // nothing of theirs worth swinging at
                }
            }
        }
    }

    // 0x006e4950 from a mount: out of the swing's reach it is refused, and the
    // player stays mounted; in it, down they get. On foot it is asked for at
    // any distance, and the server says where it is too far.
    //
    // Never in the air, unless Auto Dismount in Flight asks for it: a hostile
    // right-clicked from a flying mount dropped the player out of the sky.
    const auto victim = std::dynamic_pointer_cast<Unit>(owner_.getEntityManager().getEntity(targetGuid));
    const auto me = std::dynamic_pointer_cast<Unit>(owner_.getEntityManager().getEntity(owner_.getPlayerGuid()));
    if (owner_.isMounted()) {
        if (owner_.isPlayerFlying() && !owner_.isTaxiMountActive() &&
            addons::storedCVarValue("autoDismountFlying", "0") == "0") {
            // ERR_ATTACK_MOUNTED, as 0x0072bdb0 refuses an attack it will not
            // dismount for (0x0071b0c0). This was English.
            owner_.addUIError(interfaceString("ERR_ATTACK_MOUNTED", "Can't attack while mounted."));
            return;
        }
        if (victim && me) {
            const uint32_t ranged = owner_.autoRangedAttackSpell();
            auto* sh = owner_.getSpellHandler();
            const float reach = ranged != 0 && sh
                                    ? sh->spellReachAt(ranged, owner_.getPlayerGuid(), targetGuid).max
                                    : click_to_move::meleeReach(me->getCombatReach(), victim->getCombatReach());
            if (reach * reach < distanceSqTo(owner_.movementInfoRef(), *victim)) {
                owner_.raiseUiError(interfaceString("ERR_OUT_OF_RANGE", "Out of range."));
                return;
            }
        }
        owner_.dismount();
    }
    // 0x006e2610: one that may not be attacked now (0x00729a70) ends a running
    // attack and starts none.
    if (!victim || !me) return;
    if (!unit_target::mayAttackNow(owner_.isPlayerDead(), owner_.isMounted(), true, victim->getHealth(),
                                   victim->getDynamicFlags(),
                                   owner_.unitMayAttack(owner_.getPlayerGuid(), targetGuid))) {
        if (attack_.attacking()) stopAutoAttack();
        return;
    }
    // 0x00806550: the player's pending ranged casts (Attributes 0x2) are
    // cancelled, the server told of one with a cast time; then up from a seat
    // (0x006dcb40), and the one attacked is the target (0x00524bf0).
    if (auto* sh = owner_.getSpellHandler()) sh->cancelPendingCasts(0x2u);
    if (owner_.getStandState() != 0) owner_.setStandState(0);
    setTarget(targetGuid);

    // 0x008038f0: while one of the player's own casts in progress is one an
    // attack would interrupt (Spell.dbc InterruptFlags 0x8), the attack start
    // 0x006e2610 sends no CMSG_ATTACKSWING and starts nothing, unless an attack
    // is already under way. Nothing sends the held swing when the cast ends:
    // none of 0x006e2610's callers is reached from a cast finishing, so the
    // player asks again. (The channel half of 0x008038f0, ChannelInterruptFlags
    // 0x1000, is covered by the channel refusal above, which stops every
    // attack while a channel runs.)
    if (!attack_.attacking() && owner_.isCasting() && !owner_.isChanneling()) {
        owner_.loadSpellNameCache();
        const auto& cache = owner_.spellNameCacheRef();
        if (const auto it = cache.find(owner_.getCurrentCastSpellId());
            it != cache.end() && (it->second.interruptFlags & 0x8u) != 0)
            return;
    }

    // Face the target, once, on the command to attack it. This is the only
    // place combat turns the player: it used to be re-aimed every fifth of a
    // second for as long as the attack lasted.
    //
    // Only with the Combat page's debug setting on. The original client does
    // not turn the player at all; attacking something behind them is refused
    // until they face it themselves.
    auto target = owner_.isAutoFaceTarget() ? owner_.getEntityManager().getEntity(targetGuid)
                                            : nullptr;
    if (target) {
        const float toTargetX = target->getLatestX() - owner_.movementInfoRef().x;
        const float toTargetY = target->getLatestY() - owner_.movementInfoRef().y;
        if (std::abs(toTargetX) > 0.01f || std::abs(toTargetY) > 0.01f) {
            owner_.movementInfoRef().orientation = std::atan2(-toTargetY, toTargetX);
            if (owner_.getState() == WorldState::IN_WORLD && owner_.getSocket()) {
                owner_.sendMovement(Opcode::MSG_MOVE_SET_FACING);
            }
        }
    }
    // None while one is on or asked for, unless its stop is (0x006e2610).
    if (attack_.maySwing() && owner_.getState() == WorldState::IN_WORLD && owner_.getSocket()) {
        owner_.getSocket()->send(AttackSwingPacket::build(targetGuid));
        attack_.swingSent(targetGuid);
    }
    // 0x006e2610: swinging ends a repeating shot, the server told
    // (0x00807560(1)).
    owner_.stopAutoRepeat(true);
    LOG_INFO("Starting auto-attack on 0x", std::hex, targetGuid, std::dec);
}

void CombatHandler::stopAutoAttack() {
    // 0x006e1660: with autoRangedCombat on, an attack stopped stops Auto Shot
    // with it - the repeating spell the client recorded off a spell carrying
    // AttributesEx4 0x01000000 (0x00542030, 0x00be5d84) - whether or not a
    // swing was going.
    if (const uint32_t repeating = owner_.autoRepeatSpellId();
        repeating != 0 && addons::storedCVarValue("autoRangedCombat", "1") != "0") {
        const auto& cache = owner_.spellNameCacheRef();
        if (const auto it = cache.find(repeating);
            it != cache.end() && (it->second.attrEx4 & 0x01000000u) != 0) {
            owner_.stopAutoRepeat(true);
        }
    }
    if (!attack_.mayStop()) return;
    sendAttackStop();
    // 0x006e1660 -> 0x00806480 after the stop goes out: a cast waiting on the
    // next swing (Attributes 0x404) is cancelled with CMSG_CANCEL_CAST.
    if (auto* sh = owner_.getSpellHandler()) sh->cancelPendingCasts(0x404u);
    LOG_INFO("Stopping auto-attack");
    // 0x006e1660: the stop's tutorials.
    for (uint32_t id : attackStops_.note(owner_.getPlayerLevel(), owner_.tutorials())) owner_.triggerTutorial(id);
}

// The attack itself ends with the server's SMSG_ATTACKSTOP.
void CombatHandler::sendAttackStop() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) {
        attack_.stopped();
        return;
    }
    owner_.getSocket()->send(AttackStopPacket::build());
    attack_.stopSent();
}

// ============================================================
// Combat text
// ============================================================

namespace {
// The kinds of combat text that take health from their target.
bool combatTextIsDamage(CombatTextEntry::Type type) {
    switch (type) {
        case CombatTextEntry::MELEE_DAMAGE:
        case CombatTextEntry::SPELL_DAMAGE:
        case CombatTextEntry::CRIT_DAMAGE:
        case CombatTextEntry::PERIODIC_DAMAGE:
        case CombatTextEntry::ENVIRONMENTAL:
        case CombatTextEntry::GLANCING:
        case CombatTextEntry::CRUSHING:
            return true;
        default:
            return false;
    }
}
}  // namespace

void CombatHandler::addCombatText(CombatTextEntry::Type type, int32_t amount, uint32_t spellId, bool isPlayerSource, uint8_t powerType,
                                  uint64_t srcGuid, uint64_t dstGuid, bool spellFlag, bool periodic) {
    // A hit that did the player damage (playerDamageHits), before the
    // filters: the player is its target (by guid, or by no target given and
    // the player not its source), and it took health.
    if (amount > 0 && combatTextIsDamage(type) &&
        (dstGuid == owner_.getPlayerGuid() || (dstGuid == 0 && !isPlayerSource))) {
        ++playerDamageHits_;
    }
    // Over the world, only what 0x0071f990, 0x00722340 and 0x00722440 would
    // draw, under their settings (combat_text_filter.hpp). The rest - what
    // happens to the character - is Blizzard_CombatText's, and the log and
    // UNIT_COMBAT below are not filtered by these.
    bool floater = false;
    uint32_t floaterArgb = 0;
    if (dstGuid != 0) {
        const auto on = [](const char* cvar) { return addons::storedCVarValue(cvar, "1") != "0"; };
        FloaterSettings settings;
        settings.combatDamage = on("CombatDamage");
        settings.periodicSpells = on("CombatLogPeriodicSpells");
        settings.petMeleeDamage = on("PetMeleeDamage");
        settings.petSpellDamage = on("PetSpellDamage");
        settings.combatHealing = on("CombatHealing");
        // A swing (0x00722340): no maker's flag, and no spell or a weapon one.
        bool meleeLike = false;
        if (!spellFlag) {
            meleeLike = spellId == 0;
            if (!meleeLike) {
                owner_.getSpellName(spellId);  // loads the cache
                const auto it = owner_.spellNameCacheRef().find(spellId);
                meleeLike = it != owner_.spellNameCacheRef().end() &&
                            floaterIsSwing(false, true, it->second.attrEx3);
            }
        }
        const auto source = static_cast<FloaterSource>(floaterSourceOf(srcGuid, isPlayerSource));
        floater = worldFloaterShown(type, source, dstGuid == owner_.getPlayerGuid(), periodic, meleeLike, settings);
        floaterArgb = floaterColour(type, source, meleeLike);
    }

    CombatTextEntry entry;
    entry.type = type;
    entry.amount = amount;
    entry.spellId = spellId;
    entry.age = 0.0f;
    entry.isPlayerSource = isPlayerSource;
    entry.powerType = powerType;
    entry.srcGuid = srcGuid;
    entry.dstGuid = dstGuid;
    // Drawn as its kind (0x0071f990, 0x00722340, 0x00722440); the client
    // staggers nothing.
    entry.kind = floaterKindOf(type);
    entry.colour = floaterArgb;
    if (floater) combatText_.push_back(entry);

    // UNIT_COMBAT is not raised from here: the client raises it from each
    // log packet (GameHandler::fireUnitCombat), whatever the floating-text
    // settings, for every unit with a token.

    // Persistent combat log - use explicit GUIDs if provided, else fall back to
    // player/current-target (the old behaviour for events without specific participants).
    CombatLogEntry log;
    log.type     = type;
    log.amount   = amount;
    log.spellId  = spellId;
    log.isPlayerSource = isPlayerSource;
    log.powerType = powerType;
    log.timestamp = std::time(nullptr);
    // If the caller provided an explicit destination GUID but left source GUID as 0,
    // preserve "unknown/no source" (e.g. environmental damage) instead of
    // backfilling from current target.
    uint64_t effectiveSrc = (srcGuid != 0) ? srcGuid
                          : ((dstGuid != 0) ? 0 : (isPlayerSource ? owner_.getPlayerGuid() : owner_.getTargetGuid()));
    uint64_t effectiveDst = (dstGuid != 0) ? dstGuid
                          : (isPlayerSource ? owner_.getTargetGuid() : owner_.getPlayerGuid());
    log.sourceName = owner_.lookupName(effectiveSrc);
    log.targetName = (effectiveDst != 0) ? owner_.lookupName(effectiveDst) : std::string{};
    if (combatLog_.size() >= MAX_COMBAT_LOG)
        combatLog_.pop_front();
    combatLog_.push_back(std::move(log));
    // The interface's combat log is not this one: it is filled by the log
    // packets themselves (CombatLogFeed), whether or not a line is drawn here.
}

int CombatHandler::floaterSourceOf(uint64_t src, bool isPlayerSource) const {
    const uint64_t player = owner_.getPlayerGuid();
    if (src == 0) src = isPlayerSource ? player : 0;
    if (src == 0) return static_cast<int>(FloaterSource::Other);
    if (src == player) return static_cast<int>(FloaterSource::Player);
    // CHARMEDBY, else CREATEDBY (0x007190a0 reads +0x18, else +0x28).
    const auto ownerOf = [this](uint64_t guid) -> uint64_t {
        const auto e = owner_.getEntityManager().getEntity(guid);
        if (!e) return 0;
        const auto at = [&](UF f) -> uint64_t {
            const uint16_t i = fieldIndex(f);
            if (i == 0xFFFF) return 0;
            return e->getField(i) | (static_cast<uint64_t>(e->getField(static_cast<uint16_t>(i + 1))) << 32);
        };
        const uint64_t charmer = at(UF::UNIT_FIELD_CHARMEDBY);
        return charmer != 0 ? charmer : at(UF::UNIT_FIELD_CREATEDBY);
    };
    const uint64_t master = ownerOf(src);
    if (master == 0) return static_cast<int>(FloaterSource::Other);
    if (master == player || ownerOf(master) == player) return static_cast<int>(FloaterSource::PlayersUnit);
    return static_cast<int>(FloaterSource::Other);
}

void CombatHandler::addWorldText(uint64_t unit, uint8_t kind, const std::string& text, bool replace, uint32_t rank) {
    if (unit == 0 || text.empty()) return;
    if (replace) {
        combatText_.erase(std::remove_if(combatText_.begin(), combatText_.end(),
                                         [&](const CombatTextEntry& e) {
                                             return e.type == CombatTextEntry::WORLD_TEXT && e.dstGuid == unit &&
                                                    e.worldKind == kind;
                                         }),
                          combatText_.end());
    }
    CombatTextEntry entry;
    entry.type = CombatTextEntry::WORLD_TEXT;
    entry.dstGuid = unit;
    entry.text = text;
    entry.worldKind = kind;
    entry.kind = kind;
    entry.rank = rank;
    // A spell mechanic's line is the spell colour (0x0074da70 hands
    // 0x00adb93c); the rest take their kind's.
    if (kind == static_cast<uint8_t>(FloaterKind::Mechanic)) entry.colour = world_text::kSpellColour;
    combatText_.push_back(std::move(entry));
}

void CombatHandler::addMechanicFloater(uint64_t unit, uint64_t caster, uint32_t spellAttrEx6, uint32_t mechanicId) {
    // 0x0074da00: AttributesEx6 0x200000 and the fct settings.
    if ((spellAttrEx6 & 0x200000u) == 0) return;
    const auto on = [](const char* cvar) {
        try { return std::stoi(addons::storedCVarValue(cvar, "0")); } catch (...) { return 0; }
    };
    const bool concernsPlayer = caster == owner_.getPlayerGuid();
    if (!mechanicFloaterShown(on("fctAllSpellMechanics") > 0, on("fctSpellMechanics") != 0,
                              on("fctSpellMechanicsOther") != 0, concernsPlayer))
        return;
    const std::string* name = owner_.getSpellMechanicName(mechanicId);
    if (!name || name->empty()) return;
    // 0x007e1130: the first letter upper-cased.
    std::string text = *name;
    if (static_cast<unsigned char>(text[0]) < 0x80) text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    // "%s (%s)" with the caster's name, unless the caster is the player.
    if (!concernsPlayer) text += " (" + owner_.lookupName(caster) + ")";
    addWorldText(unit, static_cast<uint8_t>(FloaterKind::Mechanic), text);
}

void CombatHandler::addMechanicFloaters(uint64_t unit, uint64_t caster, uint32_t spellId) {
    owner_.getSpellName(spellId);
    const auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end()) return;
    const auto& s = it->second;
    // 0x0074d3f0: not a spell hidden from the log, one that teaches a spell
    // (effect 36), nor one with AttributesEx4 0x1.
    if ((s.attr & 0x180u) != 0 || s.name.empty() || (s.attrEx4 & 0x1u) != 0) return;
    for (uint32_t e : s.effectIds) {
        if (e == 36) return;
        if (e == 33) return;  // 0x0074db80: an opening spell shows none
    }
    // Each effect's mechanic once, then the spell's own where no effect has
    // it; never SpellMechanic 26.
    constexpr uint32_t kMechanicInterrupt = 26;
    for (int i = 0; i < 3; ++i) {
        const uint32_t m = s.effectMechanic[i];
        if (m == 0 || m == kMechanicInterrupt) continue;
        bool seen = false;
        for (int j = 0; j < i; ++j) seen = seen || s.effectMechanic[j] == m;
        if (!seen) addMechanicFloater(unit, caster, s.attrEx6, m);
    }
    const uint32_t own = s.mechanic;
    if (own != 0 && own != kMechanicInterrupt && own != s.effectMechanic[0] && own != s.effectMechanic[1] &&
        own != s.effectMechanic[2])
        addMechanicFloater(unit, caster, s.attrEx6, own);
}

void CombatHandler::addInterruptFloater(uint64_t unit, uint64_t caster, uint32_t spellId) {
    owner_.getSpellName(spellId);
    const auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end()) return;
    addMechanicFloater(unit, caster, it->second.attrEx6, 26);
}

void CombatHandler::notePlayerThreatStatus(uint64_t unit, int now) {
    int& was = playerThreatStatus_[unit];
    const int before = was;
    was = now;
    if (now == 0) playerThreatStatus_.erase(unit);
    if (before == now) return;
    // 0x00719220: while threat warnings apply and threatWorldText is on.
    if (!threatWarningWanted() || addons::storedCVarValue("threatWorldText", "1") == "0") return;
    if (!threatFloaterShown(before, now)) return;
    const std::string key = std::string("COMBAT_THREAT_") + (now > before ? "INCREASE" : "DECREASE") + "_" +
                            std::to_string(now);
    const std::string text = interfaceString(key.c_str(), nullptr);
    if (!text.empty()) addWorldText(unit, static_cast<uint8_t>(FloaterKind::Threat), text, true);
}

bool CombatHandler::shouldLogSpellstealAura(uint64_t casterGuid, uint64_t victimGuid, uint32_t spellId) {
    if (spellId == 0) return false;

    const auto now = std::chrono::steady_clock::now();
    constexpr auto kRecentWindow = std::chrono::seconds(1);
    while (!recentSpellstealLogs_.empty() &&
           now - recentSpellstealLogs_.front().timestamp > kRecentWindow) {
        recentSpellstealLogs_.pop_front();
    }

    for (auto it = recentSpellstealLogs_.begin(); it != recentSpellstealLogs_.end(); ++it) {
        if (it->casterGuid == casterGuid &&
            it->victimGuid == victimGuid &&
            it->spellId == spellId) {
            recentSpellstealLogs_.erase(it);
            return false;
        }
    }

    if (recentSpellstealLogs_.size() >= MAX_RECENT_SPELLSTEAL_LOGS)
        recentSpellstealLogs_.pop_front();
    recentSpellstealLogs_.push_back({.casterGuid = casterGuid, .victimGuid = victimGuid, .spellId = spellId, .timestamp = now});
    return true;
}

void CombatHandler::updateCombatText(float deltaTime) {
    for (auto& entry : combatText_) {
        entry.age += deltaTime;
    }
    combatText_.erase(
        std::remove_if(combatText_.begin(), combatText_.end(),
                       [](const CombatTextEntry& e) { return e.isExpired(); }),
        combatText_.end());
}

// ============================================================
// Packet handlers
// ============================================================

void CombatHandler::autoTargetAttacker(uint64_t attackerGuid) {
    if (attackerGuid == 0 || attackerGuid == owner_.getPlayerGuid()) return;
    if (owner_.getTargetGuid() != 0) return;
    if (!owner_.getEntityManager().hasEntity(attackerGuid)) return;
    // 0x00755020, at a swing at the player (0x00756800 case 0x14a) and a
    // harmful spell's hit (0x008063a0).
    targetUnit(attackerGuid);
}

void CombatHandler::handleAttackStart(network::Packet& packet) {
    AttackStartData data;
    if (!AttackStartParser::parse(packet, data)) return;

    // CREEP is a client presentation flag, not an instruction to keep a unit
    // translucent after it has openly engaged. The stock client reveals both
    // combatants at ATTACKSTART.
    //
    // The reason recorded here used to be that "some legacy realms do not send
    // a matching UNIT_FIELD_BYTES_1 update when an NPC breaks stealth". That
    // was almost certainly this client's own fault: UNIT_FIELD_BYTES_1 was
    // being read from field 137 where the server writes 74, so the update
    // arrived and was taken from the wrong slot. Fixed 2026-08-05. This is
    // kept because the stock client does it anyway, but if a stealth-break
    // ever needs looking at again, start by checking whether the field now
    // arrives - the premise behind this workaround is gone. Clear the cached
    // presentation state here so the normal creature visual sync restores full
    // opacity instead of leaving an invisible monster in melee.
    auto revealCombatant = [this](uint64_t guid) {
        auto entity = owner_.getEntityManager().getEntity(guid);
        if (!entity || !entity->isUnit()) return;
        auto unit = std::static_pointer_cast<Unit>(entity);
        if (!unit->hasCreepVisibility()) return;
        unit->clearCreepVisibility();
    };
    revealCombatant(data.attackerGuid);
    revealCombatant(data.victimGuid);
    // 0x00756800 case 0x143: the attacker's melee target (0x00754ff0).
    if (data.attackerGuid != owner_.getPlayerGuid()) unitMeleeTargets_[data.attackerGuid] = data.victimGuid;

    if (data.attackerGuid == owner_.getPlayerGuid()) {
        attack_.started(data.victimGuid);
        if (owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("PLAYER_ENTER_COMBAT", {});
    } else if (data.victimGuid == owner_.getPlayerGuid() && data.attackerGuid != 0) {
        // The attacker is kept as a hostile one. 0x00756800 case 0x143 asks
        // nothing to be said here - a unit reaching for the player says
        // nothing about it; the line only a reaction change brings
        // (SMSG_AI_REACTION) is where an angry unit speaks. Nor is it taken
        // for the target before it has swung.
        hostileAttackers_.insert(data.attackerGuid);
    }

    // Force both participants to face each other at combat start.
    // Uses atan2(-dy, dx): canonical orientation convention where the West/Y
    // component is negated (renderYaw = orientation + 90°, model-forward = render+X).
    auto attackerEnt = owner_.getEntityManager().getEntity(data.attackerGuid);
    auto victimEnt   = owner_.getEntityManager().getEntity(data.victimGuid);
    if (attackerEnt && victimEnt) {
        float dx = victimEnt->getX() - attackerEnt->getX();
        float dy = victimEnt->getY() - attackerEnt->getY();
        if (std::abs(dx) > 0.01f || std::abs(dy) > 0.01f) {
            attackerEnt->setOrientation(std::atan2(-dy,  dx));   // attacker → victim
            victimEnt->setOrientation  (std::atan2( dy, -dx));   // victim   → attacker
        }
    }
}

void CombatHandler::handleAttackStop(network::Packet& packet) {
    AttackStopData data;
    if (!AttackStopParser::parse(packet, data)) return;

    // 0x00756770: the attacker's melee target is gone, and the hit it
    // kept settled.
    unitMeleeTargets_.erase(data.attackerGuid);
    if (owner_.attackStopCallbackRef()) owner_.attackStopCallbackRef()(data.attackerGuid);
    if (data.attackerGuid == owner_.getPlayerGuid()) {
        attack_.stopped();
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("PLAYER_LEAVE_COMBAT", {});
    } else if (data.victimGuid == owner_.getPlayerGuid()) {
        hostileAttackers_.erase(data.attackerGuid);
    }
}

void CombatHandler::handleAttackerStateUpdate(network::Packet& packet) {
    AttackerStateUpdateData data;
    if (!owner_.getPacketParsers()->parseAttackerStateUpdate(packet, data)) return;
    // 0x00755630's last word: a unit victim that took nothing of a blocked
    // swing blocked it.
    if (data.totalDamage == 0 && data.blocked != 0 && owner_.getUnitByGuid(data.targetGuid)) data.victimState = 5;
    // 0x007512b0, first of all: the rage a swing gave goes on the attacker's
    // predicted rage (0x00722cf0), whoever the attacker is.
    if ((data.hitInfo & 0x800000u) != 0) owner_.addPredictedPower(data.attackerGuid, 1, data.rageGained);
    // Then the combat log, whoever fought.
    owner_.combatLogFeed().swing(data);
    bool isPlayerAttacker = (data.attackerGuid == owner_.getPlayerGuid());
    bool isPlayerTarget = (data.targetGuid == owner_.getPlayerGuid());
    // 0x00756800 case 0x14a: any attacker's swing draws its melee weapons.
    if (data.attackerGuid != 0 && owner_.unitAttackSwingCallbackRef()) {
        owner_.unitAttackSwingCallbackRef()(data.attackerGuid);
    }
    // Kept on the attacker for its model's events (game/melee_landing.hpp),
    // which settle it and land it.
    if (owner_.meleeHitCallbackRef()) {
        owner_.meleeHitCallbackRef()(data);
    } else {
        const melee_landing::Blow blow = data.blow();
        settleMeleeBlow(blow);
        bleedFromMeleeBlow(blow);
    }
    if (!isPlayerAttacker && !isPlayerTarget) return;  // Not our combat

    // 0x006d4ad0: the player's swing came off.
    if (isPlayerAttacker) {
        const auto victim = std::dynamic_pointer_cast<Unit>(owner_.getEntityManager().getEntity(data.targetGuid));
        const auto me = std::dynamic_pointer_cast<Unit>(owner_.getEntityManager().getEntity(data.attackerGuid));
        if (victim && me) {
            swingRefusal_.clear(steadyMs());
            if (swingLeftBehind(distanceSqTo(owner_.movementInfoRef(), *victim), me->getCombatReach(),
                                victim->getCombatReach())) {
                sendAttackStop();
            }
        }
    }
    if (isPlayerAttacker) {
        lastMeleeSwingMs_ = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        // Skip melee animation if a ranged shot was just triggered from
        // SMSG_SPELL_GO (Auto Shot / Shoot / Throw).  The ranged animation
        // is already playing; firing the melee callback here would override it.
        if (owner_.consumeSuppressMeleeSwingAnim()) {
            LOG_DEBUG("Suppressed melee swing anim - ranged shot already triggered");
        } else {
            if (owner_.meleeSwingCallbackRef()) owner_.meleeSwingCallbackRef()(0);
        }
    }
    if (!isPlayerAttacker && owner_.npcSwingCallbackRef()) {
        owner_.npcSwingCallbackRef()(data.attackerGuid);
    }
    // Its voice as the hit arrives, and its victim's as it lands, are their
    // sound slots', played with the swing's sounds (rendering/melee_sound.hpp).

    if (isPlayerTarget && data.attackerGuid != 0) {
        hostileAttackers_.insert(data.attackerGuid);
        autoTargetAttacker(data.attackerGuid);
    }
}

bool CombatHandler::combatTextOurs(uint64_t src, uint64_t dst) const {
    const uint64_t player = owner_.getPlayerGuid();
    return src == player || dst == player ||
           floaterSourceOf(src, false) != static_cast<int>(FloaterSource::Other);
}

void CombatHandler::settleMeleeBlow(const melee_landing::Blow& blow) {
    if (!melee_landing::said(blow)) return;
    const bool onPlayer = blow.target == owner_.getPlayerGuid();
    if (const auto type = melee_landing::text(blow, onPlayer); type && combatTextOurs(blow.attacker, blow.target))
        addCombatText(*type, blow.damage, 0, blow.attacker == owner_.getPlayerGuid(), 0, blow.attacker, blow.target);
    owner_.fireUnitCombat(blow.target, unit_combat::meleeAction(blow.victimState), blow.damage, blow.school, blow.hitInfo);
}

void CombatHandler::bleedFromMeleeBlow(const melee_landing::Blow& blow) {
    if (!owner_.bloodSpurtCallbackRef() || !melee_landing::bleeds(blow)) return;
    const auto victim = std::dynamic_pointer_cast<Unit>(owner_.getEntityManager().getEntity(blow.target));
    if (victim && (victim->getDynamicFlags() & UNIT_DYNFLAG_LOOTABLE) == 0)
        owner_.bloodSpurtCallbackRef()(blow.target, blow.attacker, (blow.hitInfo & melee_landing::kCrushing) != 0);
}

void CombatHandler::handleSpellDamageLog(network::Packet& packet) {
    SpellDamageLogData data;
    if (!owner_.getPacketParsers()->parseSpellDamageLog(packet, data)) return;

    // 0x006d3840: split damage is logged as such; otherwise the caster is
    // the source, or the target itself under hit flag 0x10.
    if (isActiveExpansion("wotlk")) {
        auto& feed = owner_.combatLogFeed();
        if ((data.hitFlags & 0x8u) != 0) {
            feed.damageSplit(data.attackerGuid, data.targetGuid, data.spellId, static_cast<int32_t>(data.damage),
                             static_cast<int32_t>(data.overkill), data.schoolMask, static_cast<int32_t>(data.absorbed),
                             static_cast<int32_t>(data.resisted), static_cast<int32_t>(data.blocked), data.hitFlags);
        } else {
            const uint64_t src = (data.hitFlags & 0x10u) != 0 ? data.targetGuid : data.attackerGuid;
            feed.spellDamage(src, data.targetGuid, data.spellId, static_cast<int32_t>(data.damage),
                             static_cast<int32_t>(data.overkill), data.schoolMask, static_cast<int32_t>(data.absorbed),
                             static_cast<int32_t>(data.resisted), static_cast<int32_t>(data.blocked), data.hitFlags,
                             data.periodicLog != 0);
        }
    }

    // UNIT_COMBAT for the target, whoever cast (0x006d3840): a WOUND of the
    // damage in the spell's school, for a spell the client knows that is not
    // hidden from the log (Attributes 0x100).
    if (unit_combat::spellRaises(owner_.getSpellAttributes(data.spellId)))
        owner_.fireUnitCombat(data.targetGuid, "WOUND", static_cast<int32_t>(data.damage), data.schoolMask,
                              unit_combat::hitInfoFromLog(data.absorbed, data.resisted, data.blocked,
                                                          (data.hitFlags & 0x2u) != 0));

    bool isPlayerSource = (data.attackerGuid == owner_.getPlayerGuid());
    bool isPlayerTarget = (data.targetGuid == owner_.getPlayerGuid());
    if (isPlayerTarget && data.attackerGuid != 0) {
        hostileAttackers_.insert(data.attackerGuid);
        autoTargetAttacker(data.attackerGuid);
    }

    // One text over the target, the damage or why there was none; none of a
    // share of split damage, nor of a spell the log hides (0x006d3840).
    if ((data.hitFlags & 0x8u) != 0 || !unit_combat::spellRaises(owner_.getSpellAttributes(data.spellId))) return;
    if (!combatTextOurs(data.attackerGuid, data.targetGuid)) return;
    if (const auto type = spellDamageText(data.damage, data.absorbed, data.blocked, data.resisted, data.isCrit))
        addCombatText(*type, static_cast<int32_t>(data.damage), data.spellId, isPlayerSource, 0, data.attackerGuid,
                      data.targetGuid);
}

void CombatHandler::handleSpellHealLog(network::Packet& packet) {
    SpellHealLogData data;
    if (!owner_.getPacketParsers()->parseSpellHealLog(packet, data)) return;
    // 0x006d3c30 -> 0x00751f70, periodic by the spell's AttributesEx3.
    if (isActiveExpansion("wotlk")) {
        owner_.combatLogFeed().spellHeal(data.casterGuid, data.targetGuid, data.spellId,
                                         static_cast<int32_t>(data.heal), static_cast<int32_t>(data.overheal),
                                         static_cast<int32_t>(data.absorbed), data.isCrit,
                                         owner_.combatLogFeed().periodicByAttribute(data.spellId));
    }

    // UNIT_COMBAT for the target (0x006d3c30 -> 0x00513480): a HEAL of a
    // non-zero amount, physical school, CRITICAL when it crit.
    if (data.heal != 0 && unit_combat::spellRaises(owner_.getSpellAttributes(data.spellId)))
        owner_.fireUnitCombat(data.targetGuid, "HEAL", static_cast<int32_t>(data.heal),
                              unit_combat::kPhysicalSchoolMask, data.isCrit ? 0x200u : 0u);

    bool isPlayerSource = (data.casterGuid == owner_.getPlayerGuid());
    if (!combatTextOurs(data.casterGuid, data.targetGuid)) return;

    // The amount (0x00722440), or an absorb that left none (0x006d3c30).
    const auto attributes = owner_.getSpellAttributes(data.spellId);
    if (data.heal > 0) {
        if (healTextShown(attributes, false, 0))
            addCombatText(data.isCrit ? CombatTextEntry::CRIT_HEAL : CombatTextEntry::HEAL,
                          static_cast<int32_t>(data.heal), data.spellId, isPlayerSource, 0, data.casterGuid,
                          data.targetGuid);
    } else if (data.absorbed > 0 && unit_combat::spellRaises(attributes))
        addCombatText(CombatTextEntry::ABSORB, static_cast<int32_t>(data.absorbed), data.spellId, isPlayerSource, 0, data.casterGuid, data.targetGuid);
}

void CombatHandler::handleSetForcedReactions(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    uint32_t count = packet.readUInt32();
    if (count > 64) {
        LOG_WARNING("SMSG_SET_FORCED_REACTIONS: suspicious count ", count, ", ignoring");
        packet.skipAll();
        return;
    }
    forcedReactions_.clear();
    for (uint32_t i = 0; i < count; ++i) {
        if (!packet.hasRemaining(8)) break;
        uint32_t factionId = packet.readUInt32();
        uint32_t reaction  = packet.readUInt32();
        forcedReactions_[factionId] = static_cast<uint8_t>(reaction);
    }
    LOG_INFO("SMSG_SET_FORCED_REACTIONS: ", forcedReactions_.size(), " faction overrides");
    // 0x005d06a0 answers before the standings: units are judged again.
    owner_.refreshUnitHostility();
}

// ============================================================
// Per-frame update
// ============================================================

void CombatHandler::updateAutoAttack() {
    // 0x006d5eb0: ERR_BADATTACKPOS and ERR_BADATTACKFACING, from the server's
    // refusal on. No reach of the client's own has a say: measured from centre
    // to centre here, a large creature hit at its edge was "too far away"
    // every four seconds, and a swing at one was never asked for.
    switch (swingRefusal_.due(attack_.attacking(), steadyMs())) {
        case SwingRefusal::Why::Distance:
            owner_.raiseUiError(interfaceString("ERR_BADATTACKPOS", "You are too far away!"));
            break;
        case SwingRefusal::Why::Facing:
            owner_.raiseUiError(interfaceString("ERR_BADATTACKFACING", "You are facing the wrong way!"));
            break;
        case SwingRefusal::Why::None:
            break;
    }

    // Keep active melee attackers visually facing the player as positions change.
    if (!hostileAttackers_.empty()) {
        for (uint64_t attackerGuid : hostileAttackers_) {
            auto attacker = owner_.getEntityManager().getEntity(attackerGuid);
            if (!attacker) continue;
            float dx = owner_.movementInfoRef().x - attacker->getX();
            float dy = owner_.movementInfoRef().y - attacker->getY();
            if (std::abs(dx) < 0.01f && std::abs(dy) < 0.01f) continue;
            attacker->setOrientation(std::atan2(-dy, dx));
        }
    }
}

// ============================================================
// State management
// ============================================================

void CombatHandler::forgetAttack() {
    attack_.stopped();
    swingRefusal_.clear(steadyMs());
}

void CombatHandler::resetAllCombatState() {
    hostileAttackers_.clear();
    unitMeleeTargets_.clear();
    combatText_.clear();
    forgetAttack();
    lastMeleeSwingMs_ = 0;
}

void CombatHandler::removeHostileAttacker(uint64_t guid) {
    hostileAttackers_.erase(guid);
}

void CombatHandler::clearCombatText() {
    combatText_.clear();
}

void CombatHandler::removeCombatTextForGuid(uint64_t guid) {
    combatText_.erase(
        std::remove_if(combatText_.begin(), combatText_.end(),
            [guid](const CombatTextEntry& e) {
                return e.dstGuid == guid;
            }),
        combatText_.end());
}

// ============================================================
// Moved opcode handlers (from GameHandler::registerOpcodeHandlers)
// ============================================================

void CombatHandler::handleHealthUpdate(network::Packet& packet) {
    const bool huTbc = isActiveExpansion("tbc");
    if (!packet.hasRemaining(huTbc ? 8u : 2u) ) return;
    uint64_t guid = huTbc ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(4)) return;
    uint32_t hp = packet.readUInt32();
    // 0x00716d20: the predicted health alone, and no event - the field and
    // UNIT_HEALTH are the update's. The frames that show it poll it while
    // predictedHealth is on.
    if (auto* unit = owner_.getUnitByGuid(guid)) unit->setPredictedHealth(static_cast<int32_t>(hp));
}

void CombatHandler::handlePowerUpdate(network::Packet& packet) {
    const bool puTbc = isActiveExpansion("tbc");
    if (!packet.hasRemaining(puTbc ? 8u : 2u) ) return;
    uint64_t guid = puTbc ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(5)) return;
    uint8_t  powerType = packet.readUInt8();
    uint32_t value     = packet.readUInt32();
    // 0x007236c0: the predicted power alone; the field is the update's.
    if (auto* unit = owner_.getUnitByGuid(guid)) owner_.setPredictedPower(*unit, powerType, static_cast<int32_t>(value));
}

void CombatHandler::handleUpdateComboPoints(network::Packet& packet) {
    const bool cpTbc = isActiveExpansion("tbc");
    if (!packet.hasRemaining(cpTbc ? 8u : 2u) ) return;
    uint64_t target = cpTbc ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(1)) return;
    owner_.comboPointsRef() = packet.readUInt8();
    owner_.comboTargetRef() = target;
    LOG_DEBUG("SMSG_UPDATE_COMBO_POINTS: target=0x", std::hex, target,
              std::dec, " points=", static_cast<int>(owner_.comboPointsRef()));
    // 0x00519780: the count and its unit are stored, every action and form is
    // re-checked through the whole cast check (0x0053cf10), and
    // UNIT_COMBO_POINTS (149) goes out with the player's own token as its one
    // argument. ComboFrame_OnEvent ignores the event unless that token is the
    // one PlayerFrame is showing, so the argument is not decoration:
    // "PLAYER_COMBO_POINTS" carried none, is not a name this client has at all
    // (zero times in Wow.exe, against once for UNIT_COMBO_POINTS), and nothing
    // in the shipped interface registers it - the combo points only ever lit up
    // when the target changed, which is the other event ComboFrame listens for.
    //
    // The rescan that comes with the store is a thread still open. 0x0053cf10
    // walks the action list (DAT_00be8e58 of them, DAT_00be8e5c holding each
    // slot's two flags) and the form list (DAT_00be8e28) through the whole cast
    // check; the first thing it calls, 0x005aa470, rewrites the 144 per-slot
    // pairs the buttons are drawn from (DAT_00c1ded8, DAT_00c1dc98) and
    // announces a change with ACTIONBAR_UPDATE_USABLE (0xb2). A form flipping is
    // UPDATE_SHAPESHIFT_USABLE (0x17a), a spell of that list becoming affordable
    // again says so as COMBAT_TEXT_UPDATE("SPELL_ACTIVE", name) - 0x0074d520 is
    // not a sound: it fires 0x1e4 with "%s%s", the type name being that call's
    // first argument read out of the combat-text type table at 0xadb820, slot
    // 0x23 - and the rescan's own name SPELL_UPDATE_USABLE (0xf5)
    // reaches no frame of this interface. WoWee's action flags hold the power
    // half only (0x0053d1b0, the power-only variant), which is the half the
    // buttons actually read: IsUsableAction (0x005a7e60) reads nothing else
    // either, and a finisher wanting a combo point stays bright on the client's
    // bar too - the points gate the *cast* (0x0080b5d0, through the per-target
    // store 0x00513920), which WoWee already checks. What the rescan would add
    // is the cast-check walk over the slots behind those pairs, so a button can
    // go grey for the other reasons; announcing ACTIONBAR_UPDATE_USABLE from
    // this packet only pays once that walk exists.
    owner_.fireAddonEvent("UNIT_COMBO_POINTS", {"player"});
}

void CombatHandler::handlePetUpdateComboPoints(network::Packet& packet) {
    // 0x005d36a0: the pet's packed guid, the unit's, the points - kept
    // for the first pet only (0x005d34b0).
    if (!packet.hasRemaining(2)) return;
    const uint64_t pet = packet.readPackedGuid();
    if (!packet.hasRemaining(2)) return;
    const uint64_t target = packet.readPackedGuid();
    if (!packet.hasRemaining(1)) return;
    const uint8_t points = packet.readUInt8();
    if (pet != owner_.getPetGuid()) return;
    owner_.petComboTargetRef() = target;
    owner_.petComboPointsRef() = points;
    // 0x005d34b0: the same store for the pet, the same rescan, then
    // UNIT_COMBO_POINTS (149) with the pet's token and PET_BAR_UPDATE_USABLE
    // (0x15e). The second is what the pet bar waits for - the buttons it holds
    // re-read their state on that name alone (petactionbarframe.lua:26, :47),
    // and this site used to announce neither, so a pet working its combo points
    // up left its bar untouched.
    owner_.fireAddonEvent("UNIT_COMBO_POINTS", {"pet"});
    owner_.fireAddonEvent("PET_BAR_UPDATE_USABLE", {});
}

void CombatHandler::handlePvpCredit(network::Packet& packet) {
    if (packet.hasRemaining(16)) {
        uint32_t honor      = packet.readUInt32();
        uint64_t victimGuid = packet.readUInt64();
        uint32_t rank       = packet.readUInt32();
        LOG_INFO("SMSG_PVP_CREDIT: honor=", honor, " victim=0x", std::hex, victimGuid, std::dec, " rank=", rank);
        // As 0x0050b6d0 words it, as CHAT_MSG_COMBAT_HONOR_GAIN: with no
        // victim, COMBATLOG_HONORAWARD; for a victim it can see and some
        // honour, COMBATLOG_HONORGAIN_NO_RANK where the rank is negative.
        // This was "You gain N honor points." in English.
        //
        // A ranked kill is COMBATLOG_HONORGAIN with the victim's
        // PVP_RANK_<rank>_<n> title, n chosen from their race's flags,
        // which this client does not read; that one keeps the English.
        std::string msg;
        if (victimGuid == 0) {
            msg = interfaceFormat("COMBATLOG_HONORAWARD", "You have been awarded %d honor points.",
                                  {std::to_string(honor)});
        } else if (static_cast<int32_t>(honor) > 0 && owner_.getEntityManager().getEntity(victimGuid)) {
            const std::string victimName = owner_.lookupName(victimGuid);
            if (static_cast<int32_t>(rank) < 0) {
                msg = interfaceFormat("COMBATLOG_HONORGAIN_NO_RANK", "%s dies, honorable kill (%d Honor Points)",
                                      {victimName, std::to_string(honor)});
            } else {
                msg = "You gain " + std::to_string(honor) + " honor points.";
            }
        }
        if (!msg.empty()) owner_.addLocalChatLine(ChatType::COMBAT_HONOR_GAIN, msg);
        if (honor > 0) addCombatText(CombatTextEntry::HONOR_GAIN, static_cast<int32_t>(honor), 0, true);
        // 0x0050b6d0 -> 0x007155d0: a ranked victim this client has, for some
        // honour, gets "HK: <rank title>" over it (world text kind 5). The
        // title is PVP_RANK_<rank>_<n>, n by the victim's FactionTemplate
        // group: 0 for the Horde's (4), 1 for the Alliance's (2), none else.
        // The DK form needs an honour below 1, which never gets this far.
        if (static_cast<int32_t>(honor) > 0 && static_cast<int32_t>(rank) >= 0) {
            if (const auto victim = owner_.getEntityManager().getEntity(victimGuid)) {
                std::string title;
                const uint16_t ft = fieldIndex(UF::UNIT_FIELD_FACTIONTEMPLATE);
                auto* am = owner_.services().assetManager;
                auto templates = am && am->isInitialized() ? am->loadDBCOptional("FactionTemplate.dbc") : nullptr;
                const int32_t row = templates && templates->isLoaded() && ft != 0xFFFF
                    ? templates->findRecordById(victim->getField(ft)) : -1;
                if (row >= 0) {
                    const uint32_t group = templates->getUInt32(static_cast<uint32_t>(row), 3);
                    const int side = (group & 4u) != 0 ? 0 : ((group & 2u) != 0 ? 1 : -1);
                    if (side >= 0) {
                        const std::string key = "PVP_RANK_" + std::to_string(rank) + "_" + std::to_string(side);
                        title = interfaceString(key.c_str(), nullptr);
                    }
                }
                std::string hk = interfaceString("HK", nullptr);
                if (hk.empty()) hk = "HK";
                // The victim's rank goes with it, for its badge (0x007155d0
                // hands 0x007e6030 the rank; 0x007e7390 draws it).
                addWorldText(victimGuid, static_cast<uint8_t>(FloaterKind::Honor), hk + ": " + title, false,
                             static_cast<uint32_t>(rank));
            }
        }
        if (owner_.pvpHonorCallbackRef()) owner_.pvpHonorCallbackRef()(honor, victimGuid, rank);
    }
}

void CombatHandler::handleProcResist(network::Packet& packet) {
    // 0x00750d40: full guids, the spell and a debug byte.
    if (isActiveExpansion("wotlk")) {
        SpellLogGuidsData d;
        if (SpellLogGuidsParser::parse(packet, d, true)) {
            owner_.combatLogFeed().procResist(d.casterGuid, d.victimGuid, d.spellId);
        }
        packet.skipAll();
        return;
    }
    const bool prUsesFullGuid = isActiveExpansion("tbc");
    auto readPrGuid = [&]() -> uint64_t {
        if (prUsesFullGuid)
            return (packet.hasRemaining(8)) ? packet.readUInt64() : 0;
        return packet.readPackedGuid();
    };
    if (!packet.hasRemaining(prUsesFullGuid ? 8u : 1u)             || (!prUsesFullGuid && !packet.hasFullPackedGuid())) { packet.skipAll(); return; }
    uint64_t caster = readPrGuid();
    if (!packet.hasRemaining(prUsesFullGuid ? 8u : 1u)             || (!prUsesFullGuid && !packet.hasFullPackedGuid())) { packet.skipAll(); return; }
    uint64_t victim = readPrGuid();
    if (!packet.hasRemaining(4)) return;
    uint32_t spellId = packet.readUInt32();
    if (victim == owner_.getPlayerGuid())       addCombatText(CombatTextEntry::RESIST, 0, spellId, false, 0, caster, victim);
    else if (caster == owner_.getPlayerGuid())  addCombatText(CombatTextEntry::RESIST, 0, spellId, true,  0, caster, victim);
    packet.skipAll();
}

// ============================================================
// Environmental / reflect / immune / resist
// ============================================================


void CombatHandler::handleSpellDamageShield(network::Packet& packet) {
    // Classic: packed_guid victim + packed_guid caster + spellId(4) + damage(4) + schoolMask(4)
    // TBC:     uint64 victim + uint64 caster + spellId(4) + damage(4) + schoolMask(4)
    // WotLK:   0x006d3750 - uint64 victim + uint64 caster + spellId(4) + damage(4) + overkill(4) + schoolMask(4)
    if (isActiveExpansion("wotlk")) {
        SpellDamageShieldData d;
        if (!SpellDamageShieldParser::parse(packet, d)) {
            packet.skipAll();
            return;
        }
        // 0x00752c90: the first guid deals it, the second takes it.
        owner_.combatLogFeed().damageShield(d.victimGuid, d.casterGuid, d.spellId, static_cast<int32_t>(d.damage),
                                            static_cast<int32_t>(d.overkill), d.schoolMask);
        // UNIT_COMBAT for the one it struck (0x006d3750 -> 0x00513380).
        owner_.fireUnitCombat(d.casterGuid, "WOUND", static_cast<int32_t>(d.damage),
                              static_cast<int32_t>(d.schoolMask), 0);
        // The number over the one it struck, from the shield's owner, with
        // 0x00722340's input flag set and no spell record: a spell's line,
        // gated and coloured as one, whatever the shield's spell.
        if (combatTextOurs(d.victimGuid, d.casterGuid))
            addCombatText(CombatTextEntry::SPELL_DAMAGE, static_cast<int32_t>(d.damage), d.spellId,
                          d.victimGuid == owner_.getPlayerGuid(), 0, d.victimGuid, d.casterGuid,
                          /*spellFlag=*/true);
        return;
    }
    const bool shieldTbc = isActiveExpansion("tbc");
    const bool shieldWotlkLike = !isClassicLikeExpansion() && !shieldTbc;
    const auto shieldRem = [&]() { return packet.getRemainingSize(); };
    const size_t shieldMinSz = shieldTbc ? 24u : 2u;
    if (!packet.hasRemaining(shieldMinSz)) {
        packet.skipAll(); return;
    }
    if (!shieldTbc && (!packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t victimGuid = shieldTbc
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(shieldTbc ? 8u : 1u)             || (!shieldTbc && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t casterGuid = shieldTbc
        ? packet.readUInt64() : packet.readPackedGuid();
    const size_t shieldTailSize = shieldWotlkLike ? 16u : 12u;
    if (shieldRem() < shieldTailSize) {
        packet.skipAll(); return;
    }
    uint32_t shieldSpellId = packet.readUInt32();
    uint32_t damage        = packet.readUInt32();
    if (shieldWotlkLike)
        /*uint32_t absorbed =*/ packet.readUInt32();
    /*uint32_t school =*/  packet.readUInt32();
    // Show combat text: damage shield reflect
    if (casterGuid == owner_.getPlayerGuid()) {
        // We have a damage shield that reflected damage
        addCombatText(CombatTextEntry::SPELL_DAMAGE, static_cast<int32_t>(damage), shieldSpellId, true, 0, casterGuid, victimGuid);
    } else if (victimGuid == owner_.getPlayerGuid()) {
        // A damage shield hit us (e.g. target's Thorns)
        addCombatText(CombatTextEntry::SPELL_DAMAGE, static_cast<int32_t>(damage), shieldSpellId, false, 0, casterGuid, victimGuid);
    }
}

void CombatHandler::handleSpellOrDamageImmune(network::Packet& packet) {
    // Classic/Turtle: packed casterGuid + packed victimGuid + uint32 spellId + uint8 saveType
    // TBC:            full uint64 casterGuid + full uint64 victimGuid + uint32 + uint8
    // WotLK:          as TBC (0x006ce3d0).
    if (isActiveExpansion("wotlk")) {
        SpellLogGuidsData d;
        const bool parsed = SpellLogGuidsParser::parse(packet, d, true);
        // 0x006ce3d0: an IMMUNE miss - a periodic one under the byte, a
        // damage shield's otherwise.
        if (parsed)
            owner_.combatLogFeed().spellMissed(d.casterGuid, d.victimGuid, d.spellId, combat_log::miss::kImmune,
                                               d.debug == 0, d.debug != 0);
        packet.skipAll();
        return;
    }
    const bool immuneUsesFullGuid = isActiveExpansion("tbc");
    const size_t minSz = immuneUsesFullGuid ? 21u : 2u;
    if (!packet.hasRemaining(minSz)) {
        packet.skipAll(); return;
    }
    if (!immuneUsesFullGuid && !packet.hasFullPackedGuid()) {
        packet.skipAll(); return;
    }
    uint64_t casterGuid = immuneUsesFullGuid
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(immuneUsesFullGuid ? 8u : 2u)             || (!immuneUsesFullGuid && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t victimGuid = immuneUsesFullGuid
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(5)) return;
    uint32_t immuneSpellId = packet.readUInt32();
    /*uint8_t saveType =*/ packet.readUInt8();
    // Show IMMUNE text when the player is the caster (we hit an immune target)
    // or the victim (we are immune)
    if (casterGuid == owner_.getPlayerGuid() || victimGuid == owner_.getPlayerGuid()) {
        addCombatText(CombatTextEntry::IMMUNE, 0, immuneSpellId,
                      casterGuid == owner_.getPlayerGuid(), 0, casterGuid, victimGuid);
    }
}

void CombatHandler::handleResistLog(network::Packet& packet) {
    // 3.3.5 registers nothing for SMSG_RESISTLOG (0x1d6 is not among
    // 0x006b0b80's opcodes), so the packet says nothing there.
    if (isActiveExpansion("wotlk")) {
        packet.skipAll();
        return;
    }
    // WotLK/Classic/Turtle: uint32 hitInfo + packed_guid attacker + packed_guid victim + uint32 spellId
    //                      + float resistFactor + uint32 targetRes + uint32 resistedValue + ...
    // TBC:                 same layout but full uint64 GUIDs
    // Show RESIST combat text when player resists an incoming spell.
    const bool rlUsesFullGuid = isActiveExpansion("tbc");
    auto rl_rem = [&]() { return packet.getRemainingSize(); };
    if (rl_rem() < 4) { packet.skipAll(); return; }
    /*uint32_t hitInfo =*/ packet.readUInt32();
    if (rl_rem() < (rlUsesFullGuid ? 8u : 1u)
        || (!rlUsesFullGuid && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t attackerGuid = rlUsesFullGuid
        ? packet.readUInt64() : packet.readPackedGuid();
    if (rl_rem() < (rlUsesFullGuid ? 8u : 1u)
        || (!rlUsesFullGuid && !packet.hasFullPackedGuid())) {
        packet.skipAll(); return;
    }
    uint64_t victimGuid = rlUsesFullGuid
        ? packet.readUInt64() : packet.readPackedGuid();
    if (rl_rem() < 4) { packet.skipAll(); return; }
    uint32_t spellId = packet.readUInt32();
    // Resist payload includes:
    // float resistFactor + uint32 targetResistance + uint32 resistedValue.
    // Require the full payload so truncated packets cannot synthesize
    // zero-value resist events.
    if (rl_rem() < 12) { packet.skipAll(); return; }
    /*float resistFactor =*/ packet.readFloat();
    /*uint32_t targetRes =*/ packet.readUInt32();
    int32_t resistedAmount = static_cast<int32_t>(packet.readUInt32());
    // Show RESIST when the player is involved on either side.
    if (resistedAmount > 0 && victimGuid == owner_.getPlayerGuid()) {
        addCombatText(CombatTextEntry::RESIST, resistedAmount, spellId, false, 0, attackerGuid, victimGuid);
    } else if (resistedAmount > 0 && attackerGuid == owner_.getPlayerGuid()) {
        addCombatText(CombatTextEntry::RESIST, resistedAmount, spellId, true, 0, attackerGuid, victimGuid);
    }
    packet.skipAll();
}

// ============================================================
// Pet feedback
// ============================================================

void CombatHandler::handlePetTameFailure(network::Packet& packet) {
    // ERR_TAME_FAILED on screen with the PETTAME_ reason, as 0x00802090 ->
    // 0x00800390 picks it. This was English, in chat as well, and its
    // reasons were off by one against the codes.
    static constexpr const char* kReasons[][2] = {
        {"PETTAME_UNKNOWNERROR", "Unknown taming error"},
        {"PETTAME_INVALIDCREATURE", "Invalid creature"},
        {"PETTAME_TOOMANY", "You have too many pets"},
        {"PETTAME_CREATUREALREADYOWNED", "Creature already owned"},
        {"PETTAME_NOTTAMEABLE", "Creature not tameable"},
        {"PETTAME_ANOTHERSUMMONACTIVE", "You already have a summon active"},
        {"PETTAME_UNITSCANTTAME", "You cannot tame creatures"},
        {"PETTAME_NOPETAVAILABLE", "You have no pet available to summon"},
        {"PETTAME_INTERNALERROR", "Internal pet error"},
        {"PETTAME_TOOHIGHLEVEL", "Creature is too high level"},
        {"PETTAME_DEAD", "Your pet is dead"},
        {"PETTAME_NOTDEAD", "Your pet is not dead"},
        {"PETTAME_CANTCONTROLEXOTIC", "You cannot control exotic creatures"},
    };
    if (packet.hasRemaining(1)) {
        uint8_t reason = packet.readUInt8();
        if (reason >= std::size(kReasons)) reason = 0;
        owner_.addUIError(interfaceFormat("ERR_TAME_FAILED", "%s.",
                                          {interfaceString(kReasons[reason][0], kReasons[reason][1])}));
    }
}

void CombatHandler::handlePetActionFeedback(network::Packet& packet) {
    // On screen, as 0x005d4da0 words it: 1 ERR_PET_SPELL_DEAD; 2, with the
    // spell, ERR_NO_ATTACK_TARGET - or ERR_GENERIC_NO_VALID_TARGETS for a
    // spell aimed at friends; 3, with the spell, ERR_INVALID_ATTACK_TARGET; 4
    // ERR_PET_SPELL_NOPATH; nothing else. These were English chat lines.
    if (!packet.hasRemaining(1)) return;
    const uint8_t msg = packet.readUInt8();
    switch (msg) {
        case 1:
            owner_.addUIError(interfaceString("ERR_PET_SPELL_DEAD", "Your pet is dead"));
            break;
        case 2: {
            const uint32_t spellId = packet.hasRemaining(4) ? packet.readUInt32() : 0;
            const bool friendly = spellId != 0 && owner_.getSpellTargetKind(spellId) == 1;
            const char* key = friendly ? "ERR_GENERIC_NO_VALID_TARGETS" : "ERR_NO_ATTACK_TARGET";
            owner_.addUIError(friendly ? interfaceString(key, "No valid targets.")
                                       : interfaceString(key, "There is nothing to attack."));
            owner_.playGameErrorSound(key);
            // The spell's global cooldown comes off the pet's bar
            // (0x00804780 -> 0x00804720), as for a failed pet cast.
            if (auto* spells = owner_.getSpellHandler()) spells->notePetCastFailed(spellId);
            break;
        }
        case 3: {
            owner_.addUIError(interfaceString("ERR_INVALID_ATTACK_TARGET", "You cannot attack that target."));
            owner_.playGameErrorSound("ERR_INVALID_ATTACK_TARGET");
            // The spell follows the message here too, and is let go the same way.
            const uint32_t spellId = packet.hasRemaining(4) ? packet.readUInt32() : 0;
            if (auto* spells = owner_.getSpellHandler()) spells->notePetCastFailed(spellId);
            break;
        }
        case 4:
            owner_.addUIError(interfaceStringAny({"ERR_PET_SPELL_NOPATH", "PET_SPELL_NOPATH"}, "No path available"));
            break;
        default:
            break;
    }
    packet.skipAll();
}

void CombatHandler::handlePetCastFailed(network::Packet& packet) {
    // WotLK: castCount(1) + spellId(4) + reason(1)
    // Classic/TBC: spellId(4) + reason(1) (no castCount)
    const bool hasCount = isActiveExpansion("wotlk");
    const size_t minSize = hasCount ? 6u : 5u;
    if (packet.hasRemaining(minSize)) {
        const uint8_t castCount = hasCount ? packet.readUInt8() : 0;
        uint32_t spellId   = packet.readUInt32();
        // 0x00806c30: the cast of that count by the player's charm, else
        // its summon, ends (0x007fecc0).  That routine is also what raises
        // the events, and it names the pet - whose own cast bar is bound to
        // the "pet" token (petframe.lua:182, CastingBarFrame_OnLoad(self,
        // "pet", false, false)).  Nothing here told that bar anything: a pet
        // cast the realm refused left its bar running to the end of its own
        // clock, and the frames listening for a failure saw nothing at all.
        uint64_t pet = 0;
        if (auto player = owner_.getEntityManager().getEntity(owner_.getPlayerGuid())) {
            const auto guidAt = [&](UF f) -> uint64_t {
                const uint16_t i = fieldIndex(f);
                if (i == 0xFFFF) return 0;
                return (static_cast<uint64_t>(player->getField(static_cast<uint16_t>(i + 1))) << 32) |
                       player->getField(i);
            };
            pet = guidAt(UF::UNIT_FIELD_CHARM);
            if (pet == 0) pet = guidAt(UF::UNIT_FIELD_SUMMON);
            if (pet != 0 && hasCount) owner_.castVisualEnded(pet, castCount, spellId);
            if (pet != 0 && hasCount)
                if (SpellHandler* spells = owner_.getSpellHandler()) spells->castEnded(pet, castCount, spellId);
        }
        uint8_t  reason    = (packet.hasRemaining(1))
                                 ? packet.readUInt8() : 0;
        // Up to two ids after the result, read when present, as
        // SMSG_CAST_FAILED's are (0x00806c30, 0x00809af0).
        const uint32_t miscArg = packet.hasRemaining(4) ? packet.readUInt32() : 0;
        LOG_DEBUG("SMSG_PET_CAST_FAILED: spell=", spellId,
                  " reason=", static_cast<int>(reason));
        // 0x00806c30 -> 0x00804720: its global cooldown off the pet's
        // history.
        if (auto* spells = owner_.getSpellHandler()) spells->notePetCastFailed(spellId);
        // On screen and not in chat, as 0x00806c30 -> 0x00805610 says it;
        // see petCastFailureMessage. This was "Spell: reason" in English, in
        // chat.
        if (reason != 0) {
            const std::string errMsg = petCastFailureMessage(owner_, spellId, reason, miscArg);
            if (!errMsg.empty()) owner_.addUIError(errMsg);
            // And the player's line for it, where the game error has one.
            if (!errMsg.empty())
                if (auto* spells = owner_.getSpellHandler()) spells->sayPetCastFailure(spellId, reason);
        }
        // The event that same routine raises, which is the reason's choice:
        // FAILED_QUIET for the three refusals that leave a cast running,
        // INTERRUPTED for 0x28/0x29, plain FAILED for everything else - and
        // the stop after it, this being the cast that was shown.
        if (pet != 0)
            if (auto* spells = owner_.getSpellHandler()) spells->raiseCastEnd(pet, castCount, spellId, reason);
    }
    packet.skipAll();
}

void CombatHandler::handlePetBroken(network::Packet& packet) {
    // Pet bond broken (died or forcibly dismissed) - clear pet state
    owner_.setActivePet(0);
    owner_.petSpellListRef().clear();
    owner_.petAutocastSpellsRef().clear();
    owner_.petAutocastableSpellsRef().clear();
    memset(owner_.petActionSlotsRef(), 0, sizeof(owner_.petActionSlotsRef()));
    // ERR_PET_BROKEN on screen, which is all 0x005d3120 does. This was
    // "Your pet has died." in English, in chat.
    owner_.addUIError(interfaceString("ERR_PET_BROKEN", "Your pet has run away"));
    // The same pair SMSG_PET_SPELLS fires, because the same two things have to
    // be redrawn - the pet frame and the pet action bar. Losing the pet
    // outright announced nothing while *learning one spell* announced
    // PET_BAR_UPDATE, so a dead pet's frame and its whole ability bar stayed on
    // screen until the next login.
    owner_.fireAddonEvent("UNIT_PET", {"player"});
    owner_.fireAddonEvent("PET_UI_UPDATE", {});
    LOG_INFO("SMSG_PET_BROKEN: pet bond broken");
    packet.skipAll();
}

void CombatHandler::handlePetLearnedSpell(network::Packet& packet) {
    if (packet.hasRemaining(4)) {
        uint32_t spellId = packet.readUInt32();
        owner_.petSpellListRef().push_back(spellId);
        // ERR_PET_LEARN_ABILITY_S (Attributes 0x10) or ERR_PET_LEARN_SPELL_S
        // with "Name (Rank)", in chat (0x005d4c30); nothing for a spell not
        // in Spell.dbc. This was English.
        if (const auto attrs = owner_.getSpellAttributes(spellId);
            attrs && !owner_.getSpellName(spellId).empty()) {
            const bool ability = (*attrs & 0x10u) != 0;
            owner_.addSystemChatMessage(interfaceFormat(
                ability ? "ERR_PET_LEARN_ABILITY_S" : "ERR_PET_LEARN_SPELL_S",
                ability ? "Your pet has learned a new ability: %s." : "Your pet has learned a new spell: %s.",
                {petSpellNameWithRank(owner_, spellId)}));
        }
        LOG_DEBUG("SMSG_PET_LEARNED_SPELL: spellId=", spellId);
        owner_.fireAddonEvent("PET_BAR_UPDATE", {});
    }
    packet.skipAll();
}

void CombatHandler::handlePetUnlearnedSpell(network::Packet& packet) {
    if (packet.hasRemaining(4)) {
        uint32_t spellId = packet.readUInt32();
        owner_.petSpellListRef().erase(
            std::remove(owner_.petSpellListRef().begin(), owner_.petSpellListRef().end(), spellId),
            owner_.petSpellListRef().end());
        owner_.petAutocastSpellsRef().erase(spellId);
        owner_.petAutocastableSpellsRef().erase(spellId);
        // ERR_PET_SPELL_UNLEARNED_S in chat (0x005d4c30), which was missing.
        if (owner_.getSpellAttributes(spellId) && !owner_.getSpellName(spellId).empty()) {
            owner_.addSystemChatMessage(interfaceFormat("ERR_PET_SPELL_UNLEARNED_S",
                "Your pet has unlearned %s.", {petSpellNameWithRank(owner_, spellId)}));
        }
        LOG_DEBUG("SMSG_PET_UNLEARNED_SPELL: spellId=", spellId);
    }
    packet.skipAll();
}

void CombatHandler::handlePetMode(network::Packet& packet) {
    // 0x005d3630: the pet's guid and its mode word, kept only for the pet the
    // player has, then PET_BAR_UPDATE. The word is SMSG_PET_SPELLS's: the
    // react state in the low byte and the command above it (0x005a7980 and
    // the ACT_COMMAND test beside it) - read the other way round, a change of
    // stance lit the wrong button. Attacking is the pet's combat flag
    // (UNIT_FLAG_PET_IN_COMBAT), not this.
    if (packet.hasRemaining(12)) {
        uint64_t modeGuid = packet.readUInt64();
        uint32_t mode     = packet.readUInt32();
        if (modeGuid == owner_.petGuidRef()) {
            owner_.petReactRef()   = static_cast<uint8_t>(mode & 0xFF);
            owner_.petCommandRef() = static_cast<uint8_t>((mode >> 8) & 0xFF);
            owner_.fireAddonEvent("PET_BAR_UPDATE", {});
            LOG_DEBUG("SMSG_PET_MODE: command=", static_cast<int>(owner_.petCommandRef()),
                      " react=", static_cast<int>(owner_.petReactRef()));
        }
    }
    packet.skipAll();
}

// ============================================================
// Resurrect
// ============================================================

void CombatHandler::handleResurrectFailed(network::Packet& packet) {
    // Nothing said: 0x006ce470 -> 0x00520f70 only raises an event. These
    // were English lines, on screen and in chat.
    if (packet.hasRemaining(4)) packet.readUInt32();
}

// ============================================================
// Targeting
// ============================================================

bool CombatHandler::isSelectableUnit(uint64_t guid) const {
    // A game object is not a target, whoever asks.
    //
    // WoW selects units, players and corpses; a door, a mailbox, a chest or a
    // spell's ground effect is used or walked into, and none of them can be
    // the target. Refused here rather than at the click, because every way in
    // reaches this - the click, tab, /target, a macro - and each one that
    // targeted an object put a selection circle on the floor around it and
    // sent CMSG_SET_SELECTION for a guid that is not a unit.
    if (auto entity = owner_.getEntityManager().getEntity(guid)) {
        const auto type = entity->getType();
        if (type == ObjectType::GAMEOBJECT || type == ObjectType::DYNAMICOBJECT) {
            return false;
        }
    }

    // Everything the player clicks is selectable; the server arbitrates whether a
    // corpse actually holds loot when we send CMSG_LOOT. We deliberately do NOT
    // gate dead creature corpses on UNIT_DYNFLAG_LOOTABLE.
    //
    // The reason given here was that the bit is computed per-viewer and goes
    // stale across a death/resurrect cycle, and that gating on it left a
    // genuinely lootable corpse permanently unclickable. The symptom was real;
    // the explanation was not the whole of it. UNIT_DYNAMIC_FLAGS was being
    // read from field 147 - which is UNIT_FIELD_PADDING - where the server
    // writes 79, so the bit was never right in the first place. Fixed
    // 2026-08-05.
    //
    // Not re-gated here, because that would be trading a working behaviour for
    // a theory. Anyone who wants the gate back should first confirm against a
    // live corpse that the flag now reads what it should.
    // A looted corpse stays a target until the server removes it, as in the client.
    return true;
}

bool CombatHandler::targetUnit(uint64_t guid) {
    if (guid != 0 && owner_.isUnitTargeting() && owner_.getEntityManager().getEntity(guid)) {
        owner_.spellTargetUnit(guid);
        return false;
    }
    setTarget(guid);
    return true;
}

void CombatHandler::setTarget(uint64_t guid) {
    if (guid == owner_.getTargetGuid()) return;
    // Looted-out, unskinnable corpses cannot be selected.
    if (!isSelectableUnit(guid)) return;

    // 0x00524bf0: the last target, which TargetLastTarget flips back to, is
    // the one left - none included.
    owner_.lastTargetGuidRef() =
        unit_target::lastTargetAfter(owner_.getTargetGuid(), guid, owner_.lastTargetGuidRef());
    // 0x00524bf0: the unit now taken, one in view, is remembered as the last
    // enemy where the player may attack it now (0x00729a70), else as the last
    // friend where it may help it (0x007293d0) - the target chosen, not the
    // one left, so TargetLastEnemy and TargetLastFriend (0x00525df0,
    // 0x00525e50) go back to the last of each the player picked.
    const uint64_t me = owner_.getPlayerGuid();
    auto player = std::dynamic_pointer_cast<Unit>(owner_.getEntityManager().getEntity(me));
    if (guid != 0 && player) {
        auto entity = owner_.getEntityManager().getEntity(guid);
        if (auto* unit = dynamic_cast<Unit*>(entity.get())) {
            const bool wouldDismount = unit_target::wouldDismount(
                player->getUnitFlags(), owner_.isPlayerFlying(),
                addons::storedCVarValue("autoDismount", "1") != "0",
                addons::storedCVarValue("autoDismountFlying", "0") != "0");
            if (unit_target::mayAttackNow(owner_.isPlayerDead(), owner_.isMounted(), wouldDismount,
                                          unit->getHealth(), unit->getDynamicFlags(),
                                          owner_.unitMayAttack(me, guid))) {
                lastEnemyTargetGuid_ = guid;
            } else if (owner_.unitMayAssist(me, guid)) {
                lastFriendTargetGuid_ = guid;
            }
            // Then the tutorials the choice opens (0x00524bf0): another
            // player, the first time; else a player of the player's side at
            // level 5 and up (0x00729b30); else one that may be attacked
            // (0x00729740); else a flight master; else a quest giver with
            // nothing yet.
            const bool isPlayer = entity->getType() == ObjectType::PLAYER;
            if (isPlayer && guid != me && owner_.tutorials().awaiting(0x12)) {
                owner_.triggerTutorial(0x12);
            } else if (isPlayer && owner_.unitsCooperate(me, guid) && owner_.getPlayerLevel() >= 5) {
                owner_.triggerTutorial(0x11);
            } else if (owner_.unitMayAttack(me, guid)) {
                owner_.triggerTutorial(4);
            } else if ((unit->getNpcFlags() & 0x2000u) != 0) {
                owner_.triggerTutorial(0x22);
            } else if (owner_.getQuestGiverStatus(guid) == QuestGiverStatus::UNAVAILABLE) {
                owner_.triggerTutorial(0x2a);
            }
        }
    }

    // 0x00524bf0: the old target let go ends the attack on it (0x005241b0 ->
    // 0x006e1660), and the new one is attacked in its stead, below - unless
    // Stop Auto Attack On Target Change is on, which stops a repeating shot
    // too, and tells the server (0x00807560(1)).
    const bool wasAttacking = attack_.attacking();
    if (wasAttacking && owner_.getTargetGuid() != 0) stopAutoAttack();
    const bool stopsOnChange = addons::storedCVarValue("stopAutoAttackOnTargetChange", "0") != "0";
    if (stopsOnChange) owner_.stopAutoRepeat(true);

    owner_.setTargetGuidRaw(guid);

    // Clear stale aura data from the previous target so the buff bar shows
    // an empty state until the server sends SMSG_AURA_UPDATE_ALL for the new target.
    if (owner_.getSpellHandler()) owner_.getSpellHandler()->clearTargetAuras();

    // Clear previous target's cast bar on target change
    // (the new target's cast state is naturally fetched from spellHandler_->unitCastStates_ by GUID)
    // 0x00524bf0: the new target's cast, else its channel, is judged for
    // whether the player can interrupt it (0x007262e0).
    if (owner_.getSpellHandler()) owner_.getSpellHandler()->reevaluateTargetCastLock(guid);

    // Inform server of target selection
    if (owner_.isInWorld()) {
        auto packet = SetSelectionPacket::build(guid);
        owner_.getSocket()->send(packet);
    }

    if (guid != 0) {
        LOG_INFO("Target set: 0x", std::hex, guid, std::dec);
    }
    owner_.fireAddonEvent("PLAYER_TARGET_CHANGED", {});
    // 0x006e4950 at the new target.
    if (wasAttacking && !stopsOnChange && guid != 0) startAutoAttack(guid);

    // Report the interface once, the first time there is a target to report it
    // against. A target frame is only right or wrong with something targeted,
    // and that is not a moment anything can be scheduled for - which is why
    // this one has been diagnosed from readings taken when nothing was
    // selected, where correct and broken look identical.
    static bool reportedOnce = false;
    if (!reportedOnce && guid != 0) {
        reportedOnce = true;
        ui::frameXmlRequestCheck();
        // What UnitExists("target") resolves through, said plainly. The Lua
        // probe was meant to answer this and has not run; this is the same
        // question asked where it can actually be answered - the interface
        // reports "target" as existing exactly when this guid resolves to a
        // unit.
        auto entity = owner_.getEntityManager().getEntity(guid);
        LOG_WARNING("first target: guid=0x", std::hex, guid, std::dec,
                    " entity=", (entity ? "found" : "MISSING"),
                    " unit=", (entity && dynamic_cast<Unit*>(entity.get())
                                   ? "yes" : "NO"));
    }
}

void CombatHandler::clearTarget() {
    // 0x005241b0 -> 0x006e1660: nothing targeted is nothing attacked.
    if (owner_.getTargetGuid() != 0) stopAutoAttack();
    // 0x00524bf0 with none: the target let go is the last target.
    owner_.lastTargetGuidRef() = unit_target::lastTargetAfter(owner_.getTargetGuid(), 0, owner_.lastTargetGuidRef());
    if (owner_.getTargetGuid() != 0) {
        LOG_INFO("Target cleared");
        // Zero the GUID before firing the event so callbacks/addons that query
        // the current target see null (consistent with setTarget which updates
        // targetGuid before the event).
        owner_.setTargetGuidRaw(0);
        // 0x005241b0(target, 1): the server told there is no selection any
        // more (CMSG_SET_SELECTION 0x13d with 0). Without it the server kept
        // the old one, and anything aimed at "the selected unit" - a GM
        // command among them - still went at a creature long let go.
        if (owner_.isInWorld() && owner_.getSocket()) owner_.getSocket()->send(SetSelectionPacket::build(0));
        owner_.fireAddonEvent("PLAYER_TARGET_CHANGED", {});
        // A repeating shot has nothing left to shoot at (0x00524bf0 with
        // none -> 0x00807560(1)).
        owner_.stopAutoRepeat(true);
    } else {
        owner_.setTargetGuidRaw(0);
    }
    owner_.tabCycleIndexRef() = -1;
    owner_.tabCycleStaleRef() = true;
}

void CombatHandler::forgetUnit(uint64_t guid) {
    if (guid == 0) return;
    // 0x00524350 first: the target forgotten ends a repeating shot, the
    // server told (0x00807560(1)).
    if (guid == owner_.getTargetGuid()) owner_.stopAutoRepeat(true);
    if (guid == owner_.getTargetGuid()) {
        // 0x005241b0: let go, the server told (CMSG_SET_SELECTION) and
        // PLAYER_TARGET_CHANGED.
        owner_.setTargetGuidRaw(0);
        if (owner_.isInWorld() && owner_.getSocket()) owner_.getSocket()->send(SetSelectionPacket::build(0));
        owner_.fireAddonEvent("PLAYER_TARGET_CHANGED", {});
    }
    if (owner_.lastTargetGuidRef() == guid) owner_.lastTargetGuidRef() = 0;
    if (lastEnemyTargetGuid_ == guid) lastEnemyTargetGuid_ = 0;
    if (lastFriendTargetGuid_ == guid) lastFriendTargetGuid_ = 0;
    if (owner_.focusGuidRef() == guid) {
        // 0x0051ff20 with none: PLAYER_FOCUS_CHANGED.
        owner_.focusGuidRef() = 0;
        owner_.fireAddonEvent("PLAYER_FOCUS_CHANGED", {});
    }
}

void CombatHandler::unitLeavesView(uint64_t guid) {
    const bool group = isGroupMemberGuid(guid);
    bool duel = false;
    if (group) {
        auto& em = owner_.getEntityManager();
        const auto me = em.getEntity(owner_.getPlayerGuid());
        const auto it = em.getEntity(guid);
        const auto field = [](const Entity& e, UF f) {
            const uint16_t i = fieldIndex(f);
            return i != 0xFFFF ? e.getField(i) : 0u;
        };
        const auto guidField = [&](const Entity& e, UF f) {
            const uint16_t i = fieldIndex(f);
            return i != 0xFFFF ? e.getField(i) | (static_cast<uint64_t>(e.getField(static_cast<uint16_t>(i + 1))) << 32)
                               : 0ull;
        };
        if (me && it && it->getType() == ObjectType::PLAYER) {
            duel = unit_target::duelOpponents(guidField(*me, UF::PLAYER_DUEL_ARBITER), field(*me, UF::PLAYER_DUEL_TEAM),
                                              guidField(*it, UF::PLAYER_DUEL_ARBITER), field(*it, UF::PLAYER_DUEL_TEAM));
        }
    }
    if (unit_target::forgottenOnLeaving(group, duel)) forgetUnit(guid);
}

void CombatHandler::leaveWorld() {
    const uint64_t me = owner_.getPlayerGuid();
    const auto kept = [&](uint64_t guid) { return unit_target::keptOnLeavingWorld(guid == me, isGroupMemberGuid(guid)); };
    // 0x00528c30: the target goes as a unit leaving does (0x005241b0 - the
    // server told, PLAYER_TARGET_CHANGED), and the focus with none
    // (0x0051ff20, PLAYER_FOCUS_CHANGED).
    if (const uint64_t target = owner_.getTargetGuid(); target != 0 && !kept(target)) forgetUnit(target);
    if (const uint64_t focus = owner_.focusGuidRef(); focus != 0 && !kept(focus)) forgetUnit(focus);
    owner_.lastTargetGuidRef() = 0;  // 0xbd07b8
    lastEnemyTargetGuid_ = 0;        // 0xbd07c0
    lastFriendTargetGuid_ = 0;       // 0xbd07c8
}

std::shared_ptr<Entity> CombatHandler::getTarget() const {
    if (owner_.getTargetGuid() == 0) return nullptr;
    return owner_.getEntityManager().getEntity(owner_.getTargetGuid());
}

void CombatHandler::setFocus(uint64_t guid) {
    owner_.focusGuidRef() = guid;
    owner_.fireAddonEvent("PLAYER_FOCUS_CHANGED", {});
    if (guid != 0) {
        auto entity = owner_.getEntityManager().getEntity(guid);
        if (entity) {
            std::string name;
            auto unit = std::dynamic_pointer_cast<Unit>(entity);
            if (unit && !unit->getName().empty()) {
                name = unit->getName();
            }
            if (name.empty()) name = owner_.lookupName(guid);
            // No line: FocusUnit (0x00522480) says nothing. This printed
            // "Focus set: X" in English.
            LOG_INFO("Focus set: 0x", std::hex, guid, std::dec, " (", name, ")");
        }
    }
}

void CombatHandler::clearFocus() {
    // No line: ClearFocus (0x005225e0) says nothing.
    if (owner_.focusGuidRef() != 0) {
        LOG_INFO("Focus cleared");
    }
    owner_.focusGuidRef() = 0;
    owner_.fireAddonEvent("PLAYER_FOCUS_CHANGED", {});
}

std::shared_ptr<Entity> CombatHandler::getFocus() const {
    if (owner_.focusGuidRef() == 0) return nullptr;
    return owner_.getEntityManager().getEntity(owner_.focusGuidRef());
}

void CombatHandler::setMouseoverGuid(uint64_t guid) {
    if (owner_.mouseoverGuidRef() != guid) {
        owner_.mouseoverGuidRef() = guid;
        owner_.fireAddonEvent("UPDATE_MOUSEOVER_UNIT", {});
    }
}

void CombatHandler::targetLastTarget() {
    // Silent with none, as TargetLastTarget (0x00525d70) is.
    if (owner_.lastTargetGuidRef() == 0) {
        return;
    }

    // Swap current and last target - unless a spell waiting for a unit
    // takes it (TargetLastTarget, 0x00525d70 -> 0x00524bf0).
    uint64_t temp = owner_.getTargetGuid();
    if (!targetUnit(owner_.lastTargetGuidRef())) return;
    owner_.lastTargetGuidRef() = temp;
}

void CombatHandler::cycleTarget(bool reverse, const char* noneMessage,
                                const std::function<bool(uint64_t, Entity&)>& wanted) {
    constexpr float kRangeSq = 40.0f * 40.0f;

    float px = 0.0f, py = 0.0f, pz = 0.0f;
    if (auto self = owner_.getEntityManager().getEntity(owner_.getPlayerGuid())) {
        px = self->getX(); py = self->getY(); pz = self->getZ();
    }

    struct Cand { uint64_t guid; float distSq; };
    std::vector<Cand> cands;
    for (const auto& [guid, entity] : owner_.getEntityManager().getEntities()) {
        if (!entity || guid == owner_.getPlayerGuid()) continue;
        const float dx = entity->getX() - px;
        const float dy = entity->getY() - py;
        const float dz = entity->getZ() - pz;
        const float distSq = dx * dx + dy * dy + dz * dz;
        if (distSq > kRangeSq) continue;
        if (!wanted(guid, *entity)) continue;
        cands.push_back({.guid = guid, .distSq = distSq});
    }
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.distSq < b.distSq; });

    // Silent with none, as TargetNearestEnemy and its kin (0x00525ad0) are.
    (void)noneMessage;
    if (cands.empty()) {
        return;
    }

    std::vector<uint64_t> order;
    order.reserve(cands.size());
    for (const auto& c : cands) order.push_back(c.guid);

    // Not on the list: start at the near end, or the far one going backwards.
    // The TargetNearest family (0x00524fc0) ends in 0x00524bf0.
    auto it = std::find(order.begin(), order.end(), owner_.getTargetGuid());
    if (it == order.end()) {
        targetUnit(reverse ? order.back() : order.front());
        return;
    }
    if (reverse) {
        targetUnit(it == order.begin() ? order.back() : *(it - 1));
    } else {
        auto next = it + 1;
        targetUnit(next == order.end() ? order.front() : *next);
    }
}

bool CombatHandler::isGroupMemberGuid(uint64_t guid) const {
    for (const auto& m : owner_.getPartyData().members) {
        if (m.guid == guid) return true;
    }
    return false;
}

void CombatHandler::targetEnemy(bool reverse) {
    // Corpses are never candidates - that is what mouse looting is for.
    cycleTarget(reverse, "No enemies in range.", [this](uint64_t guid, Entity& e) {
        const auto t = e.getType();
        if (t != ObjectType::UNIT && t != ObjectType::PLAYER) return false;
        auto* unit = dynamic_cast<Unit*>(&e);
        if (!unit) return false;
        // 0x00518e40: one the player may attack (0x00729740), alive, not
        // flagged dead, not lying dead (nearest_enemy_filter.hpp).
        return nearestEnemyCandidate(owner_.unitMayAttack(owner_.getPlayerGuid(), guid),
                                     unit->getHealth(), unit->getDynamicFlags(), unit->getStandState());
    });
}

void CombatHandler::targetNearest(bool reverse) {
    cycleTarget(reverse, "", [](uint64_t, Entity& e) {
        const auto t = e.getType();
        return t == ObjectType::UNIT || t == ObjectType::PLAYER;
    });
}

void CombatHandler::targetFriend(bool reverse) {
    // Dead friends stay targetable - you need to select them to resurrect them.
    cycleTarget(reverse, "No friendly targets in range.", [](uint64_t, Entity& e) {
        return e.getType() == ObjectType::PLAYER;
    });
}

void CombatHandler::targetNearestEnemyPlayer(bool reverse) {
    cycleTarget(reverse, "No enemy players in range.", [this](uint64_t guid, Entity& e) {
        if (e.getType() != ObjectType::PLAYER) return false;
        auto* unit = dynamic_cast<Unit*>(&e);
        if (!unit) return false;
        // 0x00518e40: one the player may attack (0x00729740), alive, not
        // flagged dead, not lying dead (nearest_enemy_filter.hpp).
        return nearestEnemyCandidate(owner_.unitMayAttack(owner_.getPlayerGuid(), guid),
                                     unit->getHealth(), unit->getDynamicFlags(), unit->getStandState());
    });
}

void CombatHandler::targetNearestFriendPlayer(bool reverse) {
    cycleTarget(reverse, "No friendly players in range.", [this](uint64_t guid, Entity& e) {
        if (e.getType() != ObjectType::PLAYER) return false;
        auto* unit = dynamic_cast<Unit*>(&e);
        // 0x00518e40: one the player may help (0x007293d0), alive.
        return unit && unit->getHealth() > 0 && owner_.unitMayAssist(owner_.getPlayerGuid(), guid);
    });
}

void CombatHandler::targetNearestPartyMember(bool reverse) {
    cycleTarget(reverse, "No party members in range.", [this](uint64_t guid, Entity&) {
        return isGroupMemberGuid(guid);
    });
}

void CombatHandler::targetNearestRaidMember(bool reverse) {
    // The same roster: a raid is what the group list calls itself when it has
    // grown past five, and the members arrive in the same list either way.
    cycleTarget(reverse, "No raid members in range.", [this](uint64_t guid, Entity&) {
        return isGroupMemberGuid(guid);
    });
}

bool CombatHandler::rememberedTargetReachable(uint64_t guid) const {
    // 0x00525df0 / 0x00525e50: a unit in view (0x004d4db0), else the player,
    // its pet or one of its group (0x0052d310, 0x00573200). Silent otherwise.
    if (guid == 0) return false;
    if (auto e = owner_.getEntityManager().getEntity(guid); e && e->isUnit()) return true;
    return guid == owner_.getPlayerGuid() || guid == owner_.getPetGuid() || isGroupMemberGuid(guid);
}

void CombatHandler::targetLastEnemy() {
    if (!rememberedTargetReachable(lastEnemyTargetGuid_)) return;
    targetUnit(lastEnemyTargetGuid_);  // 0x00525df0 -> 0x00524bf0
}

void CombatHandler::targetLastFriend() {
    if (!rememberedTargetReachable(lastFriendTargetGuid_)) return;
    targetUnit(lastFriendTargetGuid_);  // 0x00525e50 -> 0x00524bf0
}

void CombatHandler::tabTarget(float playerX, float playerY, float playerZ) {
    // Maximum tab-target range in yards (matches the WoW client).
    constexpr float kTabRange   = 40.0f;
    constexpr float kTabRangeSq = kTabRange * kTabRange;
    // A pause this long between tab presses restarts the cycle from the
    // nearest enemy instead of continuing a stale rotation.
    constexpr uint64_t kCycleResetMs = 4000;

    const bool playerInCombat = owner_.isInCombat();

    // Helper: returns true if the entity is a hostile that can be tab-targeted.
    // Corpses are never tab targets while fighting; out of combat they only
    // qualify when they still hold loot.
    auto isValidTabTarget = [&](const std::shared_ptr<Entity>& e) -> bool {
        if (!e) return false;
        const uint64_t guid = e->getGuid();
        auto* unit = dynamic_cast<Unit*>(e.get());
        if (!unit) return false;
        if (unit->getHealth() == 0) {
            if (playerInCombat) return false;
            auto lootIt = owner_.localLootStateRef().find(guid);
            return lootIt != owner_.localLootStateRef().end() &&
                   !lootIt->second.data.items.empty();
        }
        // 0x00518e40 (nearest_enemy_filter.hpp).
        return nearestEnemyCandidate(owner_.unitMayAttack(owner_.getPlayerGuid(), guid),
                                     unit->getHealth(), unit->getDynamicFlags(), unit->getStandState());
    };

    // Restart the cycle after a pause - the player has moved on; the next tab
    // should grab the nearest enemy, not resume an old rotation.
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (nowMs - lastTabTargetMs_ > kCycleResetMs)
        owner_.tabCycleStaleRef() = true;
    lastTabTargetMs_ = nowMs;

    // Rebuild cycle list if stale (entity added/removed since last tab press).
    if (owner_.tabCycleStaleRef()) {
        owner_.tabCycleListRef().clear();
        owner_.tabCycleIndexRef() = -1;

        struct EntityDist { uint64_t guid; float distSq; bool dead; };
        std::vector<EntityDist> sortable;
        const auto& entities = owner_.getEntityManager().getEntities();
        sortable.reserve(entities.size());

        for (const auto& [guid, entity] : entities) {
            auto t = entity->getType();
            if (t != ObjectType::UNIT && t != ObjectType::PLAYER) continue;
            if (guid == owner_.getPlayerGuid()) continue;
            if (!isValidTabTarget(entity)) continue;
            float dx = entity->getX() - playerX;
            float dy = entity->getY() - playerY;
            float dz = entity->getZ() - playerZ;
            // Sort by squared distance - monotonic with distance, skips sqrt per entity.
            float distSq = dx*dx + dy*dy + dz*dz;
            if (distSq > kTabRangeSq) continue;
            auto* unit = static_cast<Unit*>(entity.get());
            sortable.push_back({.guid = guid, .distSq = distSq, .dead = unit->getHealth() == 0});
        }

        // Live enemies cycle first (nearest to farthest); lootable corpses last.
        std::sort(sortable.begin(), sortable.end(),
                  [](const EntityDist& a, const EntityDist& b) {
                      if (a.dead != b.dead) return b.dead;
                      return a.distSq < b.distSq;
                  });

        owner_.tabCycleListRef().reserve(sortable.size());
        for (const auto& ed : sortable) {
            owner_.tabCycleListRef().push_back(ed.guid);
        }
        owner_.tabCycleStaleRef() = false;
    }

    if (owner_.tabCycleListRef().empty()) {
        clearTarget();
        return;
    }

    // If the current target was picked by other means (click, assist), resume
    // the cycle from its position so tab advances to the NEXT enemy instead of
    // jumping back to wherever the old rotation left off.
    {
        const uint64_t curTarget = owner_.getTargetGuid();
        if (curTarget != 0) {
            const auto& list = owner_.tabCycleListRef();
            for (size_t i = 0; i < list.size(); ++i) {
                if (list[i] == curTarget) {
                    owner_.tabCycleIndexRef() = static_cast<int>(i);
                    break;
                }
            }
        }
    }

    // Advance through the cycle, skipping any entry that has since died or
    // turned friendly (e.g. NPC killed between two tab presses).
    int tries = static_cast<int>(owner_.tabCycleListRef().size());
    while (tries-- > 0) {
        owner_.tabCycleIndexRef() = (owner_.tabCycleIndexRef() + 1) % static_cast<int>(owner_.tabCycleListRef().size());
        uint64_t guid = owner_.tabCycleListRef()[owner_.tabCycleIndexRef()];
        auto entity = owner_.getEntityManager().getEntity(guid);
        if (isValidTabTarget(entity)) {
            targetUnit(guid);  // TargetNearestEnemy, 0x00525ad0 -> 0x00524fc0
            return;
        }
    }

    // All cached entries are stale - clear target and force a fresh rebuild next time.
    owner_.tabCycleStaleRef() = true;
    clearTarget();
}

void CombatHandler::assistTarget() {
    if (owner_.getState() != WorldState::IN_WORLD) {
        LOG_WARNING("Cannot assist: not in world");
        return;
    }

    // ERR_GENERIC_NO_TARGET with nobody to assist, as AssistUnit()
    // (0x00525eb0) says it for /assist on its own. These were English.
    if (owner_.getTargetGuid() == 0) {
        owner_.raiseUiError(interfaceString("ERR_GENERIC_NO_TARGET", "You have no target."));
        owner_.playGameErrorSound("ERR_GENERIC_NO_TARGET");
        return;
    }

    auto target = getTarget();
    if (!target) {
        owner_.raiseUiError(interfaceString("ERR_GENERIC_NO_TARGET", "You have no target."));
        owner_.playGameErrorSound("ERR_GENERIC_NO_TARGET");
        return;
    }

    // Get target name
    std::string targetName = "Target";
    if (target->getType() == ObjectType::PLAYER) {
        auto player = std::static_pointer_cast<Player>(target);
        if (!player->getName().empty()) {
            targetName = player->getName();
        }
    } else if (target->getType() == ObjectType::UNIT) {
        auto unit = std::static_pointer_cast<Unit>(target);
        targetName = unit->getName();
    }

    // Who they have selected, read the one way it is read. See targetGuidOfUnit.
    const uint64_t assistTargetGuid = unitTargetGuid(target);

    // Silent when they have no target, as 0x00525eb0 is.
    if (assistTargetGuid == 0) {
        LOG_INFO("Assist: ", targetName, " has no target");
        return;
    }

    // Set our target to their target (AssistUnit, 0x00525eb0 -> 0x00524bf0)
    targetUnit(assistTargetGuid);
    LOG_INFO("Assisting ", targetName, ", now targeting GUID: 0x", std::hex, assistTargetGuid, std::dec);
}

// ============================================================
// PvP
// ============================================================

void CombatHandler::togglePvp() {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot toggle PvP: not in world or not connected");
        return;
    }

    auto packet = TogglePvpPacket::build();
    owner_.getSocket()->send(packet);
    auto entity = owner_.getEntityManager().getEntity(owner_.getPlayerGuid());
    bool currentlyPvp = false;
    if (entity) {
        // UNIT_FIELD_FLAGS (index 59), bit 0x1000 = UNIT_FLAG_PVP
        currentlyPvp = (entity->getField(59) & 0x00001000) != 0;
    }
    // No line here: TogglePVP (0x00516840) only sends. The original speaks
    // when the flag comes back changed (0x006e0fd0: ERR_PVP_TOGGLE_ON/OFF on
    // screen and PVP_TOGGLE_ON/OFF_VERBOSE in chat). These were English.
    LOG_INFO("Toggled PvP flag (was ", currentlyPvp ? "on" : "off", ")");
}

void CombatHandler::setPvpState(uint8_t state) {
    if (!owner_.isInWorld()) return;
    auto packet = TogglePvpPacket::build(state);
    owner_.getSocket()->send(packet);
}

// ============================================================
// Death / Resurrection
// ============================================================

void CombatHandler::releaseSpirit() {
    if (owner_.getSocket() && owner_.getState() == WorldState::IN_WORLD) {
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (owner_.repopPendingRef() && now - static_cast<int64_t>(owner_.lastRepopRequestMsRef()) < 1000) {
            return;
        }
        auto packet = RepopRequestPacket::build();
        owner_.getSocket()->send(packet);
        owner_.selfResAvailableRef() = false;
        owner_.repopPendingRef() = true;
        owner_.lastRepopRequestMsRef() = static_cast<uint64_t>(now);
        // The corpse is asked for as the ghost flag comes (0x006df710).
        LOG_INFO("Sent CMSG_REPOP_REQUEST (Release Spirit)");
    }
}

float CombatHandler::getCorpseReclaimDelaySec() const {
    if (owner_.corpseReclaimAvailableMsRef() == 0) return 0.0f;
    auto nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (nowMs >= owner_.corpseReclaimAvailableMsRef()) return 0.0f;
    return static_cast<float>(owner_.corpseReclaimAvailableMsRef() - nowMs) / 1000.0f;
}

// 0x0051b800: a resurrection on offer is declined first (0x006d1d30), and the
// corpse is named where it is in sight; the server judges.
void CombatHandler::reclaimCorpse() {
    if (!owner_.getSocket()) return;
    if (!owner_.resurrectIsSpiritHealerRef()) declineResurrect();
    owner_.getSocket()->send(ReclaimCorpsePacket::build(owner_.corpseGuidRef()));
    LOG_INFO("Sent CMSG_RECLAIM_CORPSE for corpse guid=0x", std::hex, owner_.corpseGuidRef(), std::dec);
}

void CombatHandler::useSelfRes() {
    // canSelfRes rather than the packet flag alone: the soulstone half of the
    // answer is PLAYER_SELF_RES_SPELL, which arrives as a field and is never
    // announced.  What is cleared here is the SMSG_PRE_RESURRECT flag; the realm
    // clears its own field when the resurrection is spent, and a death popup is
    // gone once its button has been pressed, so there is nothing here to latch.
    if (!owner_.canSelfRes() || !owner_.getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_SELF_RES));
    owner_.getSocket()->send(pkt);
    owner_.selfResAvailableRef() = false;
    LOG_INFO("Sent CMSG_SELF_RES (Reincarnation / Twisting Nether)");
}

void CombatHandler::activateSpiritHealer(uint64_t npcGuid) {
    if (!owner_.isInWorld()) return;
    owner_.pendingSpiritHealerGuidRef() = npcGuid;
    auto packet = SpiritHealerActivatePacket::build(npcGuid);
    owner_.getSocket()->send(packet);
    owner_.resurrectPendingRef() = true;
    LOG_INFO("Sent CMSG_SPIRIT_HEALER_ACTIVATE for 0x", std::hex, npcGuid, std::dec);
}

void CombatHandler::acceptResurrect() {
    // Says which of the three refused it. Accepting a resurrect and nothing
    // happening looks the same from the chair whether the click never arrived,
    // the offer had already been cleared, or the packet went out and the server
    // ignored it - and only the last of those is not this function's fault.
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() ||
        !owner_.resurrectRequestPendingRef()) {
        LOG_WARNING("acceptResurrect refused: inWorld=",
                    owner_.getState() == WorldState::IN_WORLD,
                    " socket=", owner_.getSocket() != nullptr,
                    " offerPending=", owner_.resurrectRequestPendingRef());
        return;
    }
    if (owner_.resurrectIsSpiritHealerRef()) {
        auto activate = SpiritHealerActivatePacket::build(owner_.resurrectCasterGuidRef());
        owner_.getSocket()->send(activate);
        LOG_INFO("Sent CMSG_SPIRIT_HEALER_ACTIVATE for 0x",
                 std::hex, owner_.resurrectCasterGuidRef(), std::dec);
    } else {
        auto resp = ResurrectResponsePacket::build(owner_.resurrectCasterGuidRef(), true);
        owner_.getSocket()->send(resp);
        LOG_INFO("Sent CMSG_RESURRECT_RESPONSE (accept) for 0x",
                 std::hex, owner_.resurrectCasterGuidRef(), std::dec);
    }
    owner_.resurrectRequestPendingRef() = false;
    owner_.resurrectPendingRef() = true;
}

void CombatHandler::declineResurrect() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || !owner_.resurrectRequestPendingRef()) return;
    auto resp = ResurrectResponsePacket::build(owner_.resurrectCasterGuidRef(), false);
    owner_.getSocket()->send(resp);
    LOG_INFO("Sent CMSG_RESURRECT_RESPONSE (decline) for 0x",
             std::hex, owner_.resurrectCasterGuidRef(), std::dec);
    owner_.resurrectRequestPendingRef() = false;
}

// ============================================================
// XP
// ============================================================

uint32_t CombatHandler::killXp(uint32_t playerLevel, uint32_t victimLevel) {
    if (playerLevel == 0 || victimLevel == 0) return 0;

    int32_t grayLevel;
    if (playerLevel <= 5)        grayLevel = 0;
    else if (playerLevel <= 39)  grayLevel = static_cast<int32_t>(playerLevel) - 5 - static_cast<int32_t>(playerLevel) / 10;
    else if (playerLevel <= 59)  grayLevel = static_cast<int32_t>(playerLevel) - 1 - static_cast<int32_t>(playerLevel) / 5;
    else                         grayLevel = static_cast<int32_t>(playerLevel) - 9;

    if (static_cast<int32_t>(victimLevel) <= grayLevel) return 0;

    uint32_t baseXp = 45 + 5 * victimLevel;

    int32_t diff = static_cast<int32_t>(victimLevel) - static_cast<int32_t>(playerLevel);
    float multiplier = 1.0f + diff * 0.05f;
    if (multiplier < 0.1f) multiplier = 0.1f;
    if (multiplier > 2.0f) multiplier = 2.0f;

    return static_cast<uint32_t>(baseXp * multiplier);
}

// An Exhaustion.dbc row (the table at 0x00ad3838): its factor (+8) and its
// localized name (+0x14).
xp_gain_line::ExhaustionRow CombatHandler::exhaustionRow(int id) {
    if (!exhaustionLoaded_) {
        auto* am = owner_.services().assetManager;
        if (am && am->isInitialized()) {
            exhaustionLoaded_ = true;
            if (auto dbc = am->loadDBC("Exhaustion.dbc"); dbc && dbc->isLoaded()) {
                // ID, XP, Factor, OutdoorHours, InnHours, Name_lang.
                for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                    xp_gain_line::ExhaustionRow r;
                    r.found = true;
                    r.factor = dbc->getFloat(i, 2);
                    r.name = dbc->getString(i, 5);
                    exhaustionRows_[static_cast<int>(dbc->getUInt32(i, 0))] = std::move(r);
                }
            }
        }
    }
    const auto it = exhaustionRows_.find(id);
    return it == exhaustionRows_.end() ? xp_gain_line::ExhaustionRow{} : it->second;
}

void CombatHandler::handleXpGain(network::Packet& packet) {
    XpGainData data;
    if (!XpGainParser::parse(packet, data)) return;

    addCombatText(CombatTextEntry::XP_GAIN, static_cast<int32_t>(data.totalXp), 0, true);
    // 0x0050c720 -> 0x0050b300 -> 0x007191a0, for a victim other than the
    // player: "XP: <amount>" over the player, world text kind 4.
    if (data.victimGuid != owner_.getPlayerGuid() && owner_.getEntityManager().getEntity(owner_.getPlayerGuid())) {
        addWorldText(owner_.getPlayerGuid(), static_cast<uint8_t>(FloaterKind::Experience),
                     interfaceString("XP", "XP") + ": " + std::to_string(data.totalXp));
    }

    // As 0x0050c720 words it, as CHAT_MSG_COMBAT_XP_GAIN: the player's own
    // guid as the victim is COMBATLOG_XPLOSS_FIRSTPERSON_UNNAMED (0x0050b660);
    // anything else goes to 0x0050b300, which picks among the rested and
    // tired forms, the group-bonus and raid-penalty forms, the
    // Recruit-a-Friend quest form and the plain ones (game/xp_gain_line.hpp).
    const std::string xp = std::to_string(data.totalXp);
    std::string msg;
    if (data.victimGuid != 0 && data.victimGuid == owner_.getPlayerGuid()) {
        msg = interfaceFormat("COMBATLOG_XPLOSS_FIRSTPERSON_UNNAMED", "You lose %d experience.", {xp});
    } else {
        // Nothing at all without the player's own object (0x0050b300).
        if (!owner_.getEntityManager().getEntity(owner_.getPlayerGuid())) return;
        xp_gain_line::Gain g;
        g.xp = static_cast<int32_t>(data.totalXp);
        g.type = data.type;
        g.restlessXp = static_cast<int32_t>(data.restlessXp);
        g.groupRate = data.groupRate;
        g.referAFriend = data.referAFriend;
        g.restState = owner_.getPlayerRestState();
        if (data.victimGuid != 0) {
            if (auto victim = owner_.getEntityManager().getEntity(data.victimGuid)) {
                g.haveVictim = true;
                g.victimIsUnit = victim->getType() == ObjectType::UNIT || victim->getType() == ObjectType::PLAYER;
                g.victimName = owner_.lookupName(data.victimGuid);
            }
        }
        const xp_gain_line::Line line = xp_gain_line::choose(g, [this](int id) { return exhaustionRow(id); });
        msg = interfaceFormat(line.key, line.enUS, line.args);
    }
    owner_.addLocalChatLine(ChatType::COMBAT_XP_GAIN, msg);
}

} // namespace game
} // namespace wowee
