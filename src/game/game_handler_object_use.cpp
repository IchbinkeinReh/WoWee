// Using a game object the way the client does (GameObject_C.cpp): whether
// it may be used (0x00711470), what its lock asks for (0x0070f160), and the
// use itself (0x007116c0). See game/game_object_use.hpp.

#include "game/game_handler.hpp"
#include "game/game_object_use.hpp"
#include "game/game_object_rotation.hpp"
#include "game/interface_text.hpp"
#include "game/inventory_slots.hpp"
#include "game/packet_parsers.hpp"
#include "game/spell_handler.hpp"
#include "game/spell_mods.hpp"
#include "game/spell_unit_target.hpp"
#include "game/update_field_table.hpp"
#include "addons/lua_api_registrations.hpp"
#include "core/coordinates.hpp"
#include "network/world_socket.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include <cmath>
#include <unordered_map>

namespace wowee {
namespace game {

namespace {

struct ErrorText {
    const char* key;
    const char* enUS;
};

ErrorText errorText(uint16_t id) {
    namespace e = go_use::err;
    switch (id) {
        case e::kPlayerDead: return {"ERR_PLAYER_DEAD", "You can't do that when you're dead."};
        case e::kClientLockedOut: return {"ERR_CLIENT_LOCKED_OUT", "You can't do that right now."};
        case e::kChestInUse: return {"ERR_CHEST_IN_USE", "That is already being used."};
        case e::kCantOpen: return {"ERR_USE_CANT_OPEN", "Can't open that."};
        case e::kUseLocked: return {"ERR_USE_LOCKED", "Locked."};
        case e::kDoorLocked: return {"ERR_DOOR_LOCKED", "The door is locked."};
        case e::kButtonLocked: return {"ERR_BUTTON_LOCKED", "The button is locked."};
        case e::kLockedWithItem: return {"ERR_USE_LOCKED_WITH_ITEM_S", "Requires %s"};
        case e::kLockedWithSpell: return {"ERR_USE_LOCKED_WITH_SPELL_S", "Requires %s"};
        case e::kLockedWithSpellKnown: return {"ERR_USE_LOCKED_WITH_SPELL_KNOWN_SI", "Requires %s %d"};
        case e::kTooFar: return {"ERR_USE_TOO_FAR", "You are too far away."};
        case e::kCantImmune: return {"ERR_USE_CANT_IMMUNE", "You can't do that while you are immune."};
        case e::kGenericStunned: return {"ERR_GENERIC_STUNNED", "You are stunned"};
        case e::kNotWhileMounted: return {"ERR_NOT_WHILE_MOUNTED", "You can't do that while mounted."};
        case e::kNotWhileShapeshifted:
            return {"ERR_NOT_WHILE_SHAPESHIFTED", "You can't do that while shapeshifted."};
        case e::kNotInCombat: return {"ERR_NOT_IN_COMBAT", "You can't do that while in combat"};
        case e::kPreventedByMechanic: return {"ERR_USE_PREVENTED_BY_MECHANIC_S", "Can't use while %s"};
        default: return {"ERR_USE_CANT_OPEN", "Can't open that."};
    }
}

/// Lock.dbc by id (Type 1-8, Index 9-16, Skill 17-24, Action 25-32).
const std::unordered_map<uint32_t, go_use::LockRow>& lockRows(pipeline::AssetManager* am) {
    static std::unordered_map<uint32_t, go_use::LockRow> rows;
    static bool loaded = false;
    if (loaded || !am || !am->isInitialized()) return rows;
    loaded = true;
    auto dbc = am->loadDBC("Lock.dbc");
    if (!dbc || !dbc->isLoaded() || dbc->getFieldCount() < 33) return rows;
    for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
        go_use::LockRow row;
        for (uint32_t i = 0; i < 8; ++i) {
            row.type[i] = dbc->getUInt32(r, 1 + i);
            row.index[i] = dbc->getUInt32(r, 9 + i);
            row.skill[i] = dbc->getUInt32(r, 17 + i);
            row.action[i] = dbc->getUInt32(r, 25 + i);
        }
        rows[dbc->getUInt32(r, 0)] = row;
    }
    return rows;
}

/// GameObjectDisplayInfo.dbc's GeoBox (+0x30..+0x44), by display id.
struct GeoBox {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};
const std::unordered_map<uint32_t, GeoBox>& displayBoxes(pipeline::AssetManager* am) {
    static std::unordered_map<uint32_t, GeoBox> boxes;
    static bool loaded = false;
    if (loaded || !am || !am->isInitialized()) return boxes;
    loaded = true;
    auto dbc = am->loadDBC("GameObjectDisplayInfo.dbc");
    const auto* layout = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("GameObjectDisplayInfo") : nullptr;
    const uint32_t first = layout ? layout->tryField("GeoBoxMinX") : 0xFFFFFFFFu;
    if (!dbc || !dbc->isLoaded() || first == 0xFFFFFFFFu || first + 6 > dbc->getFieldCount()) return boxes;
    for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
        GeoBox b;
        for (int k = 0; k < 3; ++k) {
            b.min[k] = dbc->getFloat(r, first + static_cast<uint32_t>(k));
            b.max[k] = dbc->getFloat(r, first + 3 + static_cast<uint32_t>(k));
        }
        boxes[dbc->getUInt32(r, 0)] = b;
    }
    return boxes;
}

/// SpellItemEnchantment.dbc's USE_SPELL (7) effects by enchantment: the
/// spell of the first effect of that type (Effect 2-4, EffectArg 11-13).
const std::unordered_map<uint32_t, uint32_t>& enchantUseSpells(pipeline::AssetManager* am) {
    static std::unordered_map<uint32_t, uint32_t> spells;
    static bool loaded = false;
    if (loaded || !am || !am->isInitialized()) return spells;
    loaded = true;
    auto dbc = am->loadDBC("SpellItemEnchantment.dbc");
    if (!dbc || !dbc->isLoaded() || dbc->getFieldCount() < 14) return spells;
    for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
        for (uint32_t k = 0; k < 3; ++k) {
            if (dbc->getUInt32(r, 2 + k) == 7) {
                spells[dbc->getUInt32(r, 0)] = dbc->getUInt32(r, 11 + k);
                break;
            }
        }
    }
    return spells;
}

