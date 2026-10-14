#include "game/combat_handler.hpp"
#include "game/spell_handler.hpp"
#include "audio/ui_sound_manager.hpp"
#include "addons/lua_api_registrations.hpp"
#include "game/combat_text_filter.hpp"
#include "ui/framexml_takeover.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/packet_parsers.hpp"
#include "game/entity.hpp"
#include "game/update_field_table.hpp"
#include "game/spell_unit_target.hpp"
#include "game/opcode_table.hpp"
#include "rendering/renderer.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "core/application.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "network/world_socket.hpp"
#include "game/interface_text.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iterator>
#include <random>

namespace wowee {
namespace game {

namespace {

/// SpellCastResult -> its SPELL_FAILED_ key, less the prefix, as 0x007fcca0
/// names them. The same table spell_handler.cpp keeps for the player's casts.
constexpr const char* kPetSpellFailedNames[] = {
    "SUCCESS", "AFFECTING_COMBAT", "ALREADY_AT_FULL_HEALTH", "ALREADY_AT_FULL_MANA",
    "ALREADY_AT_FULL_POWER", "ALREADY_BEING_TAMED", "ALREADY_HAVE_CHARM", "ALREADY_HAVE_SUMMON",
    "ALREADY_OPEN", "AURA_BOUNCED", "AUTOTRACK_INTERRUPTED", "BAD_IMPLICIT_TARGETS", "BAD_TARGETS",
    "CANT_BE_CHARMED", "CANT_BE_DISENCHANTED", "CANT_BE_DISENCHANTED_SKILL", "CANT_BE_MILLED",
    "CANT_BE_PROSPECTED", "CANT_CAST_ON_TAPPED", "CANT_DUEL_WHILE_INVISIBLE",
    "CANT_DUEL_WHILE_STEALTHED", "CANT_STEALTH", "CASTER_AURASTATE", "CASTER_DEAD", "CHARMED",
    "CHEST_IN_USE", "CONFUSED", "DONT_REPORT", "EQUIPPED_ITEM", "EQUIPPED_ITEM_CLASS",
    "EQUIPPED_ITEM_CLASS_MAINHAND", "EQUIPPED_ITEM_CLASS_OFFHAND", "ERROR", "FIZZLE", "FLEEING",
    "FOOD_LOWLEVEL", "HIGHLEVEL", "HUNGER_SATIATED", "IMMUNE", "INCORRECT_AREA", "INTERRUPTED",
    "INTERRUPTED_COMBAT", "ITEM_ALREADY_ENCHANTED", "ITEM_GONE", "ITEM_NOT_FOUND",
    "ITEM_NOT_READY", "LEVEL_REQUIREMENT", "LINE_OF_SIGHT", "LOWLEVEL", "LOW_CASTLEVEL",
    "MAINHAND_EMPTY", "MOVING", "NEED_AMMO", "NEED_AMMO_POUCH", "NEED_EXOTIC_AMMO",
    "NEED_MORE_ITEMS", "NOPATH", "NOT_BEHIND", "NOT_FISHABLE", "NOT_FLYING", "NOT_HERE",
    "NOT_INFRONT", "NOT_IN_CONTROL", "NOT_KNOWN", "NOT_MOUNTED", "NOT_ON_TAXI", "NOT_ON_TRANSPORT",
    "NOT_READY", "NOT_SHAPESHIFT", "NOT_STANDING", "NOT_TRADEABLE", "NOT_TRADING",
    "NOT_UNSHEATHED", "NOT_WHILE_GHOST", "NOT_WHILE_LOOTING", "NO_AMMO", "NO_CHARGES_REMAIN",
    "NO_CHAMPION", "NO_COMBO_POINTS", "NO_DUELING", "NO_ENDURANCE", "NO_FISH",
    "NO_ITEMS_WHILE_SHAPESHIFTED", "NO_MOUNTS_ALLOWED", "NO_PET", "NO_POWER", "NOTHING_TO_DISPEL",
    "NOTHING_TO_STEAL", "ONLY_ABOVEWATER", "ONLY_DAYTIME", "ONLY_INDOORS", "ONLY_MOUNTED",
    "ONLY_NIGHTTIME", "ONLY_OUTDOORS", "ONLY_SHAPESHIFT", "ONLY_STEALTHED", "ONLY_UNDERWATER",
    "OUT_OF_RANGE", "PACIFIED", "POSSESSED", "REAGENTS", "REQUIRES_AREA", "REQUIRES_SPELL_FOCUS",
    "ROOTED", "SILENCED", "SPELL_IN_PROGRESS", "SPELL_LEARNED", "SPELL_UNAVAILABLE", "STUNNED",
    "TARGETS_DEAD", "TARGET_AFFECTING_COMBAT", "TARGET_AURASTATE", "TARGET_DUELING",
    "TARGET_ENEMY", "TARGET_ENRAGED", "TARGET_FRIENDLY", "TARGET_IN_COMBAT", "TARGET_IS_PLAYER",
    "TARGET_IS_PLAYER_CONTROLLED", "TARGET_NOT_DEAD", "TARGET_NOT_IN_PARTY", "TARGET_NOT_LOOTED",
    "TARGET_NOT_PLAYER", "TARGET_NO_POCKETS", "TARGET_NO_WEAPONS", "TARGET_NO_RANGED_WEAPONS",
    "TARGET_UNSKINNABLE", "THIRST_SATIATED", "TOO_CLOSE", "TOO_MANY_OF_ITEM", "TOTEM_CATEGORY",
    "TOTEMS", "TRY_AGAIN", "UNIT_NOT_BEHIND", "UNIT_NOT_INFRONT", "WRONG_PET_FOOD",
    "NOT_WHILE_FATIGUED", "TARGET_NOT_IN_INSTANCE", "NOT_WHILE_TRADING", "TARGET_NOT_IN_RAID",
    "TARGET_FREEFORALL", "NO_EDIBLE_CORPSES", "ONLY_BATTLEGROUNDS", "TARGET_NOT_GHOST",
    "TRANSFORM_UNUSABLE", "WRONG_WEATHER", "DAMAGE_IMMUNE", "PREVENTED_BY_MECHANIC", "PLAY_TIME",
    "REPUTATION", "MIN_SKILL", "NOT_IN_ARENA", "NOT_ON_SHAPESHIFT", "NOT_ON_STEALTHED",
    "NOT_ON_DAMAGE_IMMUNE", "NOT_ON_MOUNTED", "TOO_SHALLOW", "TARGET_NOT_IN_SANCTUARY",
    "TARGET_IS_TRIVIAL", "BM_OR_INVISGOD", "EXPERT_RIDING_REQUIREMENT",
    "ARTISAN_RIDING_REQUIREMENT", "NOT_IDLE", "NOT_INACTIVE", "PARTIAL_PLAYTIME", "NO_PLAYTIME",
    "NOT_IN_BATTLEGROUND", "NOT_IN_RAID_INSTANCE", "ONLY_IN_ARENA",
    "TARGET_LOCKED_TO_RAID_INSTANCE", "ON_USE_ENCHANT", "NOT_ON_GROUND", "CUSTOM_ERROR",
    "CANT_DO_THAT_RIGHT_NOW", "TOO_MANY_SOCKETS", "INVALID_GLYPH", "UNIQUE_GLYPH",
    "GLYPH_SOCKET_LOCKED", "NO_VALID_TARGETS", "ITEM_AT_MAX_CHARGES", "NOT_IN_BARBERSHOP",
    "FISHING_TOO_LOW", "ITEM_ENCHANT_TRADE_WINDOW", "SUMMON_PENDING", "MAX_SOCKETS",
    "PET_CAN_RENAME", "TARGET_CANNOT_BE_RESURRECTED",
};

/// The on-screen line for the pet's failed cast, as 0x00805610 words it: its
/// own ERR_PET_SPELL_* lines for the results it has one for, the cooldown and
/// power lines, and otherwise the SPELL_FAILED_ text through
/// ERR_SPELL_FAILED_S. Where the result names something not looked up here
/// (an item class, a form, an area, a focus object, a mechanic, a custom
/// error) the English this client had is kept rather than an unfilled %s.
std::string petCastFailureMessage(const GameHandler& owner, uint32_t spellId, uint8_t result) {
    switch (result) {
        case 0x01: return interfaceString("ERR_PET_SPELL_AFFECTING_COMBAT", "Your pet is in combat");
        case 0x17: return interfaceString("ERR_PET_SPELL_DEAD", "Your pet is dead");
        case 0x38: return interfaceString("ERR_PET_SPELL_NOPATH", "No path available");
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
        case 0x1d: case 0x1e: case 0x1f: case 0x5e: case 0x65: case 0x66: case 0x93: case 0xac: {
            const char* enUS = getSpellCastResultString(result);
            return enUS ? std::string(enUS) : std::string();
        }
        default:
            break;
    }
    const std::string key = std::string("SPELL_FAILED_") +
        (result < std::size(kPetSpellFailedNames) ? kPetSpellFailedNames[result] : "UNKNOWN");
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
    table[Opcode::SMSG_ATTACKSWING_DEADTARGET] = [this](network::Packet& /*packet*/) {
        autoAttacking_ = false;
        autoAttackTarget_ = 0;
    };
    table[Opcode::SMSG_THREAT_CLEAR] = [this](network::Packet& /*packet*/) {
        threatLists_.clear();
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("UNIT_THREAT_LIST_UPDATE", {});
    };
    table[Opcode::SMSG_THREAT_REMOVE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(1)) return;
        uint64_t unitGuid   = packet.readPackedGuid();
        if (!packet.hasRemaining(1)) return;
        uint64_t victimGuid = packet.readPackedGuid();
        auto it = threatLists_.find(unitGuid);
        if (it != threatLists_.end()) {
            auto& list = it->second;
            list.erase(std::remove_if(list.begin(), list.end(),
                [victimGuid](const ThreatEntry& e){ return e.victimGuid == victimGuid; }),
                list.end());
            if (list.empty()) threatLists_.erase(it);
        }
    };
    table[Opcode::SMSG_CANCEL_COMBAT] = [this](network::Packet& /*packet*/) {
        autoAttacking_ = false;
        autoAttackTarget_ = 0;
        autoAttackRequested_ = false;
        autoAttackRetryPending_ = false;
        hostileAttackers_.clear();
    };

