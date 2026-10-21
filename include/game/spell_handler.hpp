#pragma once

#include "game/world_packets.hpp"
#include "game/opcode_table.hpp"
#include "game/spell_defines.hpp"
#include "game/spell_mods.hpp"
#include "game/ground_target.hpp"
#include "game/spell_history.hpp"
#include "game/spell_missiles_in_flight.hpp"
#include "game/cast_checks.hpp"
#include "game/spell_unit_target.hpp"
#include "game/handler_types.hpp"
#include "audio/spell_sound_manager.hpp"
#include "network/packet.hpp"
#include <glm/glm.hpp>
#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace game {

class GameHandler;
class Entity;

class SpellHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    explicit SpellHandler(GameHandler& owner);

    void registerOpcodes(DispatchTable& table);

    // Talent data structures (aliased from handler_types.hpp)
    using TalentEntry = game::TalentEntry;
    using TalentTabEntry = game::TalentTabEntry;

    // --- Spell book tabs ---
    struct SpellBookTab {
        std::string name;
        std::string texture; // icon path
        std::vector<uint32_t> spellIds; // spells in this tab
    };

    // Unit cast state (aliased from handler_types.hpp)
    using UnitCastState = game::UnitCastState;

    // Equipment set info (aliased from handler_types.hpp)
    using EquipmentSetInfo = game::EquipmentSetInfo;

    // --- Public API (delegated from GameHandler) ---
    void castSpell(uint32_t spellId, uint64_t targetGuid = 0);
    /// A pet's spell cast by the pet itself (0x005d4210 -> 0x0080cce0,
    /// CMSG_PET_CAST_SPELL): at a place it waits for one, as the player's do.
    void castPetSpell(uint64_t caster, uint32_t spellId, uint64_t targetGuid);
    /// 0x00729740: whether one unit may attack another, each by guid; false
    /// when either is not a unit here.
    [[nodiscard]] bool unitMayAttack(uint64_t attacker, uint64_t victim) const;
    /// 0x007293d0 with its last argument 0: whether one unit may help another.
    [[nodiscard]] bool unitMayAssist(uint64_t helper, uint64_t unit) const;

    // --- A spell waiting for a place on the ground (Spell_C.cpp) ---
    /// The spell the cursor holds while it waits for a location, or 0
    /// (0x00d3f4e4 with 0x00d3f4e0 & 0x60, 0x007fd750).
    [[nodiscard]] uint32_t groundTargetSpellId() const { return groundTarget_.spellId; }
    [[nodiscard]] bool isGroundTargeting() const { return groundTarget_.spellId != 0; }
    /// The location still wanted: TARGET_FLAG_SOURCE_LOCATION, then
    /// TARGET_FLAG_DEST_LOCATION.
    [[nodiscard]] uint32_t groundTargetRequired() const { return groundTarget_.required; }
    /// Put the spell down without casting it (SpellStopTargeting, 0x0080ac90's
    /// clean-up) - one waiting for a place or for a unit. True when there was
    /// one.
    bool cancelGroundTargeting();

    // --- A spell waiting for a unit to be clicked (Spell_C.cpp) ---
    /// The cursor holds a spell no unit filled (0x0080cce0 setting
    /// 0x00d3f4e4 with a unit still wanted).
    [[nodiscard]] bool isUnitTargeting() const { return unitTarget_.spellId != 0; }
    [[nodiscard]] uint32_t unitTargetSpellId() const { return unitTarget_.spellId; }
    /// A click on a unit, or SpellTargetUnit (0x0080dc00): the held spell
    /// takes it where it fills what is wanted (0x0080bc80) and is cast once
    /// nothing more is; no unit there is SPELL_FAILED_OUT_OF_RANGE.
    void spellTargetUnit(uint64_t unitGuid);
    /// Whether the held spell would take the unit, within its reach
    /// (0x00803a50): the Cast cursor over it, else UnableCast (0x004f8190).
    [[nodiscard]] bool spellCanTargetUnit(uint64_t unitGuid) const;

    /// 0x00804cc0: whether a missile of the cast a dynamic object carries
    /// (its DYNAMICOBJECT_CASTER, SPELLID and CASTTIME) still flies.
    [[nodiscard]] bool isMissileInFlight(uint64_t caster, uint32_t spellId, uint32_t castTime) const {
        return missilesInFlight_.contains(caster, spellId, castTime);
    }
    /// 0x008047a0, as the world goes (0x004fa390).
    void clearMissilesInFlight() { missilesInFlight_.clear(); }
    /// SMSG_NEW_WORLD (0x00403d10 -> 0x00403b70): the spell the cursor holds
    /// is put down (0x00809a60) and both spell histories are emptied
    /// (0x00804af0) - SMSG_INITIAL_SPELLS and SMSG_PET_SPELLS send them again.
    void newWorld();

    // --- The pet's spell history (0x00d3f5ac + 24) ---
    [[nodiscard]] const spell_history::History& petSpellHistory() const { return petHistory_; }
    // --- The player's (0x00d3f5ac) ---
    [[nodiscard]] const spell_history::History& playerSpellHistory() const { return playerHistory_; }
    /// 0x00809000 for the player's spell: the cooldown that runs longest of
    /// its own, its category's and its global cooldown (0x00807980), in
    /// milliseconds of historyNowMs().
    [[nodiscard]] spell_history::History::Cooldown playerCooldown(uint32_t spellId) const {
        return historyCooldown(spellId, 0, false);
    }
    /// 0x00807980 on the player's history, or the pet's: a spell's, cast
    /// from the item `itemId` (whose template may give it its category) or
    /// from none.
    [[nodiscard]] spell_history::History::Cooldown historyCooldown(uint32_t spellId, uint32_t itemId, bool pet) const;
    /// 0x00809030: an item's - its first use spell's (trigger 0), cast from it.
    [[nodiscard]] spell_history::History::Cooldown itemCooldown(uint32_t itemId) const;
    /// The client's clock for the history, in milliseconds (0x0086ae20):
    /// GetTime()'s, a thousandth of a second at a time.
    static int64_t historyNowMs();
    /// 0x00809100 for the pet: secureAbilityToggle on, and the spell's
    /// cooldown began under 1.5 s ago (0x00807d40) - its button does not
    /// turn it off.
    [[nodiscard]] bool petToggleHeld(uint32_t spellId) const;
    /// 0x00805d70: a pet's spell sent as CMSG_PET_ACTION starts its global
    /// cooldown in the pet's history.
    void notePetActionCast(uint32_t spellId, uint64_t pet);
    /// SMSG_PET_CAST_FAILED (0x00806c30 -> 0x00804720).
    void notePetCastFailed(uint32_t spellId);
    /// A click on the world at this place, canonical: fills the location the
    /// spell wants (0x0080c340) and casts once nothing more is wanted.
    void placeGroundTarget(const ground_target::Place& place);
    /// The ground under the cursor this frame, canonical, or none - where a
    /// click would land. Set by the world view, read by
    /// CameraOrSelectOrMoveStop.
    void setGroundTargetCursor(std::optional<ground_target::Place> place) { groundTarget_.cursor = place; }
    [[nodiscard]] std::optional<ground_target::Place> groundTargetCursor() const { return groundTarget_.cursor; }
    /// The game object a held spell summons, whose model the cursor's place
    /// shows (0x0080cce0 -> 0x007fe9e0), 0 for none; and whether it keeps a
    /// fixed facing rather than the player's (effect 81, 0x00d397d4).
    [[nodiscard]] uint32_t groundTargetObjectEntry() const { return groundTarget_.objectEntry; }
    [[nodiscard]] bool groundTargetObjectFixedFacing() const { return groundTarget_.objectFixedFacing; }
    [[nodiscard]] float groundTargetObjectFacing() const { return groundTarget_.objectFacing; }
    /// A right click while a spell waits for a place (0x0051fb00): effect
    /// 81's model turns a quarter (0x007fd800), any other spell is put down
    /// (0x00809a60). True when there was such a spell.
    bool turnOrCancelGroundTargeting();
    /// The circle's radius, and whether the place is in range, for the
    /// cursor's place (0x008019c0, 0x00803ee0).
    [[nodiscard]] float groundTargetSpellArea() const;
    /// 0x007fd970 for a spell of the player's.
    [[nodiscard]] spell_mods::Sum spellModifiers(uint32_t spellId, uint8_t op) const;
    [[nodiscard]] int groundTargetPlacement(const glm::vec3& canonical) const;

    /// Spell.dbc EffectImplicitTargetA, or 0 when the spell is unknown. 21 means
    /// the spell has to be aimed at a friendly unit.
    [[nodiscard]] uint32_t getSpellImplicitTargetA(uint32_t spellId) const;
    /// Whether Spell.dbc has this spell at all.
    ///
    /// A server may cast something the client's own data has never heard of -
    /// a private core's custom item does it routinely - and the answer to
    /// "what does this aim at" is then not zero but unknown. The two are worth
    /// telling apart: zero means the spell says nothing, unknown means we do.
    [[nodiscard]] bool isSpellKnownToClient(uint32_t spellId) const;

    /// The last spell the player cast while on foot. When mounting is detected,
    /// this identifies which of the player's indefinite self-cast auras is the
    /// mount - scanning for one blindly can land on a racial or a tracking buff.
    [[nodiscard]] uint32_t getLastGroundCastSpellId() const { return lastGroundCastSpellId_; }

    /// Record a spell cast by using an item - pre-WotLK mounts are items, and
    /// their on-use spell never passes through castSpell().
    void noteGroundCastSpell(uint32_t spellId) { lastGroundCastSpellId_ = spellId; }
    void cancelCast();
    void cancelAura(uint32_t spellId);

    // Known spells
    [[nodiscard]] const std::unordered_set<uint32_t>& getKnownSpells() const { return knownSpells_; }

    // Cast state
    [[nodiscard]] bool isCasting() const { return casting_ || restorationActive_; }
    [[nodiscard]] bool isChanneling() const { return casting_ ? castIsChannel_ : restorationActive_; }
    [[nodiscard]] bool isRestoring() const { return restorationActive_; }
    [[nodiscard]] bool isGameObjectInteractionCasting() const;
    [[nodiscard]] uint32_t getCurrentCastSpellId() const {
        return casting_ ? currentCastSpellId_ : restorationSpellId_;
    }
    [[nodiscard]] float getCastProgress() const {
        const float total = casting_ ? castTimeTotal_ : restorationTimeTotal_;
        const float remaining = casting_ ? castTimeRemaining_ : restorationTimeRemaining_;
        return total > 0.0f ? (total - remaining) / total : 0.0f;
    }
    [[nodiscard]] float getCastTimeRemaining() const {
        return casting_ ? castTimeRemaining_ : restorationTimeRemaining_;
    }
    [[nodiscard]] float getCastTimeTotal() const {
        return casting_ ? castTimeTotal_ : restorationTimeTotal_;
    }

    // Repeat-craft queue
    void startCraftQueue(uint32_t spellId, int count);
    void cancelCraftQueue();
    [[nodiscard]] int getCraftQueueRemaining() const { return craftQueueRemaining_; }
    [[nodiscard]] uint32_t getCraftQueueSpellId() const { return craftQueueSpellId_; }

    // Crafting window (client-side; opened by casting a profession spell
    // like Cooking or First Aid - see tradeskillOpenerSkillLine)
    [[nodiscard]] bool isCraftingWindowOpen() const { return craftingWindowOpen_; }
    [[nodiscard]] uint32_t getCraftingSkillLine() const { return craftingSkillLine_; }
    /// Opening and closing a profession announce themselves, because the
    /// interface's trade skill panel is driven entirely by these two events -
    /// it hides on TRADE_SKILL_CLOSE and fills itself on TRADE_SKILL_SHOW.
    /// Without them the panel could be complete and still never appear.
    void openCraftingWindow(uint32_t skillLine);
    void closeCraftingWindow();
    // Returns the skill line id if spellId is a tradeskill-window opener
    // (e.g. Cooking → 185) with at least one known recipe, else 0.
    uint32_t tradeskillOpenerSkillLine(uint32_t spellId);

    // SpellFocusObject.dbc name ("Anvil", "Cooking Fire", ...) for
    // requires-spell-focus cast failures; empty if unknown.
    const std::string& getSpellFocusName(uint32_t focusId);

    // TotemCategory.dbc name ("Blacksmith Hammer", "Mining Pick", ...) for
    // totem-category cast failures; empty if unknown.
    const std::string& getTotemCategoryName(uint32_t categoryId);

    // Unit cast state (tracked per GUID for target frame + boss frames)
    [[nodiscard]] const UnitCastState* getUnitCastState(uint64_t guid) const {
        auto it = unitCastStates_.find(guid);
        return (it != unitCastStates_.end() && it->second.casting) ? &it->second : nullptr;
    }
    void clearUnitCastStates() { unitCastStates_.clear(); unitCastEnds_.clear(); }
    void removeUnitCastState(uint64_t guid) { unitCastStates_.erase(guid); unitCastEnds_.erase(guid); }
    [[nodiscard]] const UnitCastEnd* getUnitCastEnd(uint64_t guid) const {
        auto it = unitCastEnds_.find(guid);
        return it != unitCastEnds_.end() ? &it->second : nullptr;
    }

    // Aura cache mutation (formerly accessed via friend)
    void clearUnitAurasCache() { unitAurasCache_.clear(); }
    std::unordered_map<uint64_t, std::vector<AuraSlot>>& unitAurasCacheRef() { return unitAurasCache_; }
    /// Copy a unit's auras into the by-guid list the party frames read.
    void mirrorAurasByGuid(uint64_t guid, const std::vector<AuraSlot>& auras);
    void removeUnitAuraCache(uint64_t guid) { unitAurasCache_.erase(guid); }

    // Known spells mutation (formerly accessed via friend)
    void addKnownSpell(uint32_t spellId) { knownSpells_.insert(spellId); }
    [[nodiscard]] bool hasKnownSpell(uint32_t spellId) const { return knownSpells_.count(spellId) > 0; }
    /// 0x007260e0 || 0x00726160: the spell known, or one SkillLineAbility
    /// supersedes it with, followed down the chain.
    [[nodiscard]] bool knowsSpellOrSuccessor(uint32_t spellId) const;

    // Target aura mutation (formerly accessed via friend)
    void clearTargetAuras() { for (auto& slot : targetAuras_) slot = AuraSlot{}; }

    // Player aura mutation (formerly accessed via friend)
    void resetPlayerAuras(size_t capacity) { playerAuras_.clear(); playerAuras_.resize(capacity); }
    AuraSlot& getPlayerAuraSlotRef(size_t slot) { return playerAuras_[slot]; }
    std::vector<AuraSlot>& getPlayerAurasMut() { return playerAuras_; }

    // Target cast helpers
    [[nodiscard]] bool isTargetCasting() const;
    [[nodiscard]] uint32_t getTargetCastSpellId() const;
    [[nodiscard]] float getTargetCastProgress() const;
    [[nodiscard]] float getTargetCastTimeRemaining() const;
    [[nodiscard]] bool isTargetCastInterruptible() const;

    // Talents
    [[nodiscard]] uint8_t getActiveTalentSpec() const { return activeTalentSpec_; }
    [[nodiscard]] uint8_t getUnspentTalentPoints() const { return unspentTalentPoints_[activeTalentSpec_]; }
    [[nodiscard]] uint8_t getUnspentTalentPoints(uint8_t spec) const { return spec < 2 ? unspentTalentPoints_[spec] : 0; }
    [[nodiscard]] const std::unordered_map<uint32_t, uint8_t>& getLearnedTalents() const { return learnedTalents_[activeTalentSpec_]; }
    [[nodiscard]] const std::unordered_map<uint32_t, uint8_t>& getLearnedTalents(uint8_t spec) const {
        static std::unordered_map<uint32_t, uint8_t> empty;
        return spec < 2 ? learnedTalents_[spec] : empty;
    }

    static constexpr uint8_t MAX_GLYPH_SLOTS = 6;
    [[nodiscard]] const std::array<uint16_t, MAX_GLYPH_SLOTS>& getGlyphs() const { return learnedGlyphs_[activeTalentSpec_]; }
    [[nodiscard]] const std::array<uint16_t, MAX_GLYPH_SLOTS>& getGlyphs(uint8_t spec) const {
        static std::array<uint16_t, MAX_GLYPH_SLOTS> empty{};
        return spec < 2 ? learnedGlyphs_[spec] : empty;
    }
    [[nodiscard]] uint8_t getTalentRank(uint32_t talentId) const {
        auto it = learnedTalents_[activeTalentSpec_].find(talentId);
        return (it != learnedTalents_[activeTalentSpec_].end()) ? it->second : 0;
    }
    void learnTalent(uint32_t talentId, uint32_t requestedRank);
    void switchTalentSpec(uint8_t newSpec);

    // Talent DBC access
    [[nodiscard]] const TalentEntry* getTalentEntry(uint32_t talentId) const {
        auto it = talentCache_.find(talentId);
        return (it != talentCache_.end()) ? &it->second : nullptr;
    }
    [[nodiscard]] const TalentTabEntry* getTalentTabEntry(uint32_t tabId) const {
        auto it = talentTabCache_.find(tabId);
        return (it != talentTabCache_.end()) ? &it->second : nullptr;
    }
    [[nodiscard]] const std::unordered_map<uint32_t, TalentEntry>& getAllTalents() const { return talentCache_; }
    [[nodiscard]] const std::unordered_map<uint32_t, TalentTabEntry>& getAllTalentTabs() const { return talentTabCache_; }

    /// Drops what was read out of Talent.dbc and TalentTab.dbc.
    ///
    /// Called when the active expansion changes, for the same reason as
    /// MovementHandler::resetTaxiDbcCache: GameHandler cleared copies of its
    /// own and the readers come here.
    void resetTalentDbcCache() {
        talentCache_.clear();
        talentTabCache_.clear();
        talentDbcLoaded_ = false;
    }
    void loadTalentDbc();
    void syncPreWotlkTalentsFromKnownSpells();

    // Auras
    [[nodiscard]] const std::vector<AuraSlot>& getPlayerAuras() const { return playerAuras_; }
    [[nodiscard]] const std::vector<AuraSlot>& getTargetAuras() const { return targetAuras_; }
    /// 0x00727e70's bit for `auraType`: an aura of the unit's with an effect
    /// of the type on.
    [[nodiscard]] bool unitHasAuraType(uint64_t guid, uint32_t auraType) const;
    /// 0x00722d80: the unit channels (UNIT_CHANNEL_SPELL) a mana spell that
    /// costs it, without AttributesEx2 0x2000000.
    [[nodiscard]] bool channelCostsMana(const Unit& unit) const;
    /// 0x008017e0: whether `unit` has what `spellId` costs - health, runes,
    /// or the power as it is predicted (0x0071c2e0). A spell not known to
    /// the cache has nothing to be short of.
    [[nodiscard]] bool enoughPower(uint32_t spellId, const Unit& unit) const;
    /// 0x0080b5d0 for the player: whether the spell could be cast now -
    /// `item` the template it is used from, none for a spell's own.
    bool spellUsable(uint32_t spellId, const ItemQueryResponseData* item);
    /// 0x00807da0: an entry of the player's history - the spell's from the
    /// item `itemId` (0 none), or one of `category` - held for an event.
    [[nodiscard]] bool historyHeld(uint32_t spellId, uint32_t itemId, uint32_t category) const;
    /// 0x00809610: nothing where the unit (0: the target) is none the
    /// spell may be aimed at, else whether it is in the caster's reach.
    [[nodiscard]] std::optional<bool> reachAnswer(uint32_t spellId, uint64_t caster, uint64_t unit) const;
    /// 0x00801650 with no unit: the spell's reach from the caster.
    [[nodiscard]] std::optional<unit_target::Range> spellRange(uint32_t spellId, uint64_t caster) const;
    /// The item the running cast was cast from (SMSG_SPELL_START's caster
    /// where it is not the unit), 0 for none (0x00805fc0).
    [[nodiscard]] uint64_t currentCastItemGuid() const { return casting_ ? castItemGuid_ : 0; }
    [[nodiscard]] const std::vector<AuraSlot>* getUnitAuras(uint64_t guid) const {
        auto it = unitAurasCache_.find(guid);
        return (it != unitAurasCache_.end()) ? &it->second : nullptr;
    }

    // Spell book tabs
    const std::vector<SpellBookTab>& getSpellBookTabs();

    // Talent wipe confirm dialog
    [[nodiscard]] bool showTalentWipeConfirmDialog() const { return talentWipePending_; }
    [[nodiscard]] uint32_t getTalentWipeCost() const { return talentWipeCost_; }
    /// The trainer offering the wipe. The confirmation closes itself when the
    /// player walks away from them.
    [[nodiscard]] uint64_t getTalentWipeNpcGuid() const { return talentWipeNpcGuid_; }
    void confirmTalentWipe();
    void cancelTalentWipe() { talentWipePending_ = false; }

    // Pet talent respec confirm
    [[nodiscard]] bool showPetUnlearnDialog() const { return petUnlearnPending_; }
    [[nodiscard]] uint32_t getPetUnlearnCost() const { return petUnlearnCost_; }
    void confirmPetUnlearn();
    void cancelPetUnlearn() { petUnlearnPending_ = false; }

    // Item use

    // Equipment sets - canonical data owned by InventoryHandler;
    // GameHandler::getEquipmentSets() delegates to inventoryHandler_.

    // Pet spells
    void sendPetAction(uint32_t action, uint64_t targetGuid = 0);
    void togglePetSpellAutocast(uint32_t spellId);
    void renamePet(const std::string& newName);

    // Spell DBC accessors
    [[nodiscard]] const int32_t* getSpellEffectBasePoints(uint32_t spellId) const;
    [[nodiscard]] float getSpellDuration(uint32_t spellId) const;
    [[nodiscard]] const std::string& getSpellName(uint32_t spellId) const;
    [[nodiscard]] const std::string& getSpellRank(uint32_t spellId) const;
    [[nodiscard]] const std::string& getSpellDescription(uint32_t spellId) const;
    [[nodiscard]] std::string getEnchantName(uint32_t enchantId) const;
    /// The gem item an enchantment came out of (SpellItemEnchantment.Src_ItemID).
    /// Zero when the enchantment is not a gem, or on a file with no such column.
    /// This is the only route from an enchantment sitting in an item's socket
    /// back to the gem that is in the socket - the item fields carry the
    /// enchantment id and nothing else.
    [[nodiscard]] uint32_t getEnchantGemItem(uint32_t enchantId) const;
    [[nodiscard]] uint8_t getSpellDispelType(uint32_t spellId) const;
    [[nodiscard]] bool isSpellInterruptible(uint32_t spellId) const;
    [[nodiscard]] bool isSpellPassive(uint32_t spellId) const;
    [[nodiscard]] uint32_t getSpellSchoolMask(uint32_t spellId) const;
    /// Spell.dbc Targets mask (SpellCastTargetFlags): 0x10 = TARGET_FLAG_ITEM.
    [[nodiscard]] uint32_t getSpellTargetFlags(uint32_t spellId) const;
    [[nodiscard]] uint32_t getSpellTargetAuraState(uint32_t spellId) const;
    /// Spell.dbc RangeIndex resolved via SpellRange.dbc, in yards. Melee ("Combat
    /// Range") is 5; self-only is 0; negative means SpellRange.dbc was unavailable.
    [[nodiscard]] float getSpellMaxRange(uint32_t spellId) const;
    /// Maps a superseded spell rank to the highest rank we actually know. Returns
    /// spellId unchanged when it is already known, or has no known same-name rank.
    [[nodiscard]] uint32_t resolveHighestKnownRank(uint32_t spellId) const;
    /// The skill's own name, by SkillLine.dbc id.
    [[nodiscard]] const std::string& getSkillLineName(uint32_t skillLineId) const;

    // Cast state
    void stopCasting();
    void resetCastState();
    void resetTalentState();
    // Full per-character reset (spells, cooldowns, auras, cast state, talents).
    // Called from GameHandler::selectCharacter so spell state doesn't bleed between characters.
    void resetAllState();
    void clearUnitCaches();

    // Aura duration
    void handleUpdateAuraDuration(uint8_t slot, uint32_t durationMs);

    // Skill DBC
    void loadSkillLineDbc();
    void extractSkillFields(const FlatFieldMap& fields);
    void extractExploredZoneFields(const FlatFieldMap& fields);

    // Update per-frame timers (call from GameHandler::update)
    void updateTimers(float dt);
    void refreshRestorationState() { refreshRestorationFromPlayerAuras(); }

    // Packet handlers dispatched from GameHandler's opcode table
    void handlePetSpells(network::Packet& packet);
    void handleListStabledPets(network::Packet& packet);

    // Pet stable commands (called via GameHandler delegation)
    void requestStabledPetList();
    void stablePet();
    void unstablePet(uint32_t petNumber);

    // DBC cache loading (called from GameHandler during login)
    void loadSpellNameCache() const;
    void loadSkillLineAbilityDbc();

    /// Ask the server what the pet is called.
    ///
    /// The name a player gave a pet arrives only in answer to
    /// CMSG_PET_NAME_QUERY. Without it a pet wears its creature template's
    /// name - "Voidwalker" where the player wrote something else - which is
    /// what the pet frame, its nameplate and the pet bar's tooltip all show.
    ///
    /// The pet number in that request is a key the server echoes back and does
    /// not look anything up with: SendPetNameQuery finds the pet by guid. So
    /// this sends a number of its own making and uses it to match the reply to
    /// the pet it asked about, which is what the field would have been for.
    void requestPetName(uint64_t petGuid);

    /// Give a hunter's pet up for good. The interface asks first - this is the
    /// other side of the ABANDON_PET dialog, whose accept called an unbound
    /// name and raised.
    /// The five values every UNIT_SPELLCAST_* event carries.
    ///
    /// unit, spell name, rank, cast id, spell id - in that order, which is what
    /// FrameXML unpacks. These were being fired as just the unit and the spell
    /// id, so the id sat where the name belongs and the cast id was absent.
    [[nodiscard]] std::vector<std::string> spellcastArgs(const std::string& unitId,
                                           uint32_t spellId) const;

    void abandonPet();

    /// Buy the next stable slot from the stable master currently open.
    void buyStableSlot();

    /// A cast the client refuses itself, shown as its failure is
    /// (0x00808200): the result's line on the error frame.
    void reportLocalCastFailure(uint32_t spellId, uint8_t result);