std::string lockTypeName(pipeline::AssetManager* am, uint32_t lockType) {
    if (auto dbc = am && am->isInitialized() ? am->loadDBC("LockType.dbc") : nullptr; dbc && dbc->isLoaded()) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i)
            if (dbc->getUInt32(i, 0) == lockType) return dbc->getString(i, 1);
    }
    return {};
}

}  // namespace

GameHandler::GameObjectFacts GameHandler::gameObjectFacts(const GameObject& go,
                                                         const GameObjectQueryResponseData* info) const {
    const auto field = [&](UF f) {
        const uint16_t i = fieldIndex(f);
        return i != 0xFFFF ? go.getField(i) : 0u;
    };
    GameObjectFacts f;
    const uint32_t bytes1 = field(UF::GAMEOBJECT_BYTES_1);
    f.state = bytes1 & 0xFFu;
    f.type = (bytes1 >> 8) & 0xFFu;
    if (fieldIndex(UF::GAMEOBJECT_BYTES_1) == 0xFFFF && info) f.type = info->type;
    f.flags = field(UF::GAMEOBJECT_FLAGS);
    f.level = static_cast<int32_t>(field(UF::GAMEOBJECT_LEVEL));
    f.info = info;
    return f;
}

uint32_t GameHandler::gameObjectData(const GameObjectFacts& f, go_use::Field which) const {
    const int i = go_use::dataIndex(f.type, which);
    if (!f.info || !f.info->hasData || i < 0 || i >= 24) return 0;
    return f.info->data[i];
}

const go_use::LockRow* GameHandler::gameObjectLock(const GameObjectFacts& f) const {
    // 0x0070ef30: the type's lock field, and the Lock.dbc row it names.
    const uint32_t lockId = gameObjectData(f, go_use::Field::Lock);
    if (lockId == 0) return nullptr;
    const auto& rows = lockRows(services_.assetManager);
    const auto it = rows.find(lockId);
    return it != rows.end() ? &it->second : nullptr;
}

