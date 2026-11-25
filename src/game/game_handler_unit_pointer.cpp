// The pointer over a unit and the right click on one: what unit_pointer.hpp
// asks of the player and of the unit.
#include "game/game_handler.hpp"
#include "game/click_to_move.hpp"
#include "game/movement_handler.hpp"
#include "game/soft_interact.hpp"
#include "game/spell_handler.hpp"
#include "game/spell_unit_target.hpp"
#include "game/unit_pointer.hpp"
#include "game/update_field_table.hpp"
#include "addons/lua_api_registrations.hpp"
#include "rendering/minimap_blips.hpp"
#include "ui/cursor_art.hpp"

namespace wowee {
namespace game {

namespace {

uint64_t guidAt(const Entity& e, UF f) {
    const uint16_t i = fieldIndex(f);
    if (i == 0xFFFF) return 0;
    return (static_cast<uint64_t>(e.getField(static_cast<uint16_t>(i + 1))) << 32) | e.getField(i);
}

}  // namespace

uint64_t GameHandler::possessedUnit() const {
    const uint64_t sight = cameraTarget();
    if (sight == playerGuid || !movementHandler_ || !movementHandler_->farSightAnnounced()) return 0;
    const auto unit = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(sight));
    if (!unit || (unit->getUnitFlags() & 0x01000000u) == 0) return 0;
    const uint64_t charmer = guidAt(*unit, UF::UNIT_FIELD_CHARMEDBY);
    return (charmer != 0 ? charmer : guidAt(*unit, UF::UNIT_FIELD_CREATEDBY)) == playerGuid ? sight : 0;
}

unit_pointer::Player GameHandler::unitPointerPlayer() const {
    unit_pointer::Player p;
    auto& entities = getEntityManager();
    if (cameraTarget() != playerGuid) {
        p.seesElsewhere = true;
        p.possesses = possessedUnit() != 0;
    }
    const auto seat = vehicleRoster_.passenger(playerGuid);
    if (seat && (vehicle::isVehicleGuid(seat->vehicleGuid) || vehicle::isPlayerGuid(seat->vehicleGuid)) &&
        entities.getEntity(seat->vehicleGuid)) {
        const auto* row = vehicleRoster_.seatOf(playerGuid);
        p.mayInteract = row && (row->flags & unit_pointer::kSeatInteracts) != 0;
    }
    return p;
}

soft_interact::LootCheck GameHandler::playerLootCheck() const {
    soft_interact::LootCheck check;
    const auto player = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(playerGuid));
    if (!player) {
        check.deadOrGhost = true;
        return check;
    }
    check.anyDistance = clickToMoveAllowed();
    // 0x00721ca0 with the form's Flags 0x100.
    if (playerInForm()) {
        const auto* form = shapeshiftFormRow(getShapeshiftFormId());
        check.formForbids = form && (form->flags & 0x100u) != 0;
    }
    check.looting = isLootWindowOpen();
    check.deadOrGhost = isPlayerDead() || isPlayerGhost();
    check.falling = (playerMovementInfo().flags & 0x1000u) != 0;
    check.stunned = (player->getUnitFlags() & 0x40000u) != 0;
    check.clientControl = movementHandler_ && movementHandler_->unitHasClientControl(playerGuid);
    return check;
}

bool GameHandler::playerMayAttackNow(const Unit& unit) const {
    const auto player = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(playerGuid));
    if (!player) return false;
    const auto on = [](const char* name, const char* byDefault) {
        return addons::storedCVarValue(name, byDefault) != "0";
    };
    const bool dismounts = unit_target::wouldDismount(player->getUnitFlags(), isPlayerFlying(), on("autoDismount", "1"),
                                                      on("autoDismountFlying", "0"));
    return unit_target::mayAttackNow(isPlayerDead() || isPlayerGhost(), isMounted(), dismounts, unit.getHealth(),
                                     unit.getDynamicFlags(), unitMayAttack(playerGuid, unit.getGuid()));
}