    // ---- Attack/combat delegates ----
    table[Opcode::SMSG_ATTACKSTART] = [this](network::Packet& packet) { handleAttackStart(packet); };
    table[Opcode::SMSG_ATTACKSTOP] = [this](network::Packet& packet) { handleAttackStop(packet); };
    table[Opcode::SMSG_ATTACKSWING_NOTINRANGE] = [this](network::Packet& /*packet*/) {
        autoAttackOutOfRange_ = true;
        // ERR_BADATTACKPOS, at most every four seconds: 0x00756800 notes the
        // refusal and 0x006d5eb0 reports it. This was English.
        if (autoAttackRangeWarnCooldown_ <= 0.0f) {
            owner_.raiseUiError(interfaceString("ERR_BADATTACKPOS", "You are too far away!"));
            autoAttackRangeWarnCooldown_ = 4.0f;
        }
    };
    table[Opcode::SMSG_ATTACKSWING_BADFACING] = [this](network::Packet& /*packet*/) {
        // Reported, not corrected. This used to turn the character to face the
        // target, which is not something the server may do to a player: the
        // facing is the player's. Running away from something with auto-attack
        // still on had the server say "not facing" every swing timer and the
        // character spin round to face it each time, fighting the player for
        // control of their own heading.
        //
        // The real client says so and leaves the heading alone - the swing
        // simply does not land until the player turns back.
        // ERR_BADATTACKFACING, by the same four-second rule (0x006d5eb0).
        if (autoAttackRangeWarnCooldown_ <= 0.0f) {
            owner_.raiseUiError(interfaceString("ERR_BADATTACKFACING", "You are facing the wrong way!"));
            autoAttackRangeWarnCooldown_ = 4.0f;
        }
    };
    table[Opcode::SMSG_ATTACKSWING_NOTSTANDING] = [this](network::Packet& /*packet*/) {
        autoAttackOutOfRange_ = false;
        autoAttackOutOfRangeTime_ = 0.0f;
        if (autoAttackRangeWarnCooldown_ <= 0.0f) {
            owner_.raiseUiError("You need to stand up to fight.");
            autoAttackRangeWarnCooldown_ = 1.25f;
        }
    };
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
        if (reaction == 2 && owner_.npcAggroCallbackRef()) {
            auto entity = owner_.getEntityManager().getEntity(guid);
            if (entity)
                owner_.npcAggroCallbackRef()(guid, glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
        }
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
                if (topNow && threatWarningWanted() &&
                    addons::storedCVarValue("threatPlaySounds", "0") != "0") {
                    if (auto* ac = owner_.services().audioCoordinator) {
                        if (auto* sfx = ac->getUiSoundManager()) sfx->playError();
                    }
                }
            }