std::optional<GameHandler::CarriedItem> GameHandler::findCarriedItem(uint32_t itemId) const {
    // 0x00754a20: the item anywhere the player carries it.
    const Inventory& inv = getInventory();
    for (int i = 0; i < inv.getBackpackSize(); ++i)
        if (inv.getBackpackSlot(i).item.itemId == itemId)
            return CarriedItem{0xFF, static_cast<uint8_t>(slots::backpackWireSlot(i)), inv.getBackpackSlot(i).item.guid};
    for (int b = 0; b < Inventory::NUM_BAG_SLOTS; ++b)
        for (int s = 0; s < inv.getBagSize(b); ++s)
            if (inv.getBagSlot(b, s).item.itemId == itemId)
                return CarriedItem{static_cast<uint8_t>(slots::wornBagContainer(b)), static_cast<uint8_t>(s),
                                   inv.getBagSlot(b, s).item.guid};
    for (int i = 0; i < inv.getKeyringSize(); ++i)
        if (inv.getKeyringSlot(i).item.itemId == itemId)
            return CarriedItem{0xFF, static_cast<uint8_t>(slots::keyringWireSlot(i)), inv.getKeyringSlot(i).item.guid};
    return std::nullopt;
}

go_use::Opening GameHandler::gameObjectOpening(const GameObjectFacts& f) {
    const go_use::LockRow* lock = gameObjectLock(f);
    if (!lock || !spellHandler_) return {};
    loadSpellNameCache();
    const auto& cache = spellNameCache_;
    const auto effects = [&](uint32_t spellId, uint32_t (&e)[3], uint32_t (&m)[3]) {
        const auto it = cache.find(spellId);
        if (it == cache.end()) return false;
        for (int k = 0; k < 3; ++k) {
            e[k] = it->second.effectIds[k];
            m[k] = it->second.effectMiscValues[k];
        }
        return true;
    };
    const auto value = [this](uint32_t spellId, int k) { return openLockEffectValue(spellId, k); };
    const auto carries = [&](uint32_t itemId) { return findCarriedItem(itemId).has_value(); };
    // 0x00707c60(0x21): the key's own use spell with an OPEN_LOCK effect,
    // else the spell of the first of its enchantments that is used (type 7).
    const auto keySpell = [&](uint32_t itemId) -> uint32_t {
        const ItemQueryResponseData* item = getItemInfo(itemId);
        if (!item || !item->valid) {
            ensureItemInfo(itemId);
            return 0;
        }
        for (const auto& sp : item->spells) {
            if (sp.spellId == 0 || sp.spellTrigger != 0) continue;
            uint32_t e[3] = {}, m[3] = {};
            if (!effects(sp.spellId, e, m)) continue;
            for (const uint32_t effect : e)
                if (effect == go_use::kEffectOpenLock) return sp.spellId;
        }
        const auto key = findCarriedItem(itemId);
        const auto online = key ? onlineItems_.find(key->guid) : onlineItems_.end();
        if (online == onlineItems_.end()) return 0;
        const auto& useSpells = enchantUseSpells(services_.assetManager);
        for (const uint32_t enchant : online->second.enchantIds) {
            if (const auto it = useSpells.find(enchant); enchant != 0 && it != useSpells.end() && it->second != 0)
                return it->second;
        }
        return 0;
    };
    const uint32_t targeting = spellHandler_->unitTargetSpellId() != 0 ? spellHandler_->unitTargetSpellId()
                                                                       : spellHandler_->groundTargetSpellId();
    const auto& known = spellHandler_->getKnownSpells();
    return go_use::evaluate(*lock, f.state, f.flags, f.level, known, targeting, effects, value, carries, keySpell);
}

int32_t GameHandler::openLockEffectValue(uint32_t spellId, int k) const {
    // 0x008016c0 for the player: the skill in the spell's line stands for
    // its level (0x006dc330), then the spell's modifiers - all effects (8)
    // and this effect's own (3, 12, 23) - and rounded to the nearest.
    const auto it = spellNameCache_.find(spellId);
    if (it == spellNameCache_.end() || !spellHandler_ || k < 0 || k > 2) return 0;
    const auto& e = it->second;
    int32_t skill = 0;
    if (const auto line = spellToSkillLine_.find(spellId); line != spellToSkillLine_.end()) {
        if (const auto sk = playerSkills_.find(line->second); sk != playerSkills_.end())
            skill = sk->second.effectiveValue();
    }
    float v = go_use::effectValue(e.effectBasePoints[k], e.effectPointsPerLevel[k], skill, e.baseLevel,
                                  e.maxLevel);
    static constexpr uint8_t kEffectOp[3] = {3, 12, 23};
    v = spell_mods::apply(v, spellHandler_->spellModifiers(spellId, 8));
    v = spell_mods::apply(v, spellHandler_->spellModifiers(spellId, kEffectOp[k]));
    return static_cast<int32_t>(std::lround(v));
}

