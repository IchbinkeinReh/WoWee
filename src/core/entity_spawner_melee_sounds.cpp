// A melee swing's sounds, timed by its attacker's model and picked by the
// client's tables (rendering/melee_sound.hpp).
#include "core/entity_spawner.hpp"

#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "game/update_field_table.hpp"
#include "game/visible_item_fields.hpp"   // visibleItemEntryField
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/m2_event_sounds.hpp"
#include "rendering/melee_sound.hpp"
#include "rendering/renderer.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/combat_sound_manager.hpp"
#include "core/weapon_attachment.hpp"

#include <algorithm>
#include <chrono>

namespace wowee::core {

namespace {

namespace ms = rendering::melee_sound;

/// Item.dbc (3.3.5a): 1 ClassID, 2 SubclassID, 3 Sound_override_subclassid,
/// 4 Material.
constexpr uint32_t kItemClass = 1, kItemSubclass = 2, kItemSoundOverride = 3, kItemMaterial = 4;
/// ItemSubClass.dbc: 0 ClassID, 1 SubClassID, 4 Flags (0x4 the unarmed
/// row, 0x00634e00), 9 WeaponSwingSize.
constexpr uint32_t kSubClassFlags = 4, kSubClassSwingSize = 9;
/// The equipment slots a player's visible items stand in (mainhand 15,
/// offhand 16) and its chest (4).
constexpr uint32_t kSlotMainHand = 15, kSlotChest = 4;
/// CreatureSoundData CreatureImpactType (+0x64); CreatureDisplayInfo
/// ModelID and SoundID; CreatureModelData SoundID.
constexpr uint32_t kSoundImpactType = 25;

uint32_t steadyMs() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

/// xorshift32: the hold's draw (the client's generator is 0x00464580; only
/// the spread it gives matters).
uint32_t nextMeleeRandom(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

}  // namespace

void EntitySpawner::loadMeleeSoundTables() {
    if (meleeTablesLoaded_ || !assetManager_) return;
    meleeTablesLoaded_ = true;
    meleeTables_ = std::make_shared<ms::Tables>();
    if (auto impacts = assetManager_->loadDBCOptional("WeaponImpactSounds.dbc");
        impacts && impacts->getFieldCount() >= 23) {
        // ID, WeaponSubClassID, ParrySoundType, ImpactSoundID[10],
        // CritImpactSoundID[10] (0x004d0380).
        for (uint32_t i = 0; i < impacts->getRecordCount(); ++i) {
            std::array<uint32_t, 10> normal{}, crit{};
            for (uint32_t c = 0; c < 10; ++c) {
                normal[c] = impacts->getUInt32(i, 3 + c);
                crit[c] = impacts->getUInt32(i, 13 + c);
            }
            meleeTables_->addImpact(impacts->getUInt32(i, 1), impacts->getUInt32(i, 2), normal, crit);
        }
    }
    if (auto swings = assetManager_->loadDBCOptional("WeaponSwingSounds2.dbc"); swings && swings->getFieldCount() >= 4) {
        // ID, SwingType, Crit, SoundID (0x004d04e0).
        for (uint32_t i = 0; i < swings->getRecordCount(); ++i)
            meleeTables_->addSwing(swings->getUInt32(i, 1), swings->getUInt32(i, 2) != 0, swings->getUInt32(i, 3));
    }
    if (auto subs = assetManager_->loadDBCOptional("ItemSubClass.dbc"); subs && subs->getFieldCount() > kSubClassSwingSize) {
        for (uint32_t i = 0; i < subs->getRecordCount(); ++i) {
            if (subs->getUInt32(i, 0) != ms::kClassWeapon) continue;
            const uint32_t sub = subs->getUInt32(i, 1);
            meleeSwingSizes_[sub] = subs->getUInt32(i, kSubClassSwingSize);
            if ((subs->getUInt32(i, kSubClassFlags) & 0x4u) != 0) meleeUnarmedSubclass_ = sub;
        }
    }
    if (auto materials = assetManager_->loadDBCOptional("Material.dbc"); materials && materials->getFieldCount() >= 2) {
        for (uint32_t i = 0; i < materials->getRecordCount(); ++i)
            meleeMaterialFlags_[materials->getUInt32(i, 0)] = materials->getUInt32(i, 1);
    }
}

std::optional<ms::Item> EntitySpawner::meleeItem(uint32_t entry) const {
    if (entry == 0) return std::nullopt;
    if (auto items = assetManager_ ? assetManager_->loadDBCOptional("Item.dbc") : nullptr;
        items && items->getFieldCount() > kItemMaterial) {
        if (const int32_t row = items->findRecordById(entry); row >= 0) {
            const auto r = static_cast<uint32_t>(row);
            return ms::Item{items->getUInt32(r, kItemClass), items->getUInt32(r, kItemSubclass),
                            static_cast<int32_t>(items->getUInt32(r, kItemSoundOverride)),
                            items->getUInt32(r, kItemMaterial)};
        }
    }
    // A template the server knows and the table does not (0x00758d30 reads
    // the item cache first).
    if (const auto* info = gameHandler_->getItemInfo(entry); info && info->valid)
        return ms::Item{info->itemClass, info->subClass, -1, info->material};
    return std::nullopt;
}

std::optional<ms::Item> EntitySpawner::heldItem(const game::Entity& entity, uint32_t hand) const {
    // Its virtual item info (vfunc +0x12c): a player's visible item, a
    // creature's virtual item slot - none in a hand disarmed (0x0071f440,
    // UNIT_FLAG_DISARMED, UNIT_FLAG2_DISARM_OFFHAND).
    uint32_t entry = 0;
    if (entity.getType() == game::ObjectType::PLAYER) {
        // The client asks for its visible-item **record** here (0x006de360), and
        // that record holds the entry's magnitude - a flagged field, most often
        // a negative one, is the same item to everything that reads the record.
        const int base = gameHandler_->visibleItemEntryBaseRef();
        if (base >= 0) entry = game::visibleItemEntryField(entity.getField(static_cast<uint16_t>(base + static_cast<int>((kSlotMainHand + hand) * 2))));
    } else {
        const uint16_t slot = game::fieldIndex(game::UF::UNIT_VIRTUAL_ITEM_SLOT_ID);
        if (slot != 0xFFFF) entry = entity.getField(static_cast<uint16_t>(slot + hand));
    }
    auto item = meleeItem(entry);
    if (!item) return std::nullopt;
    const uint16_t flags = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS);
    const uint16_t flags2 = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS_2);
    if (hand == 0 && flags != 0xFFFF && (entity.getField(flags) & kUnitFlagDisarmed) != 0 &&
        item->itemClass == ms::kClassWeapon) {
        return std::nullopt;
    }
    if (hand == 1 && flags2 != 0xFFFF && (entity.getField(flags2) & kUnitFlag2DisarmOffhand) != 0) return std::nullopt;
    return item;
}