            threatLists_[unitGuid] = std::move(list);
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

    // Client-side range gate to avoid starting "swing forever" loops when
    // target is already clearly out of range.
    if (auto target = owner_.getEntityManager().getEntity(targetGuid)) {
        float dx = owner_.movementInfoRef().x - target->getLatestX();
        float dy = owner_.movementInfoRef().y - target->getLatestY();
        float dz = owner_.movementInfoRef().z - target->getLatestZ();
        float dist3d = std::sqrt(dx * dx + dy * dy + dz * dz);
        // Use longer range limit when a ranged weapon is equipped
        const auto& rangedSlot = owner_.getInventory().getEquipSlot(game::EquipSlot::RANGED);
        bool hasRangedWeapon = !rangedSlot.empty() &&
            (rangedSlot.item.inventoryType == game::InvType::RANGED_BOW ||
             rangedSlot.item.inventoryType == game::InvType::RANGED_GUN ||
             rangedSlot.item.inventoryType == game::InvType::THROWN);
        float maxRange = hasRangedWeapon ? 40.0f : 8.0f;
        if (dist3d > maxRange) {
            // The ERR_BADATTACKPOS the server's refusal would have brought
            // (see SMSG_ATTACKSWING_NOTINRANGE); the original sends the swing
            // and lets it be refused. This was English.
            if (autoAttackRangeWarnCooldown_ <= 0.0f) {
                owner_.raiseUiError(interfaceString("ERR_BADATTACKPOS", "You are too far away!"));
                autoAttackRangeWarnCooldown_ = 4.0f;
            }
            return;
        }
    }

    // Only dismount once this is a valid attack attempt. Doing it before the
    // target/range gate knocked the player off a mount even though no combat
    // request was sent.
    //
    // Never in the air, unless Auto Dismount in Flight asks for it: a hostile
    // right-clicked from a flying mount dropped the player out of the sky.
    if (owner_.isMounted()) {
        if (owner_.isPlayerFlying() && !owner_.isTaxiMountActive() &&
            addons::storedCVarValue("autoDismountFlying", "0") == "0") {
            // ERR_ATTACK_MOUNTED, as 0x0072bdb0 refuses an attack it will not
            // dismount for (0x0071b0c0). This was English.
            owner_.addUIError(interfaceString("ERR_ATTACK_MOUNTED", "Can't attack while mounted."));
            return;
        }
        owner_.dismount();
    }