std::optional<GameHandler::ItemLockShown> GameHandler::itemLockShown(uint32_t lockId, int32_t itemLevel) {
    // 0x007086b0 for an item's lock, as its tooltip asks (0x006277f0): each
    // skill slot (type 2) against the player's spells in order, an OPEN_LOCK
    // effect of the slot's kind - the first whose value reaches what the slot
    // wants (its skill, else the item's level times five) answers, else the
    // last one tried. ENCRYPTED for kind 20: the found slot's, or any slot's
    // when no spell answers.
    const auto& rows = lockRows(services_.assetManager);
    const auto row = rows.find(lockId);
    if (lockId == 0 || row == rows.end() || !spellHandler_) return std::nullopt;
    const auto& lock = row->second;
    loadSpellNameCache();
    constexpr uint32_t kLockKindEncrypted = 20;
    ItemLockShown out;
    uint32_t foundKind = 0;
    for (int i = 0; i < 8; ++i) {
        if (lock.type[i] != 2) continue;
        for (const uint32_t spellId : spellHandler_->getKnownSpells()) {
            const auto it = spellNameCache_.find(spellId);
            if (it == spellNameCache_.end()) continue;
            for (int k = 0; k < 3; ++k) {
                if (it->second.effectIds[k] != go_use::kEffectOpenLock ||
                    it->second.effectMiscValues[k] != lock.index[i])
                    continue;
                out.hasSpell = true;
                out.required = lock.skill[i] != 0 ? static_cast<int32_t>(lock.skill[i]) : itemLevel * 5;
                out.value = openLockEffectValue(spellId, k);
                foundKind = lock.index[i];
                if (out.required <= out.value) {
                    out.encrypted = foundKind == kLockKindEncrypted;
                    return out;
                }
            }
        }
    }
    if (out.hasSpell) {
        out.encrypted = foundKind == kLockKindEncrypted;
    } else {
        for (const uint32_t kind : lock.index) out.encrypted = out.encrypted || kind == kLockKindEncrypted;
    }
    return out;
}

bool GameHandler::gameObjectAllowsMounted(const GameObjectFacts& f) const {
    // 0x00710390: a mailbox always, a barber chair never, else its field.
    if (f.type == 19) return true;
    if (f.type == 32) return false;
    return gameObjectData(f, go_use::Field::Mounted) != 0;
}

bool GameHandler::gameObjectInReach(const GameObject& go, const GameObjectFacts& f) {
    // 0x0070fa70: the handler's reach, or the opening spell's; a box round
    // the object's model widened by it, which the player has to stand in.
    float reach = go_use::handlerFor(f.type).reach;
    if (const auto o = gameObjectOpening(f); !o.locked && o.spell != 0) {
        if (spellHandler_) {
            if (const auto r = spellHandler_->spellRange(o.spell, getPlayerGuid())) reach = r->max;
        }
    } else if (f.state == 2) {
        return false;
    }
    // The player where it moves now (the mover's own position).
    const glm::vec3 offset = core::coords::canonicalToServer(
        glm::vec3(movementInfo.x - go.getX(), movementInfo.y - go.getY(), movementInfo.z - go.getZ()));
    if (f.type == 8) return glm::dot(offset, offset) <= reach * reach;
    // The box: a door's is its model's for the animation it plays
    // (0x008266b0, 0x0082ced0); any other's its display's GeoBox, or its
    // model's own bounds where the display has no row (0x00824f00,
    // 0x004f5e20) - and with no model loaded, none, so not in reach.
    std::optional<std::pair<glm::vec3, glm::vec3>> box;
    if (f.type == 0) {
        if (gameObjectBoundsCallback_) box = gameObjectBoundsCallback_(go.getGuid(), true);
    } else {
        const auto& boxes = displayBoxes(services_.assetManager);
        if (const auto row = boxes.find(go.getDisplayId()); row != boxes.end())
            box = std::make_pair(row->second.min, row->second.max);
        else if (gameObjectBoundsCallback_)
            box = gameObjectBoundsCallback_(go.getGuid(), false);
    }
    if (!box) return false;
    const float scale = gameObjectScale(go);
    const glm::vec3 local = glm::inverse(gameObjectRotation(go)) * offset;
    const glm::vec3 lo = box->first * scale - glm::vec3(reach);
    const glm::vec3 hi = box->second * scale + glm::vec3(reach);
    return local.x >= lo.x && local.y >= lo.y && local.z >= lo.z &&
           local.x <= hi.x && local.y <= hi.y && local.z <= hi.z;
}