bool EntitySpawner::metalMaterial(const std::optional<ms::Item>& item) const {
    if (!item) return false;
    const auto it = meleeMaterialFlags_.find(item->material);
    return it != meleeMaterialFlags_.end() && (it->second & 1u) != 0;
}

std::optional<glm::vec3> EntitySpawner::unitSoundPosition(uint64_t guid) const {
    if (!renderer_) return std::nullopt;
    if (gameHandler_ && guid == gameHandler_->getPlayerGuid()) return renderer_->getCharacterPosition();
    rendering::CharacterRenderer* cr = renderer_->getCharacterRenderer();
    if (!cr) return std::nullopt;
    for (const auto* instances : {&creatureInstances_, &playerInstances_}) {
        if (const auto it = instances->find(guid); it != instances->end()) {
            glm::vec3 at;
            if (cr->getInstancePosition(it->second, at)) return at;
        }
    }
    return std::nullopt;
}

float EntitySpawner::combatVolume() const {
    // The combat slider over the effects volume the play is heard at, as
    // the footfalls' slider is.
    audio::AudioCoordinator* ac = renderer_ ? renderer_->getAudioCoordinator() : nullptr;
    audio::CombatSoundManager* combat = ac ? ac->getCombatSoundManager() : nullptr;
    const float effects = audio::AudioEngine::instance().getEffectsVolume();
    if (!combat || !(effects > 0.0f)) return 1.0f;
    return std::clamp(combat->getVolumeScale() / effects, 0.0f, 1.0f);
}