    autoAttackRequested_ = true;
    autoAttackRetryPending_ = true;
    // Keep combat animation/state server-authoritative. We only flip autoAttacking
    // on SMSG_ATTACKSTART where attackerGuid == playerGuid.
    autoAttacking_ = false;
    autoAttackTarget_ = targetGuid;
    autoAttackOutOfRange_ = false;
    autoAttackOutOfRangeTime_ = 0.0f;
    autoAttackResendTimer_ = 0.0f;
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
    if (owner_.getState() == WorldState::IN_WORLD && owner_.getSocket()) {
        auto packet = AttackSwingPacket::build(targetGuid);
        owner_.getSocket()->send(packet);
    }
    LOG_INFO("Starting auto-attack on 0x", std::hex, targetGuid, std::dec);
}

void CombatHandler::stopAutoAttack() {
    if (!autoAttacking_ && !autoAttackRequested_) return;
    autoAttackRequested_ = false;
    autoAttacking_ = false;
    autoAttackRetryPending_ = false;
    autoAttackTarget_ = 0;
    autoAttackOutOfRange_ = false;
    autoAttackOutOfRangeTime_ = 0.0f;
    autoAttackResendTimer_ = 0.0f;
    if (owner_.getState() == WorldState::IN_WORLD && owner_.getSocket()) {
        auto packet = AttackStopPacket::build();
        owner_.getSocket()->send(packet);
    }
    LOG_INFO("Stopping auto-attack");
    if (owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("PLAYER_LEAVE_COMBAT", {});
}

// ============================================================
// Combat text
// ============================================================

void CombatHandler::addCombatText(CombatTextEntry::Type type, int32_t amount, uint32_t spellId, bool isPlayerSource, uint8_t powerType,
                                  uint64_t srcGuid, uint64_t dstGuid) {
    // The panel's filters, before anything is built or announced. The mapping
    // lives in combat_text_filter.hpp so it can be tested without a settings
    // store; only the lookup is here.
    const auto rule = combatTextFilterFor(type, isPlayerSource, srcGuid, dstGuid,
                                          owner_.getPetGuid(), owner_.getTargetGuid());
    if (rule.cvar && addons::storedCVarValue(rule.cvar, rule.fallback) == "0") return;

    CombatTextEntry entry;
    entry.type = type;
    entry.amount = amount;
    entry.spellId = spellId;
    entry.age = 0.0f;
    entry.isPlayerSource = isPlayerSource;
    entry.powerType = powerType;
    entry.srcGuid = srcGuid;
    entry.dstGuid = dstGuid;
    // Random horizontal stagger so simultaneous hits don't stack vertically
    static std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    entry.xSeed = dist(rng);
    combatText_.push_back(entry);

    // UNIT_COMBAT - the same hit, for the frames that draw their own feedback
    // over a portrait. This client renders the floating numbers itself from the
    // list above; the interface's unit frames render theirs from this event and
    // were never told, so a pet or target frame showed nothing while the world
    // filled with numbers.
    //
    // The tokens are not invented: CombatFeedback_OnCombatEvent enumerates the
    // ones it acts on, and everything else falls through its final else to a
    // blank. Only a unit the interface has a token for is worth announcing -
    // the frame compares the first argument against its own unit.
    if (dstGuid != 0 && owner_.addonEventCallbackRef()) {
        const std::string unitId = owner_.guidToUnitId(dstGuid);
        if (!unitId.empty()) {
            const char* action = nullptr;
            const char* flags  = "";
            switch (type) {
                case CombatTextEntry::MELEE_DAMAGE:
                case CombatTextEntry::SPELL_DAMAGE:
                case CombatTextEntry::PERIODIC_DAMAGE: action = "WOUND"; break;
                case CombatTextEntry::CRIT_DAMAGE: action = "WOUND"; flags = "CRITICAL"; break;
                case CombatTextEntry::GLANCING:    action = "WOUND"; flags = "GLANCING"; break;
                case CombatTextEntry::CRUSHING:    action = "WOUND"; flags = "CRUSHING"; break;
                case CombatTextEntry::ABSORB:      action = "WOUND"; flags = "ABSORB";   break;
                case CombatTextEntry::MISS:   action = "MISS";   break;
                case CombatTextEntry::DODGE:  action = "DODGE";  break;
                case CombatTextEntry::PARRY:  action = "PARRY";  break;
                case CombatTextEntry::BLOCK:  action = "BLOCK";  break;
                case CombatTextEntry::EVADE:  action = "EVADE";  break;
                case CombatTextEntry::IMMUNE: action = "IMMUNE"; break;
                case CombatTextEntry::RESIST: action = "RESIST"; break;
                case CombatTextEntry::HEAL:
                case CombatTextEntry::PERIODIC_HEAL: action = "HEAL"; break;
                case CombatTextEntry::CRIT_HEAL: action = "HEAL"; flags = "CRITICAL"; break;
                default: break;  // energize, xp, honour - not combat feedback
            }
            if (action) {
                owner_.addonEventCallbackRef()(
                    "UNIT_COMBAT",
                    {unitId, action, flags, std::to_string(amount), "0"});
            }
        }
    }

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
    // Held for the addon event below. log is moved into combatLog_ here, which
    // leaves both name strings empty, so reading them afterwards sent every
    // COMBAT_LOG_EVENT_UNFILTERED to Lua with a blank source and target.
    const std::string sourceName = log.sourceName;
    const std::string targetName = log.targetName;
    if (combatLog_.size() >= MAX_COMBAT_LOG)
        combatLog_.pop_front();
    combatLog_.push_back(std::move(log));

    // Fire COMBAT_LOG_EVENT_UNFILTERED for Lua addons
    // Args: subevent, sourceGUID, sourceName, 0 (sourceFlags), destGUID, destName, 0 (destFlags), spellId, spellName, amount
    //
    // Not for experience or honour: neither is a combat log entry in the
    // original (they are CHAT_MSG_COMBAT_XP_GAIN / _HONOR_GAIN lines), and the
    // table below gave both the slot of PARTY_KILL - so every kill's
    // experience told the combat log a second, invented kill.
    if (owner_.addonEventCallbackRef() &&
        type != CombatTextEntry::XP_GAIN && type != CombatTextEntry::HONOR_GAIN) {
        static const char* kSubevents[] = {
            "SWING_DAMAGE", "SPELL_DAMAGE", "SPELL_HEAL", "SWING_MISSED", "SWING_MISSED",
            "SWING_MISSED", "SWING_MISSED", "SWING_MISSED", "SPELL_DAMAGE", "SPELL_HEAL",
            "SPELL_PERIODIC_DAMAGE", "SPELL_PERIODIC_HEAL", "ENVIRONMENTAL_DAMAGE",
            "SPELL_ENERGIZE", "SPELL_DRAIN", "PARTY_KILL", "SPELL_MISSED", "SPELL_ABSORBED",
            "SPELL_MISSED", "SPELL_MISSED", "SPELL_MISSED", "SPELL_AURA_APPLIED",
            "SPELL_DISPEL", "SPELL_STOLEN", "SPELL_INTERRUPT", "SPELL_INSTAKILL",
            "PARTY_KILL", "SWING_DAMAGE", "SWING_DAMAGE"
        };
        const char* subevent = (type < sizeof(kSubevents)/sizeof(kSubevents[0]))
            ? kSubevents[type] : "UNKNOWN";
        char srcBuf[32], dstBuf[32];
        snprintf(srcBuf, sizeof(srcBuf), "0x%016llX", (unsigned long long)effectiveSrc);
        snprintf(dstBuf, sizeof(dstBuf), "0x%016llX", (unsigned long long)effectiveDst);
        std::string spellName = (spellId != 0) ? owner_.getSpellName(spellId) : std::string{};
        std::string timestamp = std::to_string(static_cast<double>(std::time(nullptr)));
        owner_.addonEventCallbackRef()("COMBAT_LOG_EVENT_UNFILTERED", {
            timestamp, subevent,
            srcBuf, sourceName, "0",
            dstBuf, targetName, "0",
            std::to_string(spellId), spellName,
            std::to_string(amount)
        });
    }
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
    // 0x00755020 from SMSG_ATTACKSTART (0x00756800).
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
        if (guid == owner_.getPlayerGuid() && owner_.stealthStateCallbackRef()) {
            owner_.stealthStateCallbackRef()(false);
        }
    };
    revealCombatant(data.attackerGuid);
    revealCombatant(data.victimGuid);
    // 0x00756800 case 0x143: the attacker's melee target (0x00754ff0).
    if (data.attackerGuid != owner_.getPlayerGuid()) unitMeleeTargets_[data.attackerGuid] = data.victimGuid;

    if (data.attackerGuid == owner_.getPlayerGuid()) {
        autoAttackRequested_ = true;
        autoAttacking_ = true;
        autoAttackRetryPending_ = false;
        autoAttackTarget_ = data.victimGuid;
        if (owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("PLAYER_ENTER_COMBAT", {});
    } else if (data.victimGuid == owner_.getPlayerGuid() && data.attackerGuid != 0) {
        hostileAttackers_.insert(data.attackerGuid);
        autoTargetAttacker(data.attackerGuid);

        // Play aggro sound when NPC attacks player
        if (owner_.npcAggroCallbackRef()) {
            auto entity = owner_.getEntityManager().getEntity(data.attackerGuid);
            if (entity && entity->getType() == ObjectType::UNIT) {
                glm::vec3 pos(entity->getX(), entity->getY(), entity->getZ());
                owner_.npcAggroCallbackRef()(data.attackerGuid, pos);
            }
        }
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

    // 0x00756770: the attacker's melee target is gone.
    unitMeleeTargets_.erase(data.attackerGuid);
    // Keep intent, but clear server-confirmed active state until ATTACKSTART resumes.
    if (data.attackerGuid == owner_.getPlayerGuid()) {
        autoAttacking_ = false;
        autoAttackRetryPending_ = autoAttackRequested_;
        autoAttackResendTimer_ = 0.0f;
        LOG_DEBUG("SMSG_ATTACKSTOP received (keeping auto-attack intent)");
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

    bool isPlayerAttacker = (data.attackerGuid == owner_.getPlayerGuid());
    bool isPlayerTarget = (data.targetGuid == owner_.getPlayerGuid());
    // 0x00756800 case 0x14a: any attacker's swing draws its melee weapons.
    if (data.attackerGuid != 0 && owner_.unitAttackSwingCallbackRef()) {
        owner_.unitAttackSwingCallbackRef()(data.attackerGuid);
    }
    if (!isPlayerAttacker && !isPlayerTarget) return;  // Not our combat

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
    if (!isPlayerAttacker) {
        auto entity = owner_.getEntityManager().getEntity(data.attackerGuid);
        auto* audioCoordinator = owner_.services().audioCoordinator;
        auto* voiceManager = audioCoordinator ? audioCoordinator->getNpcVoiceManager() : nullptr;
        if (entity && entity->getType() == ObjectType::UNIT && voiceManager) {
            auto unit = std::static_pointer_cast<Unit>(entity);
            const glm::vec3 canonical(unit->getLatestX(), unit->getLatestY(), unit->getLatestZ());
            voiceManager->playCombatAttack(
                data.attackerGuid, unit->getDisplayId(), core::coords::canonicalToRender(canonical));
        }
    }

    if (isPlayerTarget && data.attackerGuid != 0) {
        hostileAttackers_.insert(data.attackerGuid);
        autoTargetAttacker(data.attackerGuid);
    }

    // Play combat sounds via CombatSoundManager + character vocalizations
    if (auto* ac = owner_.services().audioCoordinator) {
        if (auto* csm = ac->getCombatSoundManager()) {
            auto weaponSize = audio::CombatSoundManager::WeaponSize::MEDIUM;
            if (data.isMiss()) {
                csm->playWeaponMiss(false);
            } else if (data.victimState == 2 || data.victimState == 3) {
                // Dodge/parry - swing whoosh but no impact
                csm->playWeaponSwing(weaponSize, false);
            } else {
                // Hit - swing + flesh impact
                csm->playWeaponSwing(weaponSize, data.isCrit());
                csm->playImpact(weaponSize, audio::CombatSoundManager::ImpactType::FLESH, data.isCrit());
            }
        }
        // Character vocalizations
        if (auto* asm_ = ac->getActivitySoundManager()) {
            if (isPlayerAttacker && !data.isMiss() && data.victimState != 2 && data.victimState != 3) {
                asm_->playAttackGrunt();
            }
            if (isPlayerTarget && !data.isMiss() && data.victimState != 2 && data.victimState != 3) {
                asm_->playWound(data.isCrit());
            }
        }
    }

    // 0x007512b0's order: a hit (1) or a block (5) that did damage, then a
    // miss, a full absorb, a full resist, and the victim's state.
    uint32_t totalAbsorbed = 0, totalResisted = 0;
    for (const auto& sub : data.subDamages) {
        totalAbsorbed += sub.absorbed;
        totalResisted += sub.resisted;
    }
    if ((data.victimState == 1 || data.victimState == 5) && data.totalDamage != 0) {
        CombatTextEntry::Type type;
        if (data.isCrit())
            type = CombatTextEntry::CRIT_DAMAGE;
        else if (data.isCrushing())
            type = CombatTextEntry::CRUSHING;
        else if (data.isGlancing())
            type = CombatTextEntry::GLANCING;
        else
            type = CombatTextEntry::MELEE_DAMAGE;
        addCombatText(type, data.totalDamage, 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
        if (data.blocked > 0)
            addCombatText(CombatTextEntry::BLOCK, static_cast<int32_t>(data.blocked), 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
        if (totalAbsorbed > 0)
            addCombatText(CombatTextEntry::ABSORB, static_cast<int32_t>(totalAbsorbed), 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
        if (totalResisted > 0)
            addCombatText(CombatTextEntry::RESIST, static_cast<int32_t>(totalResisted), 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
    } else if (data.isMiss()) {
        addCombatText(CombatTextEntry::MISS, 0, 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
    } else if ((data.hitInfo & 0x20u) != 0) {
        addCombatText(CombatTextEntry::ABSORB, static_cast<int32_t>(totalAbsorbed), 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
    } else if ((data.hitInfo & 0x80u) != 0) {
        addCombatText(CombatTextEntry::RESIST, static_cast<int32_t>(totalResisted), 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
    } else if (data.victimState == 2) {
        addCombatText(CombatTextEntry::DODGE, 0, 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
    } else if (data.victimState == 3) {
        addCombatText(CombatTextEntry::PARRY, 0, 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
    } else if (data.victimState == 5) {
        addCombatText(CombatTextEntry::BLOCK, static_cast<int32_t>(data.blocked), 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
    } else if (data.victimState == 6) {
        addCombatText(CombatTextEntry::EVADE, 0, 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
    } else if (data.victimState == 7) {
        addCombatText(CombatTextEntry::IMMUNE, 0, 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
    } else if (data.victimState == 8) {
        addCombatText(CombatTextEntry::DEFLECT, 0, 0, isPlayerAttacker, 0, data.attackerGuid, data.targetGuid);
    }

    // Fire hit reaction animation on the victim
    if (owner_.hitReactionCallbackRef() && !data.isMiss()) {
        using HR = GameHandler::HitReaction;
        HR reaction = HR::WOUND;
        if (data.victimState == 2) reaction = HR::DODGE;
        else if (data.victimState == 3) reaction = HR::PARRY;
        else if (data.victimState == 5) reaction = HR::BLOCK;
        else if (data.isCrit()) reaction = HR::CRIT_WOUND;
        owner_.hitReactionCallbackRef()(data.targetGuid, reaction);
    }

}

void CombatHandler::handleSpellDamageLog(network::Packet& packet) {
    SpellDamageLogData data;
    if (!owner_.getPacketParsers()->parseSpellDamageLog(packet, data)) return;

    bool isPlayerSource = (data.attackerGuid == owner_.getPlayerGuid());
    bool isPlayerTarget = (data.targetGuid == owner_.getPlayerGuid());
    if (!isPlayerSource && !isPlayerTarget) return;  // Not our combat

    if (isPlayerTarget && data.attackerGuid != 0) {
        hostileAttackers_.insert(data.attackerGuid);
        autoTargetAttacker(data.attackerGuid);
    }

    auto type = data.isCrit ? CombatTextEntry::CRIT_DAMAGE : CombatTextEntry::SPELL_DAMAGE;
    if (data.damage > 0)
        addCombatText(type, static_cast<int32_t>(data.damage), data.spellId, isPlayerSource, 0, data.attackerGuid, data.targetGuid);
    if (data.absorbed > 0)
        addCombatText(CombatTextEntry::ABSORB, static_cast<int32_t>(data.absorbed), data.spellId, isPlayerSource, 0, data.attackerGuid, data.targetGuid);
    if (data.resisted > 0)
        addCombatText(CombatTextEntry::RESIST, static_cast<int32_t>(data.resisted), data.spellId, isPlayerSource, 0, data.attackerGuid, data.targetGuid);
}

void CombatHandler::handleSpellHealLog(network::Packet& packet) {
    SpellHealLogData data;
    if (!owner_.getPacketParsers()->parseSpellHealLog(packet, data)) return;

    bool isPlayerSource = (data.casterGuid == owner_.getPlayerGuid());
    bool isPlayerTarget = (data.targetGuid == owner_.getPlayerGuid());
    if (!isPlayerSource && !isPlayerTarget) return;  // Not our combat

    auto type = data.isCrit ? CombatTextEntry::CRIT_HEAL : CombatTextEntry::HEAL;
    addCombatText(type, static_cast<int32_t>(data.heal), data.spellId, isPlayerSource, 0, data.casterGuid, data.targetGuid);
    if (data.absorbed > 0)
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

void CombatHandler::updateAutoAttack(float deltaTime) {
    // Decrement range warn cooldown
    if (autoAttackRangeWarnCooldown_ > 0.0f) {
        autoAttackRangeWarnCooldown_ = std::max(0.0f, autoAttackRangeWarnCooldown_ - deltaTime);
    }

    // Leave combat if auto-attack target is too far away (leash range)
    // and keep melee intent tightly synced while stationary.
    if (autoAttackRequested_ && autoAttackTarget_ != 0) {
        auto targetEntity = owner_.getEntityManager().getEntity(autoAttackTarget_);
        if (targetEntity) {
            const float targetX = targetEntity->getLatestX();
            const float targetY = targetEntity->getLatestY();
            const float targetZ = targetEntity->getLatestZ();
            float dx = owner_.movementInfoRef().x - targetX;
            float dy = owner_.movementInfoRef().y - targetY;
            float dz = owner_.movementInfoRef().z - targetZ;
            float dist = std::sqrt(dx * dx + dy * dy);
            float dist3d = std::sqrt(dx * dx + dy * dy + dz * dz);
            const bool classicLike = isPreWotlk();
            if (dist > 40.0f) {
                stopAutoAttack();
                LOG_INFO("Left combat: target too far (", dist, " yards)");
            } else if (owner_.isInWorld()) {
                bool allowResync = true;
                const float meleeRange = classicLike ? 5.25f : 5.75f;
                if (dist3d > meleeRange) {
                    autoAttackOutOfRange_ = true;
                    autoAttackOutOfRangeTime_ += deltaTime;
                    // ERR_BADATTACKPOS once, at most every four seconds, as
                    // for the server's refusal. This raised an English line
                    // twice over.
                    if (autoAttackRangeWarnCooldown_ <= 0.0f) {
                        owner_.raiseUiError(interfaceString("ERR_BADATTACKPOS", "You are too far away!"));
                        autoAttackRangeWarnCooldown_ = 4.0f;
                    }
                    // Stop chasing stale swings when the target remains out of range.
                    // Silently: the interface has no line for it.
                    if (autoAttackOutOfRangeTime_ > 2.0f && dist3d > 9.0f) {
                        stopAutoAttack();
                        allowResync = false;
                    }
                } else {
                    autoAttackOutOfRange_ = false;
                    autoAttackOutOfRangeTime_ = 0.0f;
                }

                if (allowResync) {
                    autoAttackResendTimer_ += deltaTime;

                    // Classic/Turtle servers do not tolerate steady attack-start
                    // reissues well. Only retry once after local start or an
                    // explicit server-side attack stop while intent is still set.
                    const float resendInterval = classicLike ? 1.0f : 0.50f;
                    if (!autoAttacking_ && !autoAttackOutOfRange_ && autoAttackRetryPending_ &&
                        autoAttackResendTimer_ >= resendInterval) {
                        autoAttackResendTimer_ = 0.0f;
                        autoAttackRetryPending_ = false;
                        auto pkt = AttackSwingPacket::build(autoAttackTarget_);
                        owner_.getSocket()->send(pkt);
                    }

                    // No periodic facing sync. The player is turned to face the
                    // target once, where the attack is commanded, and after
                    // that their facing is theirs: turning away mid-fight
                    // means the swing misses, which is what it means in the
                    // real client. Re-aiming them every fifth of a second so
                    // the server would accept the swing fought the mouse for
                    // the whole fight and made it impossible to back away from
                    // something while attacked by it.
                }
            }
        }
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

void CombatHandler::resetAllCombatState() {
    hostileAttackers_.clear();
    unitMeleeTargets_.clear();
    combatText_.clear();
    autoAttacking_ = false;
    autoAttackRequested_ = false;
    autoAttackRetryPending_ = false;
    autoAttackTarget_ = 0;
    autoAttackOutOfRange_ = false;
    autoAttackOutOfRangeTime_ = 0.0f;
    autoAttackRangeWarnCooldown_ = 0.0f;
    autoAttackResendTimer_ = 0.0f;
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
    if (auto* unit = owner_.getUnitByGuid(guid)) unit->setHealth(hp);
    if (guid != 0) {
        auto unitId = owner_.guidToUnitId(guid);
        if (!unitId.empty()) owner_.fireAddonEvent("UNIT_HEALTH", {unitId});
    }
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
    owner_.fireAddonEvent("PLAYER_COMBO_POINTS", {});
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
        if (owner_.pvpHonorCallbackRef()) owner_.pvpHonorCallbackRef()(honor, victimGuid, rank);
    }
}

void CombatHandler::handleProcResist(network::Packet& packet) {
    // 0x00750d40: full guids, the spell and a debug byte.
    if (isActiveExpansion("wotlk")) {
        SpellLogGuidsData d;
        if (SpellLogGuidsParser::parse(packet, d, true)) {
            if (d.victimGuid == owner_.getPlayerGuid())
                addCombatText(CombatTextEntry::RESIST, 0, d.spellId, false, 0, d.casterGuid, d.victimGuid);
            else if (d.casterGuid == owner_.getPlayerGuid())
                addCombatText(CombatTextEntry::RESIST, 0, d.spellId, true, 0, d.casterGuid, d.victimGuid);
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
        if (d.casterGuid == owner_.getPlayerGuid())
            addCombatText(CombatTextEntry::SPELL_DAMAGE, static_cast<int32_t>(d.damage), d.spellId, true, 0, d.casterGuid, d.victimGuid);
        else if (d.victimGuid == owner_.getPlayerGuid())
            addCombatText(CombatTextEntry::SPELL_DAMAGE, static_cast<int32_t>(d.damage), d.spellId, false, 0, d.casterGuid, d.victimGuid);
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
        if (SpellLogGuidsParser::parse(packet, d, true) &&
            (d.casterGuid == owner_.getPlayerGuid() || d.victimGuid == owner_.getPlayerGuid()))
            addCombatText(CombatTextEntry::IMMUNE, 0, d.spellId, d.casterGuid == owner_.getPlayerGuid(), 0,
                          d.casterGuid, d.victimGuid);
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
    // spell aimed at friends; 3 ERR_INVALID_ATTACK_TARGET; 4
    // ERR_PET_SPELL_NOPATH; nothing else. These were English chat lines.
    if (!packet.hasRemaining(1)) return;
    const uint8_t msg = packet.readUInt8();
    switch (msg) {
        case 1:
            owner_.addUIError(interfaceString("ERR_PET_SPELL_DEAD", "Your pet is dead"));
            break;
        case 2: {
            const uint32_t spellId = packet.hasRemaining(4) ? packet.readUInt32() : 0;
            if (spellId != 0 && owner_.getSpellTargetKind(spellId) == 1)
                owner_.addUIError(interfaceString("ERR_GENERIC_NO_VALID_TARGETS", "No valid targets."));
            else
                owner_.addUIError(interfaceString("ERR_NO_ATTACK_TARGET", "There is nothing to attack."));
            break;
        }
        case 3:
            owner_.addUIError(interfaceString("ERR_INVALID_ATTACK_TARGET", "You cannot attack that target."));
            break;
        case 4:
            owner_.addUIError(interfaceString("ERR_PET_SPELL_NOPATH", "No path available"));
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
        if (hasCount) /*uint8_t castCount =*/ packet.readUInt8();
        uint32_t spellId   = packet.readUInt32();
        uint8_t  reason    = (packet.hasRemaining(1))
                                 ? packet.readUInt8() : 0;
        LOG_DEBUG("SMSG_PET_CAST_FAILED: spell=", spellId,
                  " reason=", static_cast<int>(reason));
        // 0x00806c30 -> 0x00804720: its global cooldown off the pet's
        // history.
        if (auto* spells = owner_.getSpellHandler()) spells->notePetCastFailed(spellId);
        // On screen and not in chat, as 0x00806c30 -> 0x00805610 says it;
        // see petCastFailureMessage. This was "Spell: reason" in English, in
        // chat.
        if (reason != 0) {
            const std::string errMsg = petCastFailureMessage(owner_, spellId, reason);
            if (!errMsg.empty()) owner_.addUIError(errMsg);
        }
    }
    packet.skipAll();
}

void CombatHandler::handlePetBroken(network::Packet& packet) {
    // Pet bond broken (died or forcibly dismissed) - clear pet state
    owner_.petGuidRef() = 0;
    owner_.petSpellListRef().clear();
    owner_.petAutocastSpellsRef().clear();
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
    // uint64 petGuid, uint32 mode
    // mode bits: low byte = command state, next byte = react state
    if (packet.hasRemaining(12)) {
        uint64_t modeGuid = packet.readUInt64();
        uint32_t mode     = packet.readUInt32();
        if (modeGuid == owner_.petGuidRef()) {
            // COMMAND_ATTACK is 2. The pet frame lights its attack indicator on
            // the start and clears it on the stop, so only the crossing matters
            // - this packet arrives for every mode change, and firing on each
            // would relight an indicator that is already lit.
            constexpr uint8_t kCommandAttack = 2;
            const bool wasAttacking = owner_.petCommandRef() == kCommandAttack;
            owner_.petCommandRef() = static_cast<uint8_t>(mode & 0xFF);
            owner_.petReactRef()   = static_cast<uint8_t>((mode >> 8) & 0xFF);
            const bool nowAttacking = owner_.petCommandRef() == kCommandAttack;
            if (nowAttacking != wasAttacking && owner_.addonEventCallbackRef()) {
                owner_.addonEventCallbackRef()(
                    nowAttacking ? "PET_ATTACK_START" : "PET_ATTACK_STOP", {});
            }
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
        }
    }

    // Stop Auto Attack On Target Change, which the panel offers and nothing
    // read. Changing target left the swing running - correct for the setting's
    // default, and the only behaviour available whatever it said.
    //
    // Only when the target actually changes, and only when something is being
    // swung at: re-selecting the same unit is not a change, and stopping an
    // attack nobody started would send a cancel for nothing.
    if (guid != owner_.getTargetGuid() && autoAttacking_ &&
        addons::storedCVarValue("stopAutoAttackOnTargetChange", "0") != "0") {
        stopAutoAttack();
    }

    owner_.setTargetGuidRaw(guid);

    // Clear stale aura data from the previous target so the buff bar shows
    // an empty state until the server sends SMSG_AURA_UPDATE_ALL for the new target.
    if (owner_.getSpellHandler()) owner_.getSpellHandler()->clearTargetAuras();

    // Clear previous target's cast bar on target change
    // (the new target's cast state is naturally fetched from spellHandler_->unitCastStates_ by GUID)

    // Inform server of target selection
    if (owner_.isInWorld()) {
        auto packet = SetSelectionPacket::build(guid);
        owner_.getSocket()->send(packet);
    }

    if (guid != 0) {
        LOG_INFO("Target set: 0x", std::hex, guid, std::dec);
    }
    owner_.fireAddonEvent("PLAYER_TARGET_CHANGED", {});

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
    // 0x00524bf0 with none: the target let go is the last target.
    owner_.lastTargetGuidRef() = unit_target::lastTargetAfter(owner_.getTargetGuid(), 0, owner_.lastTargetGuidRef());
    if (owner_.getTargetGuid() != 0) {
        LOG_INFO("Target cleared");
        // Zero the GUID before firing the event so callbacks/addons that query
        // the current target see null (consistent with setTarget which updates
        // targetGuid before the event).
        owner_.setTargetGuidRaw(0);
        owner_.fireAddonEvent("PLAYER_TARGET_CHANGED", {});
    } else {
        owner_.setTargetGuidRaw(0);
    }
    owner_.tabCycleIndexRef() = -1;
    owner_.tabCycleStaleRef() = true;
}

void CombatHandler::forgetUnit(uint64_t guid) {
    if (guid == 0) return;
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
        if (!unit || unit->getHealth() == 0) return false;
        // 0x00518e40: one the player may attack (0x00729740).
        return owner_.unitMayAttack(owner_.getPlayerGuid(), guid);
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
        if (!unit || unit->getHealth() == 0) return false;
        // 0x00518e40: one the player may attack (0x00729740).
        return owner_.unitMayAttack(owner_.getPlayerGuid(), guid);
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
        // 0x00518e40: one the player may attack (0x00729740).
        return owner_.unitMayAttack(owner_.getPlayerGuid(), guid);
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
        return;
    }

    auto target = getTarget();
    if (!target) {
        owner_.raiseUiError(interfaceString("ERR_GENERIC_NO_TARGET", "You have no target."));
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
        LOG_INFO("Sent CMSG_REPOP_REQUEST (Release Spirit)");
        network::Packet cq(wireOpcode(Opcode::MSG_CORPSE_QUERY));
        owner_.getSocket()->send(cq);
    }
}

bool CombatHandler::canReclaimCorpse() const {
    if (!owner_.releasedSpiritRef() || owner_.corpseGuidRef() == 0 ||
        !owner_.corpsePositionValidRef()) return false;
    if (owner_.currentMapIdRef() != owner_.corpseMapIdRef()) return false;
    float dx = owner_.movementInfoRef().x - owner_.corpseYRef();
    float dy = owner_.movementInfoRef().y - owner_.corpseXRef();
    float dz = owner_.movementInfoRef().z - owner_.corpseZRef();
    return (dx*dx + dy*dy + dz*dz) <= (40.0f * 40.0f);
}

float CombatHandler::getCorpseReclaimDelaySec() const {
    if (owner_.corpseReclaimAvailableMsRef() == 0) return 0.0f;
    auto nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (nowMs >= owner_.corpseReclaimAvailableMsRef()) return 0.0f;
    return static_cast<float>(owner_.corpseReclaimAvailableMsRef() - nowMs) / 1000.0f;
}

void CombatHandler::reclaimCorpse() {
    if (!canReclaimCorpse() || !owner_.getSocket()) return;
    if (owner_.corpseGuidRef() == 0) {
        LOG_WARNING("reclaimCorpse: corpse GUID not yet known (corpse object not received); cannot reclaim");
        return;
    }
    auto packet = ReclaimCorpsePacket::build(owner_.corpseGuidRef());
    owner_.getSocket()->send(packet);
    LOG_INFO("Sent CMSG_RECLAIM_CORPSE for corpse guid=0x", std::hex, owner_.corpseGuidRef(), std::dec);
}

void CombatHandler::useSelfRes() {
    if (!owner_.selfResAvailableRef() || !owner_.getSocket()) return;
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

void CombatHandler::handleXpGain(network::Packet& packet) {
    XpGainData data;
    if (!XpGainParser::parse(packet, data)) return;

    addCombatText(CombatTextEntry::XP_GAIN, static_cast<int32_t>(data.totalXp), 0, true);

    // As 0x0050c720 words it, as CHAT_MSG_COMBAT_XP_GAIN: the player's own
    // guid as the victim is COMBATLOG_XPLOSS_FIRSTPERSON_UNNAMED; a victim
    // that is a unit this client has, COMBATLOG_XPGAIN_FIRSTPERSON with its
    // name; anything else COMBATLOG_XPGAIN_FIRSTPERSON_UNNAMED (0x0050b1d0).
    // This was English.
    //
    // Not given: the rested-bonus forms (COMBATLOG_XPGAIN_EXHAUSTION*, which
    // need the experience before the bonus that the parser does not keep,
    // and the rest state's name) and the group and raid ones.
    const std::string xp = std::to_string(data.totalXp);
    std::string msg;
    if (data.victimGuid != 0 && data.victimGuid == owner_.getPlayerGuid()) {
        msg = interfaceFormat("COMBATLOG_XPLOSS_FIRSTPERSON_UNNAMED", "You lose %d experience.", {xp});
    } else {
        std::string victimName;
        if (data.victimGuid != 0) {
            auto victim = owner_.getEntityManager().getEntity(data.victimGuid);
            if (victim && (victim->getType() == ObjectType::UNIT || victim->getType() == ObjectType::PLAYER))
                victimName = owner_.lookupName(data.victimGuid);
        }
        msg = victimName.empty()
            ? interfaceFormat("COMBATLOG_XPGAIN_FIRSTPERSON_UNNAMED", "You gain %d experience.", {xp})
            : interfaceFormat("COMBATLOG_XPGAIN_FIRSTPERSON", "%s dies, you gain %d experience.",
                              {victimName, xp});
    }
    owner_.addLocalChatLine(ChatType::COMBAT_XP_GAIN, msg);
}

} // namespace game
} // namespace wowee