float GameHandler::gameObjectScale(const GameObject& go) const {
    float scale = 1.0f;
    if (const uint16_t i = fieldIndex(UF::OBJECT_FIELD_SCALE_X); i != 0xFFFF) {
        const uint32_t bits = go.getField(i);
        std::memcpy(&scale, &bits, sizeof(float));
        if (!(scale > 0.0f)) scale = 1.0f;
    }
    return scale;
}

glm::quat GameHandler::gameObjectRotation(const GameObject& go) const {
    namespace gor = game_object_rotation;
    return go.getRotation() ? glm::normalize(*go.getRotation())
                            : gor::fromFacing(core::coords::canonicalToServerYaw(go.getOrientation()));
}

bool GameHandler::playerInForm() const {
    // 0x00721ca0: a form, and not one SpellShapeshiftForm flags 1 (a stance)
    // - nor one the player is already leaving (+0x9f4, which
    // getShapeshiftFormId reads as no form).
    const uint8_t form = getShapeshiftFormId();
    if (form == 0) return false;
    const auto* row = shapeshiftFormRow(form);
    return !row || (row->flags & 0x1u) == 0;
}

bool GameHandler::chairSeatInReach(const GameObject& go, const GameObjectFacts& f) const {
    // 0x00711db0: within the handler's reach of one of the chair's seats -
    // 0x00710240's count from its data for a chair, one for a barber chair
    // (0x008c8de0) - placed by the chair's place, turn and scale (0x0070c700).
    const uint32_t seats = f.type == 32 ? 1u : gameObjectData(f, go_use::Field::ChairSlots);
    const float reach = go_use::handlerFor(f.type).reach;
    const glm::vec3 offset = core::coords::canonicalToServer(
        glm::vec3(movementInfo.x - go.getX(), movementInfo.y - go.getY(), movementInfo.z - go.getZ()));
    const glm::quat rotation = gameObjectRotation(go);
    const float scale = gameObjectScale(go);
    for (uint32_t i = 0; i < seats; ++i) {
        const glm::vec3 seat = rotation * glm::vec3(0.0f, go_use::seatOffset(seats, i) * scale, 0.0f);
        const glm::vec3 d = offset - seat;
        if (glm::dot(d, d) < reach * reach) return true;
    }
    return false;
}

int GameHandler::gameObjectReactionToPlayer(const GameObject& go) const {
    // 0x0070edd0: the unit that made it, as that unit regards the player;
    // else its faction's (0x0071f770, 0-based); 3 with neither.
    const uint16_t c = fieldIndex(UF::GAMEOBJECT_FIELD_CREATED_BY);
    const uint64_t creator = c == 0xFFFF ? 0
        : (static_cast<uint64_t>(go.getField(static_cast<uint16_t>(c + 1))) << 32) | go.getField(c);
    const auto self = getEntityManager().getEntity(getPlayerGuid());
    if (creator != 0 && self) {
        if (auto unit = getEntityManager().getEntity(creator); unit && unit->isUnit())
            return unitReaction(*unit, *self);
    }
    const uint16_t fi = fieldIndex(UF::GAMEOBJECT_FACTION);
    const uint32_t faction = fi == 0xFFFF ? 0 : go.getField(fi);
    if (faction == 0) return 3;
    return factionReactionToPlayer(faction) - 1;
}