private:
    std::unordered_map<uint32_t, uint64_t> pendingPetNameQueries_;
    uint32_t nextPetNameQueryKey_ = 1;

    // --- Packet handlers ---
    void handleInitialSpells(network::Packet& packet);
    void handleCastFailed(network::Packet& packet);
    void handleSpellStart(network::Packet& packet);
    void handleSpellGo(network::Packet& packet);
    /// SMSG_SPELL_UPDATE_CHAIN_TARGETS (0x00800470): a cast's targets
    /// changed while its chains run.
    void handleSpellUpdateChainTargets(network::Packet& packet);
    void handleSpellCooldown(network::Packet& packet);
    void handleCooldownEvent(network::Packet& packet);
    void handleAuraUpdate(network::Packet& packet, bool isAll);
    void handleLearnedSpell(network::Packet& packet);

    void handleCastResult(network::Packet& packet);
    void handleSpellFailedOther(network::Packet& packet);
    void handleClearCooldown(network::Packet& packet);
    void handleModifyCooldown(network::Packet& packet);
    void handlePlaySpellVisual(network::Packet& packet);
    void handleSpellModifier(network::Packet& packet, bool isFlat);
    void handleSpellDelayed(network::Packet& packet);
    void handleSpellLogMiss(network::Packet& packet);
    void handleSpellFailure(network::Packet& packet);
    void handleItemCooldown(network::Packet& packet);
    void handleDispelFailed(network::Packet& packet);
    void handleTotemCreated(network::Packet& packet);
    void handlePeriodicAuraLog(network::Packet& packet);
    void handleSpellEnergizeLog(network::Packet& packet);
    void handleExtraAuraInfo(network::Packet& packet, bool isInit);
    void handleSpellDispelLog(network::Packet& packet);
    void handleSpellStealLog(network::Packet& packet);
    void handleSpellChanceProcLog(network::Packet& packet);
    void handleSpellInstaKillLog(network::Packet& packet);
    void handleSpellLogExecute(network::Packet& packet);
    void handleClearExtraAuraInfo(network::Packet& packet);
    void handleItemEnchantTimeUpdate(network::Packet& packet);
    void handleResumeCastBar(network::Packet& packet);
    void handleChannelStart(network::Packet& packet);
    void handleChannelUpdate(network::Packet& packet);

    // --- Internal helpers ---

    // Resolve the magic school for a spell (for audio playback).
    // Returns MagicSchool from the spell name cache, defaulting to ARCANE.
    audio::SpellSoundManager::MagicSchool resolveSpellSchool(uint32_t spellId);

    // Play a spell cast sound via audioCoordinator, if available.
    void playSpellCastSound(uint32_t spellId);

    // Resolve SpellVisualID from Spell.dbc cache for a given spellId.
    uint32_t resolveSpellVisualId(uint32_t spellId);
    // Resolve render-space position for a unit GUID (player or entity).
    bool resolveUnitPosition(uint64_t guid, glm::vec3& outPos);
    /// A wire point in the frame of the object `guid` names, put in the
    /// world (canonical) by its matrix; nullopt when the object is not known
    /// (0x0074b4c0).
    std::optional<glm::vec3> pointInObjectFrame(uint64_t guid, const glm::vec3& wirePoint);
    /// A SpellCastTargets point in the world, canonical: in the frame of
    /// `frameGuid`, or as it came where that is 0 or not known (0x009ab810).
    glm::vec3 targetPointInWorld(uint64_t frameGuid, float x, float y, float z);
    // Play the cast/precast visual effect at the caster's position.
    void triggerCastVisual(uint32_t spellId, uint64_t casterGuid, uint32_t castTimeMs = 0);
    // Play the impact visual effect at the target's position.
    void triggerImpactVisual(uint32_t spellId, uint64_t targetGuid, uint64_t casterGuid);
    void launchRangedWeaponProjectile(uint32_t spellId, uint64_t targetGuid);
    // Send the spell's missiles, as the client does from SMSG_SPELL_GO.
    // Returns the hit targets whose impact now waits for a missile to land.
    /// flewToPlace: set when a missile flies to the cast's place.
    std::vector<uint64_t> launchSpellMissiles(const SpellGoData& data, uint32_t visualId,
                                              bool* flewToPlace = nullptr);
    void refreshRestorationFromPlayerAuras();
    void stopRestorationPresentation();

    // --- handleSpellLogExecute, per effect logged (extracted to reduce nesting) ---
    void logEffectPowerDrain(const SpellLogExecuteEntry& entry, uint64_t caster, uint32_t spellId,
                             bool isPlayerCaster);
    void logEffectCreateItem(const SpellLogExecuteEntry& entry, uint32_t spellId, bool isPlayerCaster);
    void logEffectInterruptCast(const SpellLogExecuteEntry& entry, uint64_t caster, uint32_t spellId,
                                bool isPlayerCaster);
    void logEffectFeedPet(const SpellLogExecuteEntry& entry, bool isPlayerCaster);

    // Find the on-use spell for an item (trigger=0 Use or trigger=5 NoDelay).
    // CMSG_USE_ITEM requires a valid spellId or the server silently ignores it.
    void handleSupercededSpell(network::Packet& packet);
    void handleRemovedSpell(network::Packet& packet);
    void handleUnlearnSpells(network::Packet& packet);
    void handleTalentsInfo(network::Packet& packet);
    void handleAchievementEarned(network::Packet& packet);

    GameHandler& owner_;

    // --- Spell state ---
    std::unordered_set<uint32_t> knownSpells_;
    uint8_t castCount_ = 0;

    struct GroundTarget {
        uint32_t spellId = 0;
        uint64_t caster = 0;  ///< a pet casting it itself; 0 the player
        uint32_t required = 0;
        glm::vec3 source{0.0f};
        std::optional<ground_target::Place> cursor;
        uint32_t objectEntry = 0;
        bool objectFixedFacing = false;
        float objectFacing = 0.0f;  // 0x00d3f4dc for effect 81, from 0
    };
    GroundTarget groundTarget_;
    /// The spell the cursor holds for a unit, its caster and what it still
    /// wants (0x00d3f4e4, 0x00d3f4e0).
    struct UnitTarget {
        uint32_t spellId = 0;
        uint64_t caster = 0;
        uint32_t required = 0;
    };
    UnitTarget unitTarget_;
    uint64_t castItemGuid_ = 0;
    spell_history::History petHistory_;
    spell_history::History playerHistory_;
    /// 0x00af5274: the casts whose missile still flies.
    spell_missiles::InFlightList missilesInFlight_;
    /// 0x0080e1b0: a cast whose spell has a missile Speed joins the list.
    void noteMissileInFlight(const SpellGoData& data);
    int64_t historyPruneAt_ = 0;  ///< 0x00d3f608: the next prune, on a spell go
    /// 0x00805d70: the global cooldown a cast begins in the caster's history
    /// - StartRecoveryTime through its modifiers, the usual 1.5 s hastened.
    void noteGlobalCooldown(uint32_t spellId, uint64_t caster, spell_history::History& history);
    /// 0x007fef60: CategoryRecoveryTime as the history takes it - for the
    /// player (not `pet`) scaled by the main hand's speed for a category
    /// flagged 0x1, and the ranged attack time added for Attributes 0x2
    /// without AttributesEx2 0x20000; through the Cooldown modifiers (op 11)
    /// unless AttributesEx6 0x80000000.
    int32_t categoryCooldown(uint32_t spellId, bool pet) const;
    /// 0x0053bac0: the player's history changed - ACTIONBAR_UPDATE_COOLDOWN
    /// (0x005a7cc0), then SPELL_UPDATE_COOLDOWN.
    void playerCooldownsChanged();
    /// SpellCategory.dbc's Flags (0x00ad478c), 0 for none.
    uint32_t spellCategoryFlags(uint32_t category) const;
    mutable std::unordered_map<uint32_t, uint32_t> spellCategoryFlags_;
    mutable bool spellCategoriesLoaded_ = false;
    /// A cooldown through the spell's Cooldown modifiers (op 11).
    int32_t moddedCooldown(uint32_t spellId, int32_t ms) const;
    /// 0x0080bc80 for a spell and a unit: whether it fills some of
    /// `required`, and what is left. A unit out of the spell's reach is
    /// refused, said when `say` (0x00803850).
    struct UnitTake {
        bool taken = false;
        uint32_t required = 0;
        uint64_t unit = 0;  ///< the unit taken: the one asked, or its target (0x007ffc40)
        bool outOfReach = false;  ///< one it fills, refused by the reach (0x00803850)
    };
    UnitTake takeUnit(uint32_t spellId, uint64_t caster, uint64_t unitGuid, uint32_t required, bool say) const;
    /// 0x0072aaf0: whether the unit - the one that made it, up its creators -
    /// carries an aura letting the spell go without its aura state or out of
    /// combat (aura 262 matching the spell's class mask, 0x007283a0).
    bool auraLetsSpell(uint64_t unit, uint32_t spellId) const;
    /// The cursor holds the spell for a unit (0x0080cce0).
    void armUnitTarget(uint32_t spellId, uint64_t caster, uint32_t required);
    /// CMSG_PET_CAST_SPELL at a unit or at none (0x0080ac90).
    void sendPetCast(uint32_t spellId, uint64_t caster, uint64_t target);
    /// CMSG_CAST_SPELL from the player at a unit, at none, or at an item
    /// (0x0080ac90).
    void sendPlayerCast(uint32_t spellId, uint64_t target, uint64_t item = 0);
    /// 0x0080c790: the weapon in hand the player's spell with Attributes
    /// 0x200 puts its temporary enchantment on, 0 for none.
    uint64_t enchantWeapon(uint32_t spellId) const;
    /// The player's cast or a pet's, by the caster.
    void sendCastBy(uint32_t spellId, uint64_t caster, uint64_t target);
    /// 0x0080c790 -> 0x0080cce0: the unit the spell goes at, or the cursor
    /// held for one, or the refusal - the player's spells and a pet's alike.
    void castAtUnits(uint32_t spellId, uint64_t caster, uint64_t targetGuid);
    /// 0x0080cce0's checks of the player before the units: the unit asked
    /// held to the reach (0x00809610), then 0x00739650. False, said, where
    /// the cast is refused.
    bool castChecksPass(uint32_t spellId, uint64_t targetGuid);
    /// What 0x00739650 asks of the player for the spell (cast_check::Caster).
    cast_check::Caster casterState(uint32_t spellId, const Entity& player);
    /// 0x008012f0: the spell's power cost to the player.
    int32_t powerCost(uint32_t spellId, const Entity& player, int depth) const;
    /// 0x007ff180 != 0: whether the spell takes time to cast.
    bool hasCastTime(uint32_t spellId, const Entity& player) const;
    /// SMSG_SPELL_START / SMSG_SPELL_GO's cast flag 0x800: the caster's power.
    void castPowerSaid(uint64_t casterUnit, uint32_t spellId, uint32_t power, bool go);
    /// 0x007283a0: an aura of the player's of the type, on, for the spell.
    bool playerAuraAffects(uint32_t auraType, uint32_t spellId) const;
    /// 0x00812410's SupercededBySpell for the player's race and class, 0 for none.
    uint32_t supersededBy(uint32_t spellId) const;
    void loadAreaGroups() const;
    /// ChrClasses SpellClassSet for the player's class (0x00d397b4).
    uint32_t classSpellFamily() const;
    /// 0x008091d0 for the player: the spell's totems, tools and reagents.
    bool toolsCarried(uint32_t spellId, const Entity& player) const;
    /// 0x008093d0 for the player: the weapon it wants, and its ammunition.
    bool weaponCarried(uint32_t spellId, const Entity& player) const;
    /// 0x007ff480 from the caster to the unit (none: a place), at the
    /// friendly row or the hostile.
    unit_target::Range spellReach(uint32_t spellId, const Entity& caster, const Entity* unit, bool friendly) const;
    /// 0x005ee5b0: whether the player's runes pay the spell - one of
    /// PowerType 5 with a SpellRuneCost row, at the cost percentage the
    /// school's power cost modifier and multiplier and the cost modifiers
    /// (op 14) make of 100 - and, where they do not, when they will
    /// (0x005ee7e0).
    struct RuneCost {
        bool paid = true;
        int64_t readyAt = 0;
    };
    RuneCost runeCost(uint32_t spellId) const;
    /// SpellRuneCost.dbc: blood, unholy, frost by row.
    mutable std::unordered_map<uint32_t, std::array<uint32_t, 3>> runeCostRows_;
    mutable bool runeCostRowsLoaded_ = false;
    const std::array<uint32_t, 3>* runeCostRow(uint32_t id) const;
    mutable bool castTimesLoaded_ = false;
    mutable std::unordered_map<uint32_t, std::array<int32_t, 3>> castTimes_;  ///< SpellCastTimes: base, per level, minimum
    mutable bool supersedeLoaded_ = false;
    mutable std::unordered_multimap<uint32_t, std::array<uint32_t, 3>> supersedes_;  ///< spell: race mask, class mask, next
    mutable bool areaGroupsLoaded_ = false;
    mutable std::unordered_map<uint32_t, std::array<uint32_t, 7>> areaGroups_;  ///< six areas, then the next group
    /// ChrClasses SpellClassSet for the player's class (0x008007a0), and
    /// the class it was read for.
    mutable uint32_t classSpellFamily_ = 0;
    mutable uint8_t classSpellFamilyFor_ = 0xFF;
    /// Send the cast at the destination the targeting gathered.
    void castAtLocation(uint32_t spellId, const ground_target::Place& place, uint64_t caster);
    /// 0x0080c790: arms the cursor for a spell that wants a place; false
    /// where it wants none.
    bool armGroundTarget(uint32_t spellId, uint64_t caster);
    /// What SendCast does once the cast is out (0x0080ac90).
    void castSent(uint32_t spellId, uint64_t caster);
    bool casting_ = false;
    bool castIsChannel_ = false;
    uint32_t currentCastSpellId_ = 0;
    float castTimeRemaining_ = 0.0f;
    float castTimeTotal_ = 0.0f;
    bool restorationActive_ = false;
    uint32_t restorationSpellId_ = 0;
    bool restorationIsFood_ = false;
    float restorationTimeRemaining_ = 0.0f;
    float restorationTimeTotal_ = 0.0f;
    float restorationSoundTimer_ = 0.0f; // repeats the consume sound while active

    // Repeat-craft queue
    uint32_t craftQueueSpellId_ = 0;
    int craftQueueRemaining_ = 0;

    // Crafting window
    bool craftingWindowOpen_ = false;
    uint32_t craftingSkillLine_ = 0;

    // SpellFocusObject.dbc names, loaded lazily
    std::unordered_map<uint32_t, std::string> spellFocusNames_;
    bool spellFocusDbcLoaded_ = false;

    // TotemCategory.dbc names, loaded lazily
    std::unordered_map<uint32_t, std::string> totemCategoryNames_;
    bool totemCategoryDbcLoaded_ = false;

    uint32_t lastGroundCastSpellId_ = 0;
    /// When auto-attack was last toggled, for the Ability Toggle guard: a
    /// second press inside a short window is an accident rather than a
    /// decision. See the SPELL_ID_ATTACK branch in castSpell.
    std::chrono::steady_clock::time_point autoAttackToggledAt_{};

    // Per-unit cast state
    std::unordered_map<uint64_t, UnitCastState> unitCastStates_;
    std::unordered_map<uint64_t, UnitCastEnd> unitCastEnds_;
    void noteUnitCastEnd(uint64_t guid, bool channelZeroed) {
        auto& e = unitCastEnds_[guid];
        e.serial += 1;
        e.channelZeroed = channelZeroed;
    }

    // Talents (dual-spec support)
    uint8_t activeTalentSpec_ = 0;
    uint8_t unspentTalentPoints_[2] = {0, 0};
    std::unordered_map<uint32_t, uint8_t> learnedTalents_[2];
    std::array<std::array<uint16_t, MAX_GLYPH_SLOTS>, 2> learnedGlyphs_{};
    std::unordered_map<uint32_t, TalentEntry> talentCache_;
    std::unordered_map<uint32_t, TalentTabEntry> talentTabCache_;
    bool talentDbcLoaded_ = false;

    // Auras
    std::vector<AuraSlot> playerAuras_;
    std::vector<AuraSlot> targetAuras_;
    std::unordered_map<uint64_t, std::vector<AuraSlot>> unitAurasCache_;

    // Spell book tabs
    std::vector<SpellBookTab> spellBookTabs_;
    size_t lastSpellCount_ = 0;
    bool spellBookTabsDirty_ = true;

    // Talent wipe confirm dialog
    bool talentWipePending_ = false;
    uint64_t talentWipeNpcGuid_ = 0;
    uint32_t talentWipeCost_ = 0;

    // Pet talent respec confirm dialog
    bool petUnlearnPending_ = false;
    uint32_t petUnlearnCost_ = 0;
};

} // namespace game
} // namespace wowee