int EntitySpawner::meleePriority(const ms::Hit& hit, MeleeParty party) const {
    // A play the active mover (DAT_00ca1238) or the active player
    // (0x004d3790) is attacker or target of keeps its voice over a crowd's:
    // the caller works out which and hands 0x004d0720, 0x004d0660, 0x004d0540
    // or 0x004d0850 the answer as its last argument, which asks 0x6e.
    if (!gameHandler_) return audio::voice_priority::kDefault;
    const uint64_t own = party == MeleeParty::ActivePlayer ? gameHandler_->getPlayerGuid() : gameHandler_->activeMover();
    return own != 0 && (hit.attacker == own || hit.target == own) ? audio::voice_priority::kActiveMover
                                                                  : audio::voice_priority::kDefault;
}

void EntitySpawner::onMeleeHit(const game::AttackerStateUpdateData& data) {
    if (!gameHandler_) return;
    ms::Hit hit{.attacker = data.attackerGuid,
                .target = data.targetGuid,
                .hitInfo = data.hitInfo,
                .damage = data.totalDamage,
                .victimState = data.victimState,
                .meleeSpellId = data.meleeSpellId,
                .blocked = data.blocked};
    auto& entities = gameHandler_->getEntityManager();
    // 0x00756800 case 0x14a: an attacker not in the world - the hit lands
    // on its target at once; else the one it kept is settled and this one
    // kept for its model.
    if (!entities.getEntity(hit.attacker)) {
        if (entities.getEntity(hit.target)) landMeleeHit(hit);
        return;
    }
    settleKeptMeleeHit(hit.attacker);
    pendingMeleeHits_[hit.attacker] = hit;
    // 0x00755130: the attacker's exertion, by its slot's chance - a
    // creature's; a player's voice is the ActivitySoundManager's.
    if (const auto attacker = entities.getEntity(hit.attacker);
        attacker && attacker->getType() == game::ObjectType::UNIT) {
        const auto& unit = static_cast<const game::Unit&>(*attacker);
        if (ms::exertsOnArrival(hit, unit.getHealth() > 0)) playUnitSlot(unit, ms::exertionSlot(hit), false, nullptr);
    }
    // Units gone from the world take what was kept of them along.
    if (pendingMeleeHits_.size() > 256)
        std::erase_if(pendingMeleeHits_, [&](const auto& kept) { return !entities.getEntity(kept.first); });
}

void EntitySpawner::onMeleeAttackStop(uint64_t attacker) { settleKeptMeleeHit(attacker); }

void EntitySpawner::settleKeptMeleeHit(uint64_t attacker) {
    // 0x00756180: with a target, the impact or the miss - not the target's
    // part - and the hit let go.
    const auto it = pendingMeleeHits_.find(attacker);
    if (it == pendingMeleeHits_.end()) return;
    const ms::Hit hit = it->second;
    pendingMeleeHits_.erase(it);
    if (hit.target != 0 && gameHandler_ && gameHandler_->getEntityManager().getEntity(hit.target)) settleMeleeHit(hit);
}

