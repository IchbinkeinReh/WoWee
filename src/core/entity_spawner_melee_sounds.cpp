// A melee swing's sounds, timed by its attacker's model and picked by the
// client's tables (rendering/melee_sound.hpp).
#include "core/entity_spawner.hpp"

#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "game/update_field_table.hpp"
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
/// The creature cache's flag of a target that plays no wound animation.
constexpr uint32_t kTypeFlagNoWound = 0x8u;

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
        const int base = gameHandler_->visibleItemEntryBaseRef();
        if (base >= 0) entry = entity.getField(static_cast<uint16_t>(base + static_cast<int>((kSlotMainHand + hand) * 2)));
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
        player.playOnce(player.idByName(name, assetManager_), &*at, volume, assetManager_);
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
        if (sound != 0) player.playOnce(sound, &*at, volume, assetManager_, audio::PlayLimit::WeaponImpact);
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
        const auto chest = meleeItem(target.getField(static_cast<uint16_t>(base + static_cast<int>(kSlotChest * 2))));
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
    if (target->getType() == game::ObjectType::UNIT) {
        const auto& info = gameHandler_->getCreatureInfoCache();
        const auto it = info.find(static_cast<const game::Unit&>(*target).getEntry());
        if (it != info.end() && (it->second.typeFlags & kTypeFlagNoWound) != 0) {
            const uint32_t now = steadyMs();
            if (meleeReactionHeld_ && static_cast<int32_t>(now - meleeReactionUntilMs_) <= 0) return;
            meleeReactionHeld_ = true;
            meleeReactionUntilMs_ = now + ms::reactionHoldMs(nextMeleeRandom(meleeRandom_));
        }
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
        // 0x00746540 -> 0x004d0540: the attacker's weapon on the target's
        // main-hand weapon (a parry), else its off hand's weapon or shield.
        const auto attacker = entities.getEntity(hit.attacker);
        std::optional<ms::Item> weapon;
        if (attacker && attacker->isUnit()) {
            weapon = heldItem(*attacker, (hit.hitInfo & ms::kHitOffhand) != 0 ? 1u : 0u);
            if (weapon && weapon->itemClass != ms::kClassWeapon) weapon.reset();
        }
        std::optional<ms::Item> guard = heldItem(*target, 0);
        if (ms::defend(hit) == ms::Defend::Block || !guard || guard->itemClass != ms::kClassWeapon) {
            guard = heldItem(*target, 1);
            if (guard && guard->itemClass != ms::kClassWeapon && guard->itemClass != ms::kClassArmor) guard.reset();
        }
        if (!guard) break;
        const uint32_t sound = meleeTables_->impact(ms::impactSubclass(weapon, meleeUnarmedSubclass_),
                                                    ms::defenceColumn(guard, metalMaterial(guard)),
                                                    metalMaterial(weapon), crit);
        if (sound != 0) player.playOnce(sound, &above, volume, assetManager_, audio::PlayLimit::WeaponImpact);
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
    }
}

void EntitySpawner::meleeModelEvent(uint64_t unitGuid, uint32_t eventId, const glm::vec3& at) {
    // 0x00756240: the swing's whoosh at $CSS; the blow landing at $CAH and
    // $AH0..$AH3.
    const auto it = pendingMeleeHits_.find(unitGuid);
    if (it == pendingMeleeHits_.end()) return;
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
                                                   true);
        return;
    }
    if (!ms::isLandEvent(eventId)) return;
    const ms::Hit hit = it->second;
    pendingMeleeHits_.erase(it);
    if (hit.target != 0 && gameHandler_ && gameHandler_->getEntityManager().getEntity(hit.target)) landMeleeHit(hit);
}

}  // namespace wowee::core