bool GameHandler::gameObjectUsableNow(const GameObject& go, const GameObjectFacts& f) {
    // 0x007112a0.
    namespace gu = go_use;
    const uint16_t di = fieldIndex(UF::GAMEOBJECT_DYNAMIC);
    const uint32_t dynamic = di == 0xFFFF ? 0 : go.getField(di) & 0xFFFFu;
    if (!gu::usableByState(f.type, gameObjectLock(f) != nullptr, f.flags, dynamic,
                           gameObjectReactionToPlayer(go)))
        return false;
    const uint16_t c = fieldIndex(UF::GAMEOBJECT_FIELD_CREATED_BY);
    const uint64_t creator = c == 0xFFFF ? 0
        : (static_cast<uint64_t>(go.getField(static_cast<uint16_t>(c + 1))) << 32) | go.getField(c);
    if (f.type == 6 && creator != 0 && (creator >> 60) == 0) {
        // A trap a player set: only where that player may fight this one,
        // or is free-for-all and not grouped with it.
        auto setter = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(creator));
        if (!setter) return false;
        const uint16_t b2 = fieldIndex(UF::UNIT_FIELD_BYTES_2);
        const uint32_t pvp = b2 == 0xFFFF ? 0 : (setter->getField(b2) >> 8) & 0xFFu;
        bool grouped = false;
        for (const auto& m : getPartyData().members)
            if (m.guid == creator) grouped = true;
        const bool freeForAll = (pvp & 0x4u) != 0 && !grouped;
        if ((pvp & 0x1u) == 0 && !unitMayAttack(creator, getPlayerGuid()) && !freeForAll) return false;
    }
    // 0x00710410: one made for its maker alone.
    if (gameObjectData(f, gu::Field::CreatorOnly) != 0 && creator != getPlayerGuid()) return false;
    return true;
}

bool GameHandler::isPlayersBobber(const GameObject& go) const {
    // 0x00712030: the bobber the player channels at.
    auto me = getEntityManager().getEntity(getPlayerGuid());
    const uint16_t ch = fieldIndex(UF::UNIT_FIELD_CHANNEL_OBJECT);
    if (!me || ch == 0xFFFF) return false;
    const uint64_t channel =
        (static_cast<uint64_t>(me->getField(static_cast<uint16_t>(ch + 1))) << 32) | me->getField(ch);
    return channel == go.getGuid();
}

GameHandler::GameObjectPointer GameHandler::gameObjectPointer(uint64_t guid) {
    GameObjectPointer p;
    auto entity = getEntityManager().getEntity(guid);
    if (!entity || entity->getType() != ObjectType::GAMEOBJECT) return p;
    const auto& go = static_cast<const GameObject&>(*entity);
    const GameObjectFacts f = gameObjectFacts(go, getCachedGameObjectInfo(go.getEntry()));
    namespace gu = go_use;
    const auto data = [&](int i) { return f.info && f.info->hasData && i >= 0 && i < 24 ? f.info->data[i] : 0u; };
    switch (gu::mouseoverBy(f.type)) {
        case gu::Mouseover::Never: p.tracked = false; break;
        case gu::Mouseover::Always: p.tracked = true; break;
        case gu::Mouseover::Highlight: p.tracked = data(gu::highlightIndex(f.type)) != 0; break;
        case gu::Mouseover::Bobber: p.tracked = isPlayersBobber(go) && gameObjectUsableNow(go, f); break;
        case gu::Mouseover::Usable: p.tracked = gameObjectUsableNow(go, f); break;
    }
    if (p.tracked) {
        if (const uint32_t quest = data(gu::requiredQuestIndex(f.type)); quest != 0) {
            const auto& log = getQuestLog();
            p.tracked = std::any_of(log.begin(), log.end(), [quest](const auto& q) { return q.questId == quest; });
        }
    }
    p.interactive = p.tracked && gameObjectClickable(go, f);
    return p;
}

bool GameHandler::gameObjectClickable(const GameObject& go, const GameObjectFacts& f) {
    // The handler's slot +0x18, which a right click asks before anything
    // else (0x00711140).
    namespace gu = go_use;
    const gu::Clickable kind = gu::clickableBy(f.type);
    if (kind == gu::Clickable::Never) return false;
    auto me = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(getPlayerGuid()));
    if (!me) return false;
    if (kind == gu::Clickable::Bobber && !isPlayersBobber(go)) return false;
    if (!gameObjectUsableNow(go, f)) return false;
    if (kind == gu::Clickable::Chair) {
        // 0x00711ce0: not the chair the player already sits in - sitting
        // (stand states 4-6) within sqrt(1/6) yd of one of its seats.
        const uint8_t stand = me->getStandState();
        if (stand > 3 && stand < 7) {
            const uint32_t seats = f.type == 32 ? 1u : gameObjectData(f, gu::Field::ChairSlots);
            const glm::vec3 offset = core::coords::canonicalToServer(
                glm::vec3(movementInfo.x - go.getX(), movementInfo.y - go.getY(), movementInfo.z - go.getZ()));
            const glm::quat rotation = gameObjectRotation(go);
            const float scale = gameObjectScale(go);
            for (uint32_t i = 0; i < seats; ++i) {
                const glm::vec3 d = offset - rotation * glm::vec3(0.0f, gu::seatOffset(seats, i) * scale, 0.0f);
                if (glm::dot(d, d) < 1.0f / 6.0f) return false;
            }
        }
    }
    return true;
}