void EntitySpawner::settleMeleeHit(const ms::Hit& hit) {
    // 0x00755a60.
    loadMeleeSoundTables();
    if (!renderer_ || !meleeTables_) return;
    auto& entities = gameHandler_->getEntityManager();
    const auto attacker = entities.getEntity(hit.attacker);
    const auto target = entities.getEntity(hit.target);
    rendering::M2EventSounds& player = renderer_->getM2EventSounds();
    const float volume = combatVolume();
    switch (ms::settle(hit)) {
    case ms::Settle::None:
        return;
    case ms::Settle::Miss: {
        // The attacker's main hand's whoosh (0x004d0850), where it is.
        if (!attacker || !attacker->isUnit()) return;
        const auto at = unitSoundPosition(hit.attacker);
        if (!at) return;
        const char* name = ms::twoHanded(heldItem(*attacker, 0)) ? "(DONOTRENAME)Combat Miss 2H"
                                                                 : "(DONOTRENAME)Combat Miss 1H";
        // 0x00755a60 asks of the active player.
        player.playOnce(player.idByName(name, assetManager_), &*at, volume, assetManager_, audio::PlayLimit::None,
                        false, meleePriority(hit, MeleeParty::ActivePlayer));
        return;
    }
    case ms::Settle::Impact: {
        // 0x004d0660: the attacker's weapon (in the hand that swung) on the
        // target's column, where the target is.
        if (!attacker || !attacker->isUnit() || !target || !target->isUnit()) return;
        const auto at = unitSoundPosition(hit.target);
        if (!at) return;
        auto weapon = heldItem(*attacker, (hit.hitInfo & ms::kHitOffhand) != 0 ? 1u : 0u);
        if (weapon && weapon->itemClass != ms::kClassWeapon) weapon.reset();
        const uint32_t sound =
            meleeTables_->impact(ms::impactSubclass(weapon, meleeUnarmedSubclass_), impactColumn(*target),
                                 metalMaterial(weapon), (hit.hitInfo & ms::kHitCrit) != 0);
        if (sound != 0)
            player.playOnce(sound, &*at, volume, assetManager_, audio::PlayLimit::WeaponImpact, false,
                            meleePriority(hit, MeleeParty::ActivePlayer));
        return;
    }
    }
}

uint32_t EntitySpawner::impactColumn(const game::Entity& target) const {
    if (target.getType() == game::ObjectType::PLAYER) {
        // 0x007634f0: the chest's material - of the player's own, whose
        // items it has; another's are not to be had, and are flesh.
        if (target.getGuid() != gameHandler_->getPlayerGuid()) return 0;
        const int base = gameHandler_->visibleItemEntryBaseRef();
        if (base < 0) return 0;
        const auto chest = meleeItem(game::visibleItemEntryField(target.getField(static_cast<uint16_t>(base + static_cast<int>(kSlotChest * 2)))));
        if (!chest || chest->itemClass != ms::kClassArmor) return 0;
        const auto it = meleeMaterialFlags_.find(chest->material);
        return ms::playerImpactColumn(it != meleeMaterialFlags_.end() ? it->second : 0u);
    }
    // 0x007463e0: its CreatureSoundData's CreatureImpactType.
    const auto& unit = static_cast<const game::Unit&>(target);
    auto displays = assetManager_->loadDBCOptional("CreatureDisplayInfo.dbc");
    auto models = assetManager_->loadDBCOptional("CreatureModelData.dbc");
    auto sounds = assetManager_->loadDBCOptional("CreatureSoundData.dbc");
    if (!displays || !sounds || sounds->getFieldCount() <= kSoundImpactType) return 0;
    const int32_t display = displays->findRecordById(unit.getDisplayId());
    if (display < 0) return 0;
    uint32_t soundId = displays->getUInt32(static_cast<uint32_t>(display), 2);
    if (soundId == 0 && models && models->getFieldCount() > 13) {
        const int32_t model = models->findRecordById(displays->getUInt32(static_cast<uint32_t>(display), 1));
        if (model >= 0) soundId = models->getUInt32(static_cast<uint32_t>(model), 13);
    }
    const int32_t row = soundId != 0 ? sounds->findRecordById(soundId) : -1;
    return row >= 0 ? ms::creatureImpactColumn(sounds->getUInt32(static_cast<uint32_t>(row), kSoundImpactType)) : 0;
}

