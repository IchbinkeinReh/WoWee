#include "game/combat_log_feed.hpp"

#include <cmath>
#include <ctime>

#include <glm/glm.hpp>

#include "addons/lua_api_registrations.hpp"
#include "core/app_clock.hpp"
#include "game/game_handler.hpp"
#include "game/interface_text.hpp"
#include "game/spell_handler.hpp"
#include "game/spell_missiles_in_flight.hpp"
#include "game/world_packets.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"

namespace wowee::game {

namespace cl = combat_log;

namespace {

int64_t clientNowMs() { return static_cast<int64_t>(core::appTimeSeconds() * 1000.0); }

uint64_t guidField(const Entity& e, UF low) {
    const uint16_t idx = fieldIndex(low);
    if (idx == 0xFFFF) return 0;
    return e.getField(idx) | (static_cast<uint64_t>(e.getField(static_cast<uint16_t>(idx + 1))) << 32);
}
uint32_t plainField(const Entity& e, UF f) {
    const uint16_t idx = fieldIndex(f);
    return idx != 0xFFFF ? e.getField(idx) : 0u;
}

/// 0x00af5220: what a power's amounts are shown divided by.
int32_t powerDivisor(uint32_t powerType) {
    static constexpr int32_t kDivisors[7] = {1, 10, 1, 1, 1000, 1, 10};
    return powerType < 7 ? kDivisors[powerType] : 1;
}

/// 0x00a2d394: the school each environmental type's damage is of.
uint32_t environmentalSchool(uint32_t type) {
    static constexpr uint32_t kSchoolBit[7] = {0, 0, 0, 2, 3, 2, 5};
    return 1u << (type < 7 ? kSchoolBit[type] : 0u);
}

/// 0x00577370: to the nearest, halves away from zero.
int32_t roundHalfAway(float v) { return static_cast<int32_t>(std::lround(v)); }

}  // namespace

/// What the builders read of a Spell.dbc row.
struct CombatLogFeed::SpellRow {
    uint32_t id = 0;
    std::string name;
    uint32_t attr = 0, attrEx2 = 0, attrEx3 = 0, attrEx4 = 0, attrEx6 = 0, attrEx7 = 0;
    uint32_t effects[3] = {0, 0, 0};
    uint32_t stackAmount = 0;
    float speed = 0.0f;