std::string GameHandler::unitCursorArt(uint64_t guid) {
    namespace up = unit_pointer;
    const auto unit = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(guid));
    const auto player = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(playerGuid));
    if (!unit || !player) return {};
    up::CursorAsk a;
    a.player = unitPointerPlayer();
    a.autoLoots = clickAutoLoots();
    a.pacified = (player->getUnitFlags() & 0x20000u) != 0;
    a.hasControl = isServerMovementAllowed();
    const MovementInfo& me = playerMovementInfo();
    const float dx = unit->getX() - me.x, dy = unit->getY() - me.y, dz = unit->getZ() - me.z;
    a.distanceSq = dx * dx + dy * dy + dz * dz;
    a.alive = unit->getHealth() > 0;
    a.isPlayer = unit->getType() == ObjectType::PLAYER;
    a.serviceOffered = npcServiceOffered(guid);
    a.npcFlags = unit->getNpcFlags();
    a.combatReach = unit->getCombatReach();
    a.questStatus = static_cast<uint8_t>(getQuestGiverStatus(guid));
    // 0x00715ea0: the creature's IconName, a kind of the table's or a
    // picture of that name.
    std::string icon;
    if (!a.isPlayer) {
        const auto& infos = getCreatureInfoCache();
        if (const auto it = infos.find(unit->getEntry()); it != infos.end()) icon = it->second.iconName;
    }
    if (!icon.empty()) {
        const auto kindOf = [](const std::string& name) {
            const int kind = ui::cursorKindForArt(name);
            return kind != ui::kCursorKindNone ? kind : ui::kCursorKindCustom;
        };
        a.icon = kindOf(icon);
        a.iconUnable = kindOf("Unable" + icon);
    }
    // 0x00723050: a group member's vehicle with a seat left.
    const npc_interaction::Unit facts = npcInteractionFacts(guid);
    a.seatFree = facts.isPlayer && !facts.isSelf && facts.inGroup && facts.emptySeats != 0;
    const bool looted = isLootWindowOpen() && getCurrentLoot().lootGuid == guid;
    a.lootable = !a.alive && (unit->getDynamicFlags() & UNIT_DYNFLAG_LOOTABLE) != 0;
    if (a.lootable) {
        soft_interact::LootCheck check = playerLootCheck();
        const float reach = soft_interact::lootReach(unit->getCombatReach(), player->getCombatReach());
        check.reachSq = reach * reach;
        check.distSq = a.distanceSq;
        a.mayLoot = looted || soft_interact::playerMayLoot(check);
    }
    if (const uint32_t spell = (unit->getUnitFlags() & 0x4000000u) != 0 ? skinSpellFor(guid) : 0;
        spell != 0 && !(a.isPlayer && unitMayAssist(playerGuid, guid)) && spellHandler_) {
        a.skinnable = true;
        a.skinReach = spellHandler_->spellReachAt(spell, playerGuid, guid).max;
        const auto& infos = getCreatureInfoCache();
        if (const auto it = infos.find(unit->getEntry()); !a.isPlayer && it != infos.end()) {
            const uint32_t flags = it->second.typeFlags;
            a.skinKind = (flags & 0x100u) ? 1 : (flags & 0x200u) ? 2 : (flags & 0x8000u) ? 3 : 0;
        }
    }
    a.attackable = playerMayAttackNow(*unit);
    a.meleeReach = click_to_move::meleeReach(unit->getCombatReach(), player->getCombatReach());
    if (const uint16_t bytes0 = fieldIndex(UF::UNIT_FIELD_BYTES_0); bytes0 != 0xFFFF)
        a.playerIsHorde = rendering::minimap_blips::raceTeam(static_cast<uint8_t>(player->getField(bytes0) & 0xFF)) == 1;
    const int kind = up::cursor(a);
    // 0x00715f20.
    if (kind == ui::kCursorKindCustom) {
        const float reach = a.combatReach + 4.0f;
        return std::string("Interface\\Cursor\\") + (a.distanceSq <= reach * reach ? "" : "Unable") + icon + ".blp";
    }
    return ui::cursorPathForKind(kind);
}

std::string GameHandler::gameObjectCursorArt(uint64_t guid) {
    // 0x0070ce10: none of one that takes no click (the type handler's +0x18).
    const auto entity = getEntityManager().getEntity(guid);
    if (!entity || entity->getType() != ObjectType::GAMEOBJECT || !gameObjectPointer(guid).interactive) return {};
    const auto& go = static_cast<const GameObject&>(*entity);
    const auto* info = getCachedGameObjectInfo(go.getEntry());
    const bool usable = gameObjectUseAllowed(guid);
    // The template's IconName, a kind of the table's or a picture of that
    // name (0x0070cec0).
    if (info && !info->iconName.empty()) {
        const std::string art = (usable ? "" : "Unable") + info->iconName;
        if (const int kind = ui::cursorKindForArt(art); kind != ui::kCursorKindNone) return ui::cursorPathForKind(kind);
        return "Interface\\Cursor\\" + art + ".blp";
    }
    const uint32_t type = gameObjectFacts(go, info).type;
    if (const int own = unit_pointer::objectCursor(type, static_cast<uint8_t>(getQuestGiverStatus(guid)), usable); own != 0)
        return ui::cursorPathForKind(own);
    // 0x0070f9b0: its lock's LockType CursorName, "Unable" before it where it
    // cannot be used but for LockType 1, and only a name the table has; else
    // Interact.
    const auto [name, lockType] = lockCursor(go, info);
    if (!name.empty())
        return ui::cursorPathForKind(ui::cursorKindForArt(!usable && lockType != 1 ? "Unable" + name : name));
    return ui::cursorPathForKind(usable ? 5 : 5 + unit_pointer::kUnable);
}

unit_pointer::Click GameHandler::unitClick(uint64_t guid) {
    namespace up = unit_pointer;
    const auto unit = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(guid));
    const auto player = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(playerGuid));
    if (!unit || !player) return up::Click::Nothing;
    up::ClickAsk a;
    a.player = unitPointerPlayer();
    a.playerAlive = player->getHealth() > 0;
    a.playerIsMover = activeMover() == playerGuid;
    a.alive = unit->getHealth() > 0;
    a.shownDead = (unit->getDynamicFlags() & UNIT_DYNFLAG_DEAD) != 0;
    a.maySpeak = mayInteractWith(guid);
    a.mayAttack = playerMayAttackNow(*unit);
    return up::click(a);
}

}  // namespace game
}  // namespace wowee