std::optional<ms::Item> EntitySpawner::defenderItem(const game::Entity& unit, bool block) const {
    // 0x00754ef0: the main hand's item, unless blocking or it holds
    // something other than a weapon - then the off hand's weapon or shield.
    std::optional<ms::Item> item = heldItem(unit, 0);
    if (block || (item && item->itemClass != ms::kClassWeapon)) {
        item = heldItem(unit, 1);
        if (item && item->itemClass != ms::kClassWeapon && item->itemClass != ms::kClassArmor) item.reset();
    }
    return item;
}

bool EntitySpawner::playsNoWound(const game::Entity& unit) const {
    // 0x00722180.
    const uint64_t guid = unit.getGuid();
    const bool self = guid == gameHandler_->getPlayerGuid();
    // 0x007571c0 on its vehicle kit (+0xf5c, with a Vehicle row): a
    // VehicleSeat animation on its whole model (+0x58 bit 26), or Flags
    // 0x10000 while the cast it shows animates it (+0xa38 0x400) or a kit's
    // animation it was told to hold plays (+0xfa4) - which only a type 0 kit
    // sets (0x0073b140 with 0x00745230's flag 0x80), and only DanceMoves
    // plays those (0x006e18d0, SMSG_PLAY_DANCE), which this client does not.
    bool vehicleBusy = false;
    if (const auto* vehicle = gameHandler_->vehicleRoster().vehicleOf(guid)) {
        const auto* shown = gameHandler_->castVisuals().state(guid);
        const bool castAnimates = shown && shown->precastAnimating;
        vehicleBusy = vehicleWholeModelAnimating(guid) ||
                      ((vehicle->flags & ms::kVehicleFlagCastNoWound) != 0 && castAnimates);
    }
    std::optional<uint32_t> typeFlags;
    if (unit.getType() == game::ObjectType::UNIT) {
        const auto& info = gameHandler_->getCreatureInfoCache();
        if (const auto it = info.find(static_cast<const game::Unit&>(unit).getEntry()); it != info.end())
            typeFlags = it->second.typeFlags;
    }
    uint32_t animation = 0;
    if (renderer_) {
        if (rendering::CharacterRenderer* cr = renderer_->getCharacterRenderer()) {
            uint32_t instance = 0;
            if (self) {
                instance = renderer_->getCharacterInstanceId();
            } else {
                for (const auto* instances : {&creatureInstances_, &playerInstances_}) {
                    if (const auto it = instances->find(guid); it != instances->end()) instance = it->second;
                }
            }
            float timeMs = 0.0f, durationMs = 0.0f;
            if (instance != 0 && !cr->getAnimationState(instance, animation, timeMs, durationMs)) animation = 0;
        }
    }
    return ms::noWound(vehicleBusy, typeFlags, animation);
}