    /// The test nearly every builder makes: not hidden from the client or
    /// from its combat log (Attributes 0x80 | 0x100), and named.
    [[nodiscard]] bool shown() const { return (attr & 0x180u) == 0 && !name.empty(); }
    /// 0x0074d3f0: a cast the log leaves out of SPELL_CAST_START/_SUCCESS.
    [[nodiscard]] bool castHidden() const {
        if (!shown()) return true;
        for (uint32_t e : effects)
            if (e == 36) return true;  // SPELL_EFFECT_LEARN_SPELL
        return (attrEx4 & 1u) != 0;
    }
    /// 0x0074d3d0: SPELL_EFFECT_OPEN_LOCK.
    [[nodiscard]] bool opensLock() const {
        for (uint32_t e : effects)
            if (e == 33) return true;
        return false;
    }
};

CombatLogFeed::CombatLogFeed(GameHandler& owner) : owner_(owner) {
    cl::Log::Hooks hooks;
    hooks.nowMs = clientNowMs;
    hooks.retentionSeconds = [] {
        return std::atoi(addons::storedCVarValue("combatLogRetentionTime", "300").c_str());
    };
    hooks.hidesGuid = [this](uint64_t guid) {
        // 0x0074d210: the creature's template, only if it is already known.
        const auto& cache = owner_.getCreatureInfoCache();
        auto it = cache.find(cl::guidEntry(guid));
        return it != cache.end() && (it->second.typeFlags & 0x4000u) != 0;
    };
    hooks.announce = [this](const cl::Entry& entry, bool filtered) {
        if (owner_.combatLogEventCallbackRef()) owner_.combatLogEventCallbackRef()(argsOf(entry), filtered);
    };
    log_.setHooks(std::move(hooks));
    log_.reset(clientNowMs(), static_cast<int64_t>(std::time(nullptr)));
}

void CombatLogFeed::enterWorld() {
    log_.reset(clientNowMs(), static_cast<int64_t>(std::time(nullptr)));
    pendingMisses_.clear();
    replayDelay_ = 0;
}

std::vector<cl::Arg> CombatLogFeed::argsOf(const cl::Entry& entry) const {
    const cl::SpellLookup spells = [this](uint32_t id) -> std::optional<cl::SpellInfo> {
        owner_.loadSpellNameCache();
        auto it = owner_.spellNameCacheRef().find(id);
        if (it == owner_.spellNameCacheRef().end()) return std::nullopt;
        return cl::SpellInfo{.name = it->second.name, .school = it->second.schoolMask};
    };
    return cl::entryArgs(entry, log_.timestampOf(entry), spells);
}

std::optional<CombatLogFeed::SpellRow> CombatLogFeed::spell(uint32_t spellId) const {
    if (spellId == 0) return std::nullopt;
    owner_.loadSpellNameCache();
    auto it = owner_.spellNameCacheRef().find(spellId);
    if (it == owner_.spellNameCacheRef().end()) return std::nullopt;
    const auto& s = it->second;
    SpellRow r;
    r.id = spellId;
    r.name = s.name;
    r.attr = s.attr;
    r.attrEx2 = s.attrEx2;
    r.attrEx3 = s.attrEx3;
    r.attrEx4 = s.attrEx4;
    r.attrEx6 = s.attrEx6;
    r.attrEx7 = s.attrEx7;
    for (int i = 0; i < 3; ++i) r.effects[i] = s.effectIds[i];
    r.stackAmount = s.stackAmount;
    r.speed = s.missileSpeed;
    return r;
}

bool CombatLogFeed::periodicByAttribute(uint32_t spellId) const {
    const auto rec = spell(spellId);
    return rec && (rec->attrEx3 & 0x02000000u) != 0;
}

std::string CombatLogFeed::unknownName() const { return interfaceString("UNKNOWNOBJECT", "Unknown"); }

// ---- UnitWorld ----

uint64_t CombatLogFeed::activePlayer() const { return owner_.getPlayerGuid(); }
uint64_t CombatLogFeed::target() const { return owner_.getTargetGuid(); }
uint64_t CombatLogFeed::focus() const { return owner_.getFocusGuid(); }

bool CombatLogFeed::inRaid() const {
    return owner_.isInGroup() && groupIsRaid(owner_.getPartyData().groupType);
}

bool CombatLogFeed::hasPartyMembers() const {
    if (!owner_.isInGroup()) return false;
    const auto& pd = owner_.getPartyData();
    for (const auto& m : pd.members) {
        if (!groupIsRaid(pd.groupType) || m.subGroup == pd.subGroup) return true;
    }
    return false;
}

bool CombatLogFeed::partyMember(uint64_t guid) const {
    if (guid == 0) return false;
    if (guid == owner_.getPlayerGuid()) return true;
    if (!owner_.isInGroup()) return false;
    const auto& pd = owner_.getPartyData();
    for (const auto& m : pd.members) {
        if (m.guid == guid && (!groupIsRaid(pd.groupType) || m.subGroup == pd.subGroup)) return true;
    }
    return false;
}

bool CombatLogFeed::raidMember(uint64_t guid) const {
    if (guid == 0 || !inRaid()) return false;
    if (guid == owner_.getPlayerGuid()) return true;
    for (const auto& m : owner_.getPartyData().members)
        if (m.guid == guid) return true;
    return false;
}

uint32_t CombatLogFeed::memberFlags(uint64_t guid, bool /*raid*/) const {
    if (!owner_.isInGroup()) return 0;
    const auto& pd = owner_.getPartyData();
    if (guid == owner_.getPlayerGuid()) return pd.flags;
    for (const auto& m : pd.members)
        if (m.guid == guid) return m.flags;
    return 0;
}

uint32_t CombatLogFeed::raidTarget(uint64_t guid) const {
    const uint8_t icon = owner_.getEntityRaidMark(guid);
    return icon < 8 ? icon : 8u;
}

cl::UnitWorld::Object CombatLogFeed::object(uint64_t guid) const {
    Object o;
    const auto entity = owner_.getEntityManager().getEntity(guid);
    if (!entity) return o;
    if (entity->isUnit()) {
        o.kind = Object::Kind::Unit;
        o.charmedBy = guidField(*entity, UF::UNIT_FIELD_CHARMEDBY);
        o.summonedBy = guidField(*entity, UF::UNIT_FIELD_SUMMONEDBY_LO);
        o.createdBy = guidField(*entity, UF::UNIT_FIELD_CREATEDBY);
    } else if (entity->getType() == ObjectType::GAMEOBJECT) {
        o.kind = Object::Kind::GameObject;
        o.createdBy = guidField(*entity, UF::GAMEOBJECT_FIELD_CREATED_BY);
    } else {
        o.kind = Object::Kind::Other;
    }
    return o;
}

std::optional<int> CombatLogFeed::reactionToPlayer(uint64_t unit) const {
    auto& em = owner_.getEntityManager();
    const auto entity = em.getEntity(unit);
    if (!entity || !entity->isUnit() || !em.getEntity(owner_.getPlayerGuid())) return std::nullopt;
    return owner_.unitReactionToPlayer(static_cast<const Unit&>(*entity)) - 1;
}

// ---- Names ----

void CombatLogFeed::nameOf(cl::Party& side) {
    const uint64_t guid = side.guid;
    std::optional<cl::NameKey> key;
    const uint32_t kind = cl::guidKind(guid);
    if (cl::isPlayerGuid(guid)) {
        key = cl::NameKey{.kind = cl::NameKey::Kind::Player, .id = guid};
    } else if (kind == cl::kGuidCreature || kind == cl::kGuidVehicle) {
        // A pet the unit says it is, by its name; otherwise its template's.
        const auto entity = owner_.getEntityManager().getEntity(guid);
        if (entity && entity->isUnit() && plainField(*entity, UF::UNIT_FIELD_PETNUMBER) != 0)
            key = cl::NameKey{.kind = cl::NameKey::Kind::Pet, .id = guid};
        else if (entity)
            key = cl::NameKey{.kind = cl::NameKey::Kind::Creature, .id = plainField(*entity, UF::OBJECT_FIELD_ENTRY)};
        else
            key = cl::NameKey{.kind = cl::NameKey::Kind::Creature, .id = cl::guidEntry(guid)};
    } else if (kind == cl::kGuidPet) {
        key = cl::NameKey{.kind = cl::NameKey::Kind::Pet, .id = guid};
    } else if (kind == cl::kGuidGameObject) {
        key = cl::NameKey{.kind = cl::NameKey::Kind::GameObject, .id = cl::guidEntry(guid)};
    }
    if (!key) {
        side.name = unknownName();
        return;
    }

    // Asked for only in the world; out of it there is nobody to answer.
    const bool canAsk = owner_.isInWorld();
    switch (key->kind) {
        case cl::NameKey::Kind::Player: {
            const auto& cache = owner_.getPlayerNameCache();
            if (auto it = cache.find(guid); it != cache.end()) {
                side.name = it->second;
                side.realm = owner_.getCachedPlayerRealm(guid);
                return;
            }
            if (canAsk) owner_.queryPlayerName(guid);
            break;
        }
        case cl::NameKey::Kind::Pet: {
            const auto entity = owner_.getEntityManager().getEntity(guid);
            if (entity && entity->isUnit()) {
                const auto& name = static_cast<const Unit&>(*entity).getName();
                if (!name.empty()) {
                    side.name = name;
                    return;
                }
            }
            if (canAsk && owner_.getSpellHandler()) owner_.getSpellHandler()->requestPetName(guid);
            break;
        }
        case cl::NameKey::Kind::Creature: {
            const auto entry = static_cast<uint32_t>(key->id);
            const auto& cache = owner_.getCreatureInfoCache();
            if (auto it = cache.find(entry); it != cache.end()) {
                side.name = it->second.name;
                return;
            }
            if (canAsk) owner_.queryCreatureInfo(entry, guid);
            break;
        }
        case cl::NameKey::Kind::GameObject: {
            const auto entry = static_cast<uint32_t>(key->id);
            if (const auto* info = owner_.getCachedGameObjectInfo(entry)) {
                side.name = info->name;
                return;
            }
            if (canAsk) owner_.queryGameObjectInfo(entry, guid);
            break;
        }
        case cl::NameKey::Kind::Item:
            break;
    }
    if (!canAsk) {
        side.name = unknownName();
        return;
    }
    side.waiting = key;
}

void CombatLogFeed::itemNameOf(cl::Entry& entry, uint32_t itemId) {
    // 0x00750210: the item's name, asked for when it is not known.
    entry.suffix |= cl::suffix::kItem;
    entry.itemId = itemId;
    if (const auto* info = owner_.getItemInfo(itemId)) {
        entry.itemName = info->name;
        return;
    }
    if (!owner_.isInWorld()) {
        entry.itemName = unknownName();
        return;
    }
    owner_.ensureItemInfo(itemId);
    entry.itemWaiting = cl::NameKey{.kind = cl::NameKey::Kind::Item, .id = itemId};
}

void CombatLogFeed::setSide(cl::Party& side, uint64_t guid) {
    side.guid = guid;
    nameOf(side);
    side.flags = cl::unitFlags(*this, guid);
}

cl::Entry& CombatLogFeed::create(cl::Event event, uint64_t src, uint64_t dst, const SpellRow* rec) {
    cl::Entry& e = log_.create(event);
    // AttributesEx4 0x2000: a spell the log names no caster for.
    if (src != 0 && (rec == nullptr || (rec->attrEx4 & 0x2000u) == 0)) setSide(e.src, src);
    if (dst != 0) setSide(e.dst, dst);
    if (rec) {
        e.spellId = rec->id;
        e.filterSpellName = rec->name;
    }
    return e;
}

void CombatLogFeed::commit(uint32_t delayMs) { log_.commit(delayMs); }

void CombatLogFeed::playerNameAnswered(uint64_t guid, const std::string& name, const std::string& realm,
                                       bool found) {
    log_.answer({.kind = cl::NameKey::Kind::Player, .id = guid}, found ? name : unknownName(),
                found ? realm : std::string{});
}
void CombatLogFeed::creatureAnswered(uint32_t entry, const std::string& name, bool found) {
    log_.answer({.kind = cl::NameKey::Kind::Creature, .id = entry}, found ? name : unknownName(), {});
}
void CombatLogFeed::gameObjectAnswered(uint32_t entry, const std::string& name, bool found) {
    log_.answer({.kind = cl::NameKey::Kind::GameObject, .id = entry}, found ? name : unknownName(), {});
}
void CombatLogFeed::petNameAnswered(uint64_t guid, const std::string& name, bool found) {
    log_.answer({.kind = cl::NameKey::Kind::Pet, .id = guid}, found ? name : unknownName(), {});
}
void CombatLogFeed::itemAnswered(uint32_t entry, const std::string& name, bool found) {
    log_.answer({.kind = cl::NameKey::Kind::Item, .id = entry}, found ? name : unknownName(), {});
}

// ---- The builders ----

void CombatLogFeed::swing(const AttackerStateUpdateData& d) {
    // 0x00754fd0: a swing that is a spell's is logged only when it hit.
    if (d.meleeSpellId != 0 && d.victimState != 1) return;
    // The first two sub-damages with a school, summed.
    int32_t absorbed = 0, resisted = 0;
    for (size_t i = 0; i < d.subDamages.size() && i < 2; ++i) {
        if (d.subDamages[i].schoolMask == 0) break;
        absorbed += static_cast<int32_t>(d.subDamages[i].absorbed);
        resisted += static_cast<int32_t>(d.subDamages[i].resisted);
    }
    if ((d.victimState == 1 || d.victimState == 5) && d.totalDamage != 0) {
        cl::Entry& e = create(cl::Event::SwingDamage, d.attackerGuid, d.targetGuid, nullptr);
        e.suffix |= cl::suffix::kDamage;
        e.damage.amount = d.totalDamage;
        e.damage.overkill = d.overkill;
        e.damage.school = d.subDamages.empty() ? 0u : d.subDamages.front().schoolMask;
        e.damage.resisted = resisted;
        e.damage.blocked = static_cast<int32_t>(d.blocked);
        e.damage.absorbed = absorbed;
        e.damage.critical = (d.hitInfo & 0x200u) != 0;
        e.damage.glancing = (d.hitInfo & 0x10000u) != 0;
        e.damage.crushing = (d.hitInfo & 0x20000u) != 0;
        commit();
        return;
    }
    uint32_t missType;
    if ((d.hitInfo & 0x10u) != 0) missType = cl::miss::kMiss;
    else if ((d.hitInfo & 0x20u) != 0) missType = cl::miss::kAbsorb;
    else if ((d.hitInfo & 0x80u) != 0) missType = cl::miss::kResist;
    else if (d.victimState == 2) missType = cl::miss::kDodge;
    else if (d.victimState == 3) missType = cl::miss::kParry;
    else if (d.victimState == 5) missType = cl::miss::kBlock;
    else if (d.victimState == 6) missType = cl::miss::kEvade;
    else if (d.victimState == 7) missType = cl::miss::kImmune;
    else if (d.victimState == 8) missType = cl::miss::kDeflect;
    else return;
    cl::Entry& e = create(cl::Event::SwingMissed, d.attackerGuid, d.targetGuid, nullptr);
    e.suffix |= cl::suffix::kMissType;
    e.missType = missType;
    if (missType == cl::miss::kAbsorb) {
        e.suffix |= cl::suffix::kMissAbsorbed;
        e.missAbsorbed = absorbed;
    } else if (missType == cl::miss::kResist) {
        e.suffix |= cl::suffix::kMissResisted;
        e.missResisted = resisted;
    } else if (missType == cl::miss::kBlock) {
        e.suffix |= cl::suffix::kMissBlocked;
        e.missBlocked = static_cast<int32_t>(d.blocked);
    }
    commit();
}

void CombatLogFeed::environmental(uint64_t victim, uint32_t type, int32_t damage, int32_t absorbed,
                                  int32_t resisted) {
    cl::Entry& e = create(cl::Event::EnvironmentalDamage, 0, victim, nullptr);
    e.suffix |= cl::suffix::kEnvironmental | cl::suffix::kDamage;
    e.environmentalType = type;
    e.damage.amount = damage;
    e.damage.school = environmentalSchool(type);
    e.damage.resisted = resisted;
    e.damage.absorbed = absorbed;
    commit();
}

void CombatLogFeed::spellMissed(uint64_t src, uint64_t dst, uint32_t spellId, uint32_t missInfo,
                                bool damageShield, bool periodic) {
    if (missInfo == 0) return;
    const auto rec = spell(spellId);
    if (!rec || !rec->shown()) return;
    cl::Event event;
    if (periodic) event = cl::Event::SpellPeriodicMissed;
    else if (damageShield) event = cl::Event::DamageShieldMissed;
    else event = (rec->attrEx3 & 0x8000u) != 0 ? cl::Event::RangeMissed : cl::Event::SpellMissed;
    cl::Entry& e = create(event, src, dst, &*rec);
    // 0x0074d980, with no amount.
    e.suffix |= cl::suffix::kMissType;
    e.missType = missInfo;
    if (missInfo == cl::miss::kResist) e.suffix |= cl::suffix::kMissResisted;
    else if (missInfo == cl::miss::kBlock) e.suffix |= cl::suffix::kMissBlocked;
    else if (missInfo == cl::miss::kAbsorb) e.suffix |= cl::suffix::kMissAbsorbed;
    commit(replayDelay_);
}

void CombatLogFeed::spellDamage(uint64_t src, uint64_t dst, uint32_t spellId, int32_t damage, int32_t overkill,
                                uint32_t school, int32_t absorbed, int32_t resisted, int32_t blocked,
                                uint32_t hitFlags, bool periodic) {
    const auto rec = spell(spellId);
    if (!rec || !rec->shown()) return;
    const bool ranged = (rec->attrEx3 & 0x8000u) != 0;
    if (damage != 0) {
        const cl::Event event = periodic ? cl::Event::SpellPeriodicDamage
                                         : (ranged ? cl::Event::RangeDamage : cl::Event::SpellDamage);
        cl::Entry& e = create(event, src, dst, &*rec);
        e.suffix |= cl::suffix::kDamage;
        e.damage.amount = damage;
        e.damage.overkill = overkill;
        e.damage.school = school;
        e.damage.resisted = resisted;
        e.damage.blocked = blocked;
        e.damage.absorbed = absorbed;
        e.damage.critical = (hitFlags & 2u) != 0;
        commit(replayDelay_);
        return;
    }
    // Nothing got through: what stopped it, timed now.
    const cl::Event event = periodic ? cl::Event::SpellPeriodicMissed
                                     : (ranged ? cl::Event::RangeMissed : cl::Event::SpellMissed);
    uint32_t missType;
    if (absorbed != 0) missType = cl::miss::kAbsorb;
    else if (blocked != 0) missType = cl::miss::kBlock;
    else if (resisted != 0) missType = cl::miss::kResist;
    else return;
    cl::Entry& e = create(event, src, dst, &*rec);
    e.suffix |= cl::suffix::kMissType;
    e.missType = missType;
    if (missType == cl::miss::kAbsorb) {
        e.suffix |= cl::suffix::kMissAbsorbed;
        e.missAbsorbed = absorbed;
    } else if (missType == cl::miss::kBlock) {
        e.suffix |= cl::suffix::kMissBlocked;
        e.missBlocked = blocked;
    } else {
        e.suffix |= cl::suffix::kMissResisted;
        e.missResisted = resisted;
    }
    commit();
}

void CombatLogFeed::damageSplit(uint64_t src, uint64_t dst, uint32_t spellId, int32_t damage, int32_t overkill,
                                uint32_t school, int32_t absorbed, int32_t resisted, int32_t blocked,
                                uint32_t hitFlags) {
    const auto rec = spell(spellId);
    if (!rec || !rec->shown()) return;
    cl::Entry& e = create(cl::Event::DamageSplit, src, dst, &*rec);
    e.suffix |= cl::suffix::kDamage;
    e.damage.amount = damage;
    e.damage.overkill = overkill;
    e.damage.school = school;
    e.damage.resisted = resisted;
    e.damage.blocked = blocked;
    e.damage.absorbed = absorbed;
    e.damage.critical = (hitFlags & 2u) != 0;
    commit(replayDelay_);
}

void CombatLogFeed::spellHeal(uint64_t src, uint64_t dst, uint32_t spellId, int32_t heal, int32_t overheal,
                              int32_t absorbed, bool critical, bool periodic) {
    if (heal == 0 && overheal == 0 && absorbed == 0) return;
    const auto rec = spell(spellId);
    if (!rec || !rec->shown()) return;
    cl::Entry& e = create(periodic ? cl::Event::SpellPeriodicHeal : cl::Event::SpellHeal, src, dst, &*rec);
    e.suffix |= cl::suffix::kHeal;
    e.heal.amount = heal;
    e.heal.overhealing = overheal;
    e.heal.absorbed = absorbed;
    e.heal.critical = critical;
    commit(replayDelay_);
}

void CombatLogFeed::spellEnergize(uint64_t src, uint64_t dst, uint32_t spellId, uint32_t powerType,
                                  int32_t amount, bool periodic) {
    if (amount == 0) return;
    const auto rec = spell(spellId);
    if (!rec || !rec->shown()) return;
    cl::Entry& e =
        create(periodic ? cl::Event::SpellPeriodicEnergize : cl::Event::SpellEnergize, src, dst, &*rec);
    e.suffix |= cl::suffix::kEnergize;
    e.power.amount = amount / powerDivisor(powerType);
    e.power.powerType = powerType;
    commit(replayDelay_);
}

void CombatLogFeed::spellDrain(uint64_t src, uint64_t dst, uint32_t spellId, uint32_t powerType,
                               uint32_t amount, float multiplier, bool periodic) {
    // Happiness is the pet's, told in chat (0x0050bbf0), not logged.
    if (amount == 0 || powerType == 4) return;
    const auto rec = spell(spellId);
    if (!rec || !rec->shown()) return;
    const int32_t extra = roundHalfAway(static_cast<float>(static_cast<int32_t>(amount)) * multiplier);
    cl::Event event;
    if (extra != 0) event = periodic ? cl::Event::SpellPeriodicLeech : cl::Event::SpellLeech;
    else event = periodic ? cl::Event::SpellPeriodicDrain : cl::Event::SpellDrain;
    cl::Entry& e = create(event, src, dst, &*rec);
    const int32_t div = powerDivisor(powerType);
    e.suffix |= cl::suffix::kDrain;
    e.power.amount = static_cast<int32_t>(amount) / div;
    e.power.powerType = powerType;
    e.power.extraAmount = extra / div;
    commit(replayDelay_);
}

void CombatLogFeed::instakill(uint64_t src, uint64_t dst, uint32_t spellId) {
    const auto rec = spell(spellId);
    if (!rec || !rec->shown()) return;
    create(cl::Event::SpellInstakill, src, dst, &*rec);
    commit();
}

void CombatLogFeed::summon(uint64_t src, uint64_t dst, uint32_t spellId) {
    const auto rec = spell(spellId);
    if (!rec || !rec->shown()) return;
    create(cl::guidKind(dst) == cl::kGuidGameObject ? cl::Event::SpellCreate : cl::Event::SpellSummon, src, dst,
           &*rec);
    commit();
}

void CombatLogFeed::resurrect(uint64_t src, uint64_t dst, uint32_t spellId) {
    const auto rec = spell(spellId);
    if (!rec || !rec->shown()) return;
    create(cl::Event::SpellResurrect, src, dst, &*rec);
    commit();
}

void CombatLogFeed::interrupt(uint64_t src, uint64_t dst, uint32_t spellId, uint32_t interrupted) {
    const auto rec = spell(spellId);
    const auto stopped = spell(interrupted);
    if (!rec || !rec->shown() || !stopped || !stopped->shown()) return;
    cl::Entry& e = create(cl::Event::SpellInterrupt, src, dst, &*rec);
    e.suffix |= cl::suffix::kExtraSpell;
    e.extraSpellId = interrupted;
    commit();
}

void CombatLogFeed::extraAttacks(uint64_t src, uint64_t dst, uint32_t spellId, int32_t count) {
    const auto rec = spell(spellId);
    if (!rec || !rec->shown() || count == 0) return;
    cl::Entry& e = create(cl::Event::SpellExtraAttacks, src, dst, &*rec);
    e.suffix |= cl::suffix::kAmount;
    e.amount = count;
    commit(replayDelay_);
}

void CombatLogFeed::durabilityDamage(uint64_t src, uint64_t dst, uint32_t spellId, uint32_t itemId) {
    const auto rec = spell(spellId);
    if (!rec || !rec->shown()) return;
    cl::Entry& e = create(cl::Event::SpellDurabilityDamage, src, dst, &*rec);
    itemNameOf(e, itemId);
    commit();
}

void CombatLogFeed::durabilityDamageAll(uint64_t src, uint64_t dst, uint32_t spellId) {
    const auto rec = spell(spellId);
    if (!rec || !rec->shown()) return;
    create(cl::Event::SpellDurabilityDamageAll, src, dst, &*rec);
    commit();
}

void CombatLogFeed::aura(uint64_t unit, uint64_t caster, uint32_t spellId, bool debuff, bool applied) {
    // Not on the dead, unless it is put on them; not a passive one, and not
    // one AttributesEx6 0x400 keeps out.
    if (!applied) {
        const auto entity = owner_.getEntityManager().getEntity(unit);
        if (!entity || plainField(*entity, UF::UNIT_FIELD_HEALTH) == 0) return;
    }
    const auto rec = spell(spellId);
    if (!rec || !rec->shown() || (rec->attr & 0x40u) != 0 || (rec->attrEx6 & 0x400u) != 0) return;
    cl::Entry& e = create(applied ? cl::Event::SpellAuraApplied : cl::Event::SpellAuraRemoved, caster, unit, &*rec);
    e.suffix |= cl::suffix::kAuraType;
    e.auraDebuff = debuff;
    commit();
}

void CombatLogFeed::auraDose(uint64_t unit, uint64_t caster, uint32_t spellId, uint32_t before, uint32_t after,
                             bool debuff) {
    const auto rec = spell(spellId);
    if (!rec || !rec->shown() || rec->stackAmount <= 1) return;
    cl::Entry& e = create(after <= before ? cl::Event::SpellAuraRemovedDose : cl::Event::SpellAuraAppliedDose,
                          caster, unit, &*rec);
    e.suffix |= cl::suffix::kAuraType | cl::suffix::kAuraAmount;
    e.auraDebuff = debuff;
    e.auraAmount = static_cast<int32_t>(after);
    commit();
}

void CombatLogFeed::auraRefresh(uint64_t unit, uint64_t caster, uint32_t spellId, bool debuff) {
    const auto rec = spell(spellId);
    if (!rec || !rec->shown() || (rec->attr & 0x40u) != 0 || (rec->attrEx6 & 0x400u) != 0) return;
    cl::Entry& e = create(cl::Event::SpellAuraRefresh, caster, unit, &*rec);
    e.suffix |= cl::suffix::kAuraType;
    e.auraDebuff = debuff;
    commit();
}

void CombatLogFeed::dispel(uint64_t src, uint64_t dst, uint32_t spellId, uint32_t auraSpell, bool debuff,
                           bool stolen) {
    const auto rec = spell(spellId);
    const auto removed = spell(auraSpell);
    if (!rec || !rec->shown() || !removed || !removed->shown()) return;
    cl::Entry& e = create(stolen ? cl::Event::SpellStolen : cl::Event::SpellDispel, src, dst, &*rec);
    e.suffix |= cl::suffix::kExtraSpell | cl::suffix::kAuraType;
    e.extraSpellId = auraSpell;
    e.auraDebuff = debuff;
    commit(replayDelay_);
}

void CombatLogFeed::auraBroken(uint64_t src, uint64_t dst, uint32_t breakerSpell, uint32_t auraSpell,
                               bool debuff) {
    std::optional<SpellRow> breaker;
    if (breakerSpell != 0) {
        breaker = spell(breakerSpell);
        if (!breaker || !breaker->shown()) return;
    }
    const auto rec = spell(auraSpell);
    if (!rec || !rec->shown()) return;
    // The breaking spell can keep its caster out of the log too.
    const uint64_t from = (breaker && (breaker->attrEx4 & 0x2000u) != 0) ? 0 : src;
    cl::Entry& e =
        create(breakerSpell != 0 ? cl::Event::SpellAuraBrokenSpell : cl::Event::SpellAuraBroken, from, dst, &*rec);
    if (breakerSpell != 0) {
        e.suffix |= cl::suffix::kExtraSpell;
        e.extraSpellId = breakerSpell;
    }
    e.suffix |= cl::suffix::kAuraType;
    e.auraDebuff = debuff;
    commit();
}

void CombatLogFeed::damageShield(uint64_t src, uint64_t dst, uint32_t spellId, int32_t damage, int32_t overkill,
                                 uint32_t school) {
    const auto rec = spell(spellId);
    if (!rec || !rec->shown()) return;
    cl::Entry& e = create(cl::Event::DamageShield, src, dst, &*rec);
    e.suffix |= cl::suffix::kDamage;
    e.damage.amount = damage;
    e.damage.overkill = overkill;
    e.damage.school = school;
    commit();
}

void CombatLogFeed::unitDied(uint64_t unit) {
    const auto entity = owner_.getEntityManager().getEntity(unit);
    if (!entity) return;
    const auto& cache = owner_.getCreatureInfoCache();
    const auto info = cache.find(plainField(*entity, UF::OBJECT_FIELD_ENTRY));
    // 0x00718a90: a player always; a creature only once its template is
    // known, and not one whose template says so (type flag 0x400).
    if (entity->getType() != ObjectType::PLAYER &&
        (info == cache.end() || (info->second.typeFlags & 0x400u) != 0))
        return;
    // 0x00752ed0: a gas cloud dissipates. UNIT_DESTROYED is chosen only for a
    // summoning spell its lookup failed to find, which never comes to pass.
    const bool gasCloud = info != cache.end() && info->second.creatureType == 13;
    create(gasCloud ? cl::Event::UnitDissipates : cl::Event::UnitDied, 0, unit, nullptr);
    commit();
}

void CombatLogFeed::buildingDamage(uint64_t src, uint64_t building, uint32_t spellId, int32_t amount) {
    const auto rec = spell(spellId);
    if (!rec || !rec->shown()) return;
    cl::Entry& e = create(cl::Event::SpellBuildingDamage, src, building, &*rec);
    e.suffix |= cl::suffix::kDamage;
    e.damage.amount = amount;
    // The spell's own SchoolMask (+0x284 of the record the builder reads).
    owner_.loadSpellNameCache();
    if (auto it = owner_.spellNameCacheRef().find(spellId); it != owner_.spellNameCacheRef().end())
        e.damage.school = it->second.schoolMask;
    commit();
}

void CombatLogFeed::buildingHeal(uint64_t src, uint64_t building, uint32_t spellId, int32_t amount) {
    const auto rec = spell(spellId);
    if (!rec || !rec->shown()) return;
    cl::Entry& e = create(cl::Event::SpellBuildingHeal, src, building, &*rec);
    e.suffix |= cl::suffix::kHeal;
    e.heal.amount = amount;
    commit();
}

void CombatLogFeed::castStart(uint64_t caster, uint32_t spellId, uint32_t castFlags) {
    if ((castFlags & 1u) != 0) return;  // 0x00806700: not a pending cast's own start
    const auto entity = owner_.getEntityManager().getEntity(caster);
    const auto rec = spell(spellId);
    if (!entity || !entity->isUnit() || !rec || rec->castHidden()) return;
    // Only a cast that takes time starts (the unit's cast time, 0x007ff180).
    if (!owner_.getSpellHandler() || !owner_.getSpellHandler()->hasCastTime(spellId, *entity)) return;
    create(cl::Event::SpellCastStart, caster, 0, &*rec);
    commit();
}

void CombatLogFeed::castGo(const SpellGoData& go) {
    const auto rec = spell(go.spellId);
    auto& em = owner_.getEntityManager();
    // 0x007519e0: a cast with no cast time succeeds; one with a cast time has
    // already started.
    if ((go.castFlags & 1u) == 0 || (rec && (rec->attrEx7 & 0x80000000u) != 0)) {
        const auto entity = em.getEntity(go.casterUnit);
        if (entity && entity->isUnit() && rec && !rec->castHidden() && !rec->opensLock() &&
            owner_.getSpellHandler() && !owner_.getSpellHandler()->hasCastTime(go.spellId, *entity)) {
            create(cl::Event::SpellCastSuccess, go.casterUnit, go.targetGuid, &*rec);
            commit();
        }
    }
    if (go.missTargets.empty()) return;
    // The misses' source: the first guid where it is a unit or a game object,
    // else the caster.
    uint64_t src = go.casterUnit;
    if (const auto first = em.getEntity(go.casterGuid);
        first && (first->isUnit() || first->getType() == ObjectType::GAMEOBJECT))
        src = go.casterGuid;
    // At once for a spell with no missile; otherwise as each one lands
    // (0x00700e20, 0x00702fc0).
    const bool atOnce = rec && rec->speed == 0.0f && (rec->attrEx2 & 0x80u) == 0;
    const auto from = em.getEntity(go.casterUnit);
    for (const SpellGoMissEntry& m : go.missTargets) {
        std::vector<PendingMiss> misses = {{.landsAt = 0, .src = src, .dst = m.targetGuid, .spellId = go.spellId,
                                            .missInfo = m.missType}};
        // A reflected spell that missed its caster on the way back.
        if (m.missType == cl::miss::kReflect && m.reflectResult != 0)
            misses.push_back({.landsAt = 0, .src = src, .dst = go.casterUnit, .spellId = go.spellId,
                              .missInfo = m.reflectResult});
        int64_t flight = 0;
        if (!atOnce && rec && rec->speed > 0.0f && from) {
            if (const auto to = em.getEntity(m.targetGuid)) {
                const glm::vec3 a(from->getX(), from->getY(), from->getZ());
                const glm::vec3 b(to->getX(), to->getY(), to->getZ());
                flight = spell_missiles::flightMs(glm::length(b - a), rec->speed);
            }
        }
        for (PendingMiss& p : misses) {
            if (flight <= 0) {
                spellMissed(p.src, p.dst, p.spellId, p.missInfo, false, false);
            } else {
                p.landsAt = clientNowMs() + flight;
                pendingMisses_.push_back(p);
            }
        }
    }
}

void CombatLogFeed::update() {
    if (pendingMisses_.empty()) return;
    const int64_t now = clientNowMs();
    std::vector<PendingMiss> landed;
    for (auto it = pendingMisses_.begin(); it != pendingMisses_.end();) {
        if (now - it->landsAt >= 0) {
            landed.push_back(*it);
            it = pendingMisses_.erase(it);
        } else {
            ++it;
        }
    }
    for (const PendingMiss& p : landed) spellMissed(p.src, p.dst, p.spellId, p.missInfo, false, false);
}

void CombatLogFeed::castFailed(uint32_t spellId, const std::string& text) {
    if (text.empty()) return;
    const auto rec = spell(spellId);
    if (!rec || !rec->shown()) return;
    cl::Entry& e = create(cl::Event::SpellCastFailed, owner_.getPlayerGuid(), 0, &*rec);
    e.suffix |= cl::suffix::kFailedType;
    e.failedType = text;
    commit();
}

void CombatLogFeed::partyKill(uint64_t killer, uint64_t victim) {
    create(cl::Event::PartyKill, killer, victim, nullptr);
    commit();
}

void CombatLogFeed::procResist(uint64_t src, uint64_t dst, uint32_t spellId) {
    const auto rec = spell(spellId);
    if (!rec || !rec->shown()) return;
    cl::Entry& e = create(cl::Event::SpellMissed, src, dst, &*rec);
    e.suffix |= cl::suffix::kMissType | cl::suffix::kMissResisted;
    e.missType = cl::miss::kResist;
    commit();
}

void CombatLogFeed::dispelFailed(uint64_t src, uint64_t dst, uint32_t spellId, const std::vector<uint32_t>& failed) {
    const auto rec = spell(spellId);
    if (!rec || !rec->shown()) return;
    for (uint32_t held : failed) {
        const auto aura = spell(held);
        if (!aura || !aura->shown()) continue;
        cl::Entry& e = create(cl::Event::SpellDispelFailed, src, dst, &*rec);
        e.suffix |= cl::suffix::kExtraSpell;
        e.extraSpellId = held;
        commit();
    }
}

void CombatLogFeed::enchantment(uint64_t target, uint64_t caster, uint32_t itemId, uint32_t enchantId) {
    // The SpellItemEnchantment row, unless its flag 2 keeps it out.
    auto* am = owner_.services().assetManager;
    if (!am || !am->isInitialized()) return;
    auto dbc = am->loadDBC("SpellItemEnchantment.dbc");
    if (!dbc || !dbc->isLoaded()) return;
    const int32_t row = dbc->findRecordById(enchantId);
    if (row < 0) return;
    const auto* layout = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("SpellItemEnchantment")
                                                        : nullptr;
    const uint32_t flagsField = layout ? layout->tryField("Flags") : 0xFFFFFFFFu;
    if (flagsField < dbc->getFieldCount() && (dbc->getUInt32(static_cast<uint32_t>(row), flagsField) & 2u) != 0)
        return;
    cl::Entry& e = create(caster == 0 ? cl::Event::EnchantRemoved : cl::Event::EnchantApplied, caster, target, nullptr);
    e.suffix |= cl::suffix::kEnchantName;
    e.enchantName = owner_.getEnchantName(enchantId);
    itemNameOf(e, itemId);
    commit();
}

}  // namespace wowee::game