bool GameHandler::ritualAllows(const GameObject& go, const GameObjectFacts& f) {
    // 0x00712080: a ritual whose spell may not be cast in combat
    // (Attributes 0x10000000) is refused in combat (0x0071afb0: UNIT_FLAG
    // 0x800, or a unit being fought) unless an aura lets that go (0x0072aaf0:
    // aura 262 on the spell); and one a player made only for that player and
    // those grouped with it (0x00718ca0, 0x00718d70). Neither says why.
    const uint32_t spellId = gameObjectData(f, go_use::Field::RitualSpell);
    loadSpellNameCache();
    if (const auto it = spellNameCache_.find(spellId); it != spellNameCache_.end() &&
                                                       (it->second.attr & 0x10000000u) != 0) {
        auto me = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(getPlayerGuid()));
        const bool fighting = (me && (me->getUnitFlags() & 0x800u) != 0) || isAutoAttacking();
        if (fighting && !(spellHandler_ && spellHandler_->playerAuraAffects(262, spellId))) return false;
    }
    uint64_t creator = 0;
    if (const uint16_t i = fieldIndex(UF::GAMEOBJECT_FIELD_CREATED_BY); i != 0xFFFF)
        creator = (static_cast<uint64_t>(go.getField(static_cast<uint16_t>(i + 1))) << 32) | go.getField(i);
    if (creator != 0 && (creator >> 60) == 0) {
        if (!getEntityManager().getEntity(creator)) return false;
        if (creator == getPlayerGuid()) return true;
        for (const auto& m : getPartyData().members)
            if (m.guid == creator) return true;
        return false;
    }
    return true;
}

uint16_t GameHandler::gameObjectRefusal(const GameObject& go, const GameObjectFacts& f, std::string& arg) {
    namespace e = go_use::err;
    // The ritual's own checks come first (0x00712080), then 0x00711470.
    if (f.type == 18 && !ritualAllows(go, f)) return go_use::kRefusedSilently;
    if (const uint16_t generic = gameObjectGenericRefusal(go, f, arg); generic != 0) return generic;
    // A chair wants the player at one of its seats (0x00711db0), a barber
    // chair the player looking as itself and in no form (0x00711eb0).
    if ((f.type == 7 || f.type == 32) && !chairSeatInReach(go, f)) return e::kTooFar;
    if (f.type == 32) {
        auto me = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(getPlayerGuid()));
        const auto field = [&](UF which) {
            const uint16_t i = fieldIndex(which);
            return me && i != 0xFFFF ? me->getField(i) : 0u;
        };
        if (field(UF::UNIT_FIELD_DISPLAYID) != field(UF::UNIT_FIELD_NATIVEDISPLAYID) || playerInForm())
            return e::kNotWhileShapeshifted;
    }
    return 0;
}

uint16_t GameHandler::gameObjectGenericRefusal(const GameObject& go, const GameObjectFacts& f, std::string& arg) {
    namespace e = go_use::err;
    // 0x00711470, in its order.
    if (isPlayerDead() || isPlayerGhost()) return e::kPlayerDead;
    if (!isServerMovementAllowed()) return e::kClientLockedOut;
    auto me = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(getPlayerGuid()));
    const uint32_t unitFlags = me ? me->getUnitFlags() : 0u;
    if ((unitFlags & 0x40000u) != 0) {
        if (const uint32_t mechanic = playerAuraMechanic({12u}); mechanic != 0) {
            const std::string* name = getSpellMechanicName(mechanic);
            arg = name ? *name : std::string("UNKNOWN");
            return e::kPreventedByMechanic;
        }
        return e::kGenericStunned;
    }
    if (!gameObjectLock(f)) {
        const bool getsDown = unit_target::wouldDismount(unitFlags, isPlayerFlying(),
                                                         addons::storedCVarValue("autoDismount", "1") != "0",
                                                         addons::storedCVarValue("autoDismountFlying", "0") != "0");
        if (isMounted() && !gameObjectAllowsMounted(f) && !getsDown) return e::kNotWhileMounted;
        if (playerInForm()) {
            const auto* row = shapeshiftFormRow(getShapeshiftFormId());
            if ((!row || (row->flags & 0x8u) == 0) && !wouldAutoUnshift()) return e::kNotWhileShapeshifted;
        }
    }
    if ((unitFlags & 0x80000u) != 0 && gameObjectData(f, go_use::Field::NoCombat) != 0) return e::kNotInCombat;
    if ((unitFlags & 0x80000000u) != 0 && (f.type == 3 || gameObjectData(f, go_use::Field::NoImmune) != 0))
        return e::kCantImmune;
    if ((f.flags & go_use::kFlagLocked) != 0 && gameObjectOpening(f).locked)
        return go_use::handlerFor(f.type).lockedError;
    if (!gameObjectInReach(go, f)) return e::kTooFar;
    return 0;
}