void EntitySpawner::landMeleeHit(const ms::Hit& hit) {
    // 0x00755e40 on the target: nothing of one already lootable
    // (UNIT_DYNAMIC_FLAGS 0x1); else the settling, then its own part.
    auto& entities = gameHandler_->getEntityManager();
    const auto target = entities.getEntity(hit.target);
    if (!target || !target->isUnit()) return;
    if ((static_cast<const game::Unit&>(*target).getDynamicFlags() & game::UNIT_DYNFLAG_LOOTABLE) != 0) return;
    settleMeleeHit(hit);
    if (!renderer_ || !meleeTables_) return;
    // A blow landing on a target that plays no wound animation holds off
    // the next such target's reaction for three to five seconds, and is
    // silent while one is held (0x00722180, 0x00ca1600).
    if (playsNoWound(*target)) {
        const uint32_t now = steadyMs();
        if (meleeReactionHeld_ && static_cast<int32_t>(now - meleeReactionUntilMs_) <= 0) return;
        meleeReactionHeld_ = true;
        meleeReactionUntilMs_ = now + ms::reactionHoldMs(nextMeleeRandom(meleeRandom_));
    }
    const auto at = unitSoundPosition(hit.target);
    if (!at) return;
    const glm::vec3 above = *at + glm::vec3(0.0f, 0.0f, 2.0f);
    rendering::M2EventSounds& player = renderer_->getM2EventSounds();
    const float volume = combatVolume();
    const bool crit = (hit.hitInfo & ms::kHitCrit) != 0;
    switch (ms::defend(hit)) {
    case ms::Defend::Parry:
    case ms::Defend::Block: {
        // 0x00746540 -> 0x004d0540: the attacker's weapon on what the
        // target defends with (0x00754ef0) - nothing at all where it has
        // nothing to.
        const auto attacker = entities.getEntity(hit.attacker);
        std::optional<ms::Item> weapon;
        if (attacker && attacker->isUnit()) {
            weapon = heldItem(*attacker, (hit.hitInfo & ms::kHitOffhand) != 0 ? 1u : 0u);
            if (weapon && weapon->itemClass != ms::kClassWeapon) weapon.reset();
        }
        const std::optional<ms::Item> guard = defenderItem(*target, ms::defend(hit) == ms::Defend::Block);
        if (!guard) break;
        const uint32_t sound = meleeTables_->impact(ms::impactSubclass(weapon, meleeUnarmedSubclass_),
                                                    ms::defenceColumn(guard, metalMaterial(guard)),
                                                    metalMaterial(weapon), crit);
        if (sound != 0)
            player.playOnce(sound, &above, volume, assetManager_, audio::PlayLimit::WeaponImpact, false,
                            meleePriority(hit, MeleeParty::ActiveMover));
        break;
    }
    case ms::Defend::Deflect:
        player.playOnce(player.idByName("(DONOTRENAME)ShieldWoodImpact", assetManager_), &above, volume,
                        assetManager_);
        break;
    case ms::Defend::None:
        break;
    }
    if (ms::absorbs(hit)) {
        player.playOnce(player.idByName("(DONOTRENAME)AbsorbGetHit", assetManager_), &above, volume, assetManager_);
    } else if (const auto slot = ms::injurySlot(hit); slot && target->getType() == game::ObjectType::UNIT) {
        // A creature's injury, by its slot's chance; a player's voice is
        // the ActivitySoundManager's.
        playUnitSlot(static_cast<const game::Unit&>(*target), *slot, false, nullptr);
    }
}

void EntitySpawner::landWithNothingKept(uint64_t guid, const glm::vec3& at) {
    // 0x00756240 with no hit kept and no target.
    if (!gameHandler_ || !renderer_ || !assetManager_) return;
    const auto entity = gameHandler_->getEntityManager().getEntity(guid);
    if (!entity || !entity->isUnit()) return;
    const auto& unit = static_cast<const game::Unit&>(*entity);
    rendering::M2EventSounds& player = renderer_->getM2EventSounds();
    if (entity->getType() != game::ObjectType::PLAYER) {
        // A creature's Exertion (0x0071a3f0's row), outright.
        const int32_t row = unitSoundRow(unit, true);
        auto sounds = assetManager_->loadDBCOptional("CreatureSoundData.dbc");
        if (row < 0 || !sounds) return;
        const uint32_t sound = sounds->getUInt32(static_cast<uint32_t>(row), ms::kSoundExertionColumn);
        if (sound != 0) player.playOnce(sound, &at, 1.0f, assetManager_);
        return;
    }
    // A player's (0x00763570): the AnimEventSoundID of the cast it shows
    // (+0xa60), a yard over it.
    const auto* shown = gameHandler_->castVisuals().state(guid);
    if (!shown || shown->spellId == 0) return;
    const uint32_t visualId = gameHandler_->getSpellVisualId(shown->spellId);
    auto visuals = assetManager_->loadDBCOptional("SpellVisual.dbc");
    if (visualId == 0 || !visuals || visuals->getFieldCount() <= ms::kSpellVisualAnimEventSound) return;
    const int32_t row = visuals->findRecordById(visualId);
    if (row < 0) return;
    const uint32_t sound = visuals->getUInt32(static_cast<uint32_t>(row), ms::kSpellVisualAnimEventSound);
    const auto where = unitSoundPosition(guid);
    if (sound == 0 || !where) return;
    const glm::vec3 above = *where + glm::vec3(0.0f, 0.0f, ms::kAnimEventSoundHeight);
    player.playOnce(sound, &above, 1.0f, assetManager_);
}

void EntitySpawner::onSpellMiss(uint64_t caster, uint64_t target, uint8_t missType, uint32_t spellId) {
    // 0x0080e1b0: only a spell that lands at once, without AttributesEx2
    // 0x80.
    const ms::SpellDefend defend = ms::spellDefend(missType);
    if (defend == ms::SpellDefend::None || !gameHandler_ || !renderer_) return;
    gameHandler_->loadSpellNameCache();
    const auto& spells = gameHandler_->spellNameCacheRef();
    const auto spell = spells.find(spellId);
    if (spell == spells.end() || !ms::spellMissSounds(spell->second.missileSpeed, spell->second.attrEx2)) return;
    // 0x00755850 on the target: not one feigning death or dead.
    auto& entities = gameHandler_->getEntityManager();
    const auto victim = entities.getEntity(target);
    if (!victim || !victim->isUnit()) return;
    const uint16_t flags2 = game::fieldIndex(game::UF::UNIT_FIELD_FLAGS_2);
    if (flags2 != 0xFFFF && (victim->getField(flags2) & 0x1u) != 0) return;
    if (static_cast<const game::Unit&>(*victim).getStandState() == 7) return;
    loadMeleeSoundTables();
    if (!meleeTables_) return;
    const auto at = unitSoundPosition(target);
    if (!at) return;
    const glm::vec3 above = *at + glm::vec3(0.0f, 0.0f, 2.0f);
    std::optional<ms::Item> weapon;
    std::optional<ms::Item> guard;
    if (defend == ms::SpellDefend::Parry) {
        // The caster's main-hand weapon, while its weapons are out of their
        // sheaths (+0xb5c), on what the target parries with.
        if (const auto attacker = entities.getEntity(caster); attacker && attacker->isUnit()) {
            uint32_t sheath = 0;
            if (caster == gameHandler_->getPlayerGuid()) {
                const uint16_t bytes2 = game::fieldIndex(game::UF::UNIT_FIELD_BYTES_2);
                if (bytes2 != 0xFFFF) sheath = attacker->getField(bytes2) & 0xFFu;
            } else if (const auto state = unitSheathState(caster)) {
                sheath = static_cast<uint32_t>(*state);
            }
            if (sheath != 0) {
                weapon = heldItem(*attacker, 0);
                if (weapon && weapon->itemClass != ms::kClassWeapon) weapon.reset();
            }
        }
        guard = defenderItem(*victim, false);
    } else {
        // A bare hand on the off hand's weapon or shield.
        guard = heldItem(*victim, 1);
        if (guard && guard->itemClass != ms::kClassWeapon && guard->itemClass != ms::kClassArmor) guard.reset();
    }
    // 0x004d0540, with nothing to defend with on the wooden weapon column.
    const uint32_t sound = meleeTables_->impact(ms::impactSubclass(weapon, meleeUnarmedSubclass_),
                                                ms::defenceColumn(guard, metalMaterial(guard)),
                                                metalMaterial(weapon), false);
    if (sound != 0) {
        // 0x00755850 hands 0x004d0540, as its last argument, whether the
        // caster is the active mover (DAT_00ca1238): pushed ahead of the
        // position for 0x2c to leave behind (0x007558e8..0x00755969).
        const uint64_t mover = gameHandler_->activeMover();
        renderer_->getM2EventSounds().playOnce(sound, &above, combatVolume(), assetManager_,
                                               audio::PlayLimit::WeaponImpact, false,
                                               mover != 0 && caster == mover ? audio::voice_priority::kActiveMover
                                                                             : audio::voice_priority::kDefault);
    }
}