void GameHandler::sayGameObjectError(uint16_t id, const std::vector<std::string>& args) {
    const ErrorText t = errorText(id);
    playGameErrorSound(t.key);
    raiseUiError(args.empty() ? interfaceString(t.key, t.enUS) : interfaceFormat(t.key, t.enUS, args));
}

bool GameHandler::useGameObjectByLock(const GameObject& go, const GameObjectFacts& f) {
    const uint64_t guid = go.getGuid();
    const go_use::Opening o = gameObjectOpening(f);
    if (!o.locked) {
        if (o.spell == 0) {
            // Nothing to cast: off the mount where the object wants that,
            // out of a form that may not use it (SpellShapeshiftForm flag 8),
            // and CMSG_GAMEOBJ_USE.
            // 0x007116c0 dismounts where no dismount is already waiting on the
            // mount's animation (+0xa30 0x10000000, which this client never
            // holds: it dismounts at once), and the request itself
            // (0x007412e0) is nothing on a flight path (UNIT_FLAG 0x100000).
            auto me = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(getPlayerGuid()));
            const bool onFlight = me && (me->getUnitFlags() & 0x100000u) != 0;
            if (isMounted() && !gameObjectAllowsMounted(f) && !onFlight) dismount();
            if (playerInForm()) {
                const auto* row = shapeshiftFormRow(getShapeshiftFormId());
                if (!row || (row->flags & 0x8u) == 0) leaveShapeshiftForm();
            }
            socket->send(GameObjectUsePacket::build(guid));
            return true;
        }
        // The opening spell, cast at the object through the cast entry's own
        // checks (0x0080da40 -> 0x0080cce0, then 0x0080bc80) - from the key,
        // where a key opens it.
        if (!spellHandler_) return false;
        if (o.keyItem != 0) {
            const auto key = findCarriedItem(o.keyItem);
            return key && spellHandler_->castAtGameObject(o.spell, guid, key->guid, key->bag, key->slot);
        }
        return spellHandler_->castAtGameObject(o.spell, guid);
    }
    // Locked: what the first slot wanted.
    const go_use::LockRow& lock = *gameObjectLock(f);
    const uint16_t refusal = go_use::lockedRefusal(lock, f.type, f.state, f.flags, o.spell != 0);
    namespace e = go_use::err;
    switch (refusal) {
        case 0:
            break;
        case e::kLockedWithItem: {
            // The key's name, once it is described - nothing until then.
            const ItemQueryResponseData* item = getItemInfo(lock.index[0]);
            if (item && item->valid) sayGameObjectError(refusal, {item->name});
            else ensureItemInfo(lock.index[0]);
            break;
        }
        case e::kLockedWithSpell:
        case e::kLockedWithSpellKnown: {
            if (lock.type[0] == 3) {
                // The spell the lock names, where there is one.
                const std::string& name = getSpellName(lock.index[0]);
                if (!name.empty()) sayGameObjectError(e::kLockedWithSpell, {name});
                break;
            }
            std::string name = lockTypeName(services_.assetManager, lock.index[0]);
            if (name.empty()) name = "UNKNOWN";
            if (refusal == e::kLockedWithSpell) {
                sayGameObjectError(refusal, {name});
            } else {
                const int32_t required = lock.skill[0] != 0 ? static_cast<int32_t>(lock.skill[0]) : f.level * 5;
                sayGameObjectError(refusal, {name, std::to_string(required)});
            }
            break;
        }
        default:
            sayGameObjectError(refusal, {});
            break;
    }
    return false;
}

} // namespace game
} // namespace wowee