void EntitySpawner::meleeModelEvent(uint64_t unitGuid, uint32_t eventId, const glm::vec3& at) {
    // 0x00756240: the swing's whoosh at $CSS; the blow landing at $CAH and
    // $AH0..$AH3.
    const auto it = pendingMeleeHits_.find(unitGuid);
    if (it == pendingMeleeHits_.end()) {
        if (ms::isLandEvent(eventId)) landWithNothingKept(unitGuid, at);
        return;
    }
    if (eventId == ms::kEventSwing) {
        const ms::Hit& hit = it->second;
        if (!ms::swingSounds(hit) || !gameHandler_) return;
        const auto attacker = gameHandler_->getEntityManager().getEntity(hit.attacker);
        if (!attacker || !attacker->isUnit()) return;
        loadMeleeSoundTables();
        if (!meleeTables_) return;
        // 0x00746360: nothing in the hand swings light; a weapon by its
        // subclass's WeaponSwingSize; anything else not at all.
        uint32_t size = 0;
        if (const auto held = heldItem(*attacker, (hit.hitInfo & ms::kHitOffhand) != 0 ? 1u : 0u)) {
            if (held->itemClass != ms::kClassWeapon) return;
            const auto sz = meleeSwingSizes_.find(held->subClass);
            size = sz != meleeSwingSizes_.end() ? sz->second : 0u;
        }
        const uint32_t sound = meleeTables_->swing(size, (hit.hitInfo & ms::kHitCrit) != 0);
        // 0x004d0720: its own volume - full, half for a miss.
        const float volume = ((hit.hitInfo & ms::kHitMiss) != 0 ? 0.5f : 1.0f) * combatVolume();
        if (sound != 0)
            renderer_->getM2EventSounds().playOnce(sound, &at, volume, assetManager_, audio::PlayLimit::WeaponSwing,
                                                   true, meleePriority(hit, MeleeParty::ActiveMover));
        return;
    }
    if (!ms::isLandEvent(eventId)) return;
    const ms::Hit hit = it->second;
    pendingMeleeHits_.erase(it);
    // $AH0..$AH3 of a hit that did damage: the attacker's custom attack
    // sound first (its own row, not its pet row).
    if (const uint32_t column = ms::customAttackColumn(eventId); column != 0 && hit.damage != 0 && gameHandler_) {
        const auto attacker = gameHandler_->getEntityManager().getEntity(hit.attacker);
        auto sounds = assetManager_ ? assetManager_->loadDBCOptional("CreatureSoundData.dbc") : nullptr;
        if (attacker && attacker->isUnit() && sounds && renderer_) {
            const int32_t row = unitSoundRow(static_cast<const game::Unit&>(*attacker), false);
            const uint32_t sound = row >= 0 ? sounds->getUInt32(static_cast<uint32_t>(row), column) : 0u;
            if (sound != 0)
                renderer_->getM2EventSounds().playOnce(sound, &at, 1.0f, assetManager_, audio::PlayLimit::WeaponImpact);
        }
    }
    if (hit.target != 0 && gameHandler_ && gameHandler_->getEntityManager().getEntity(hit.target)) landMeleeHit(hit);
}

}  // namespace wowee::core
