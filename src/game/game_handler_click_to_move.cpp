// Click-to-move: the player sent somewhere by a click (autoInteract) and
// steered there with the movement packets the keys send. The pure parts are
// in click_to_move.hpp; this is the client's glue around them (Unit_C,
// 0x00727400 and its neighbours).
#include "game/game_handler.hpp"
#include "game/click_to_move.hpp"
#include "game/chat_profanity.hpp"
#include "game/expansion_profile.hpp"
#include "game/interface_text.hpp"
#include "game/movement_handler.hpp"
#include "game/spell_handler.hpp"
#include "game/update_field_table.hpp"
#include "addons/lua_api_registrations.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include <cmath>

namespace wowee {
namespace game {

namespace ctm = click_to_move;

namespace {

glm::vec3 toServer(const glm::vec3& canonical) { return core::coords::canonicalToServer(canonical); }

constexpr uint32_t kSwimOrFly = static_cast<uint32_t>(MovementFlags::SWIMMING) |
                                static_cast<uint32_t>(MovementFlags::FLYING);

}  // namespace

bool GameHandler::autoInteractOn() const {
    // Registered by 0x0051d990 with the default "1" for the koKR client (the
    // locale index 0x00c5de9c is 1) and "0" for every other; its callback
    // 0x00512700 only copies it to the camera (0x005fe6a0). Every user reads
    // the CVar itself (0x00bd08f4 +0x30).
    const auto* profile = services_.expansionRegistry ? services_.expansionRegistry->getActive() : nullptr;
    const bool korean = profile && chat_profanity::localeIndex(profile->locale) == 1;
    return addons::storedCVarValue("autoInteract", korean ? "1" : "0") != "0";
}

bool GameHandler::playerAliveAsMover() const {
    // The player is alive (health above zero) and the active mover.
    if (!movementHandler_ || movementHandler_->activeMover() != playerGuid) return false;
    auto me = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(playerGuid));
    return me && static_cast<int32_t>(me->getHealth()) > 0;
}

bool GameHandler::clickToMoveAllowed() const {
    // 0x00721f50.
    return playerAliveAsMover() && autoInteractOn();
}

bool GameHandler::isClickToMoving() const {
    // 0x00721f90.
    return ctm_.kind != ctm::Kind::None && movementHandler_ && movementHandler_->activeMover() == playerGuid;
}

std::optional<glm::vec3> GameHandler::clickToMoveSubject(uint64_t guid) const {
    // 0x007317a0's subject: where it is; nothing for a unit on a taxi, or a
    // dead one unless it is being looted or skinned.
    auto e = getEntityManager().getEntity(guid);
    if (!e) return std::nullopt;
    if (auto* unit = dynamic_cast<const Unit*>(e.get())) {
        if ((unit->getUnitFlags() & 0x100000u) != 0) return std::nullopt;
        if (static_cast<int32_t>(unit->getHealth()) < 1 && ctm_.kind != ctm::Kind::Skin &&
            ctm_.kind != ctm::Kind::Loot)
            return std::nullopt;
    }
    return toServer(glm::vec3(e->getX(), e->getY(), e->getZ()));
}

bool GameHandler::startClickToMove(ctm::Kind kind, uint64_t guid, const glm::vec3& canonicalAt, float facing,
                                   std::optional<float> spellRange) {
    // 0x00727400. Whatever the mover was doing ends first, a follow (kind 3)
    // with AUTOFOLLOW_END (0x0081b530(0x166)).
    if (isClickToMoving() && ctm_.kind == ctm::Kind::Follow) fireAddonEvent("AUTOFOLLOW_END", {});
    if (ctm_.kind != ctm::Kind::None) ctm_ = ctm::State{};
    if (!movementHandler_) return false;
    const glm::vec3 mover = toServer(glm::vec3(movementInfo.x, movementInfo.y, movementInfo.z));
    const glm::vec3 at = toServer(canonicalAt);
    // 0x00715c70: not from too far away.
    if (!ctm::startsInRange(kind, mover, at)) {
        playGameErrorSound("ERR_AUTOFOLLOW_TOO_FAR");
        raiseUiError(interfaceString("ERR_AUTOFOLLOW_TOO_FAR", "Target is too far away."));
        return false;
    }
    // 0x0071b3b0: how near counts as there.
    ctm::ReachInput reach;
    if (auto me = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(playerGuid)))
        reach.moverCombatReach = me->getCombatReach();
    if (auto subject = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(guid)))
        reach.targetCombatReach = subject->getCombatReach();
    reach.spellRange = spellRange;
    reach.objectReach = ctmObjectReach_;
    const auto reachSq = ctm::reachSq(kind, reach);
    if (!reachSq) return false;
    const bool fullSpeedTurning = (movementInfo.flags2 & 0x8u) != 0;
    ctm_ = ctm::start(kind, guid, at, facing, *reachSq, mover, fullSpeedTurning,
                      movementHandler_->getServerTurnRate(), movementHandler_->movementClockMs());
    // A seated player stands (0x006dcb40(0)): the camera stands it on the
    // first steer, as it does for a key (CameraController::update).
    return true;
}

void GameHandler::stopClickToMove(bool faceSubject, bool stopMoving) {
    // 0x007272c0. Only the camera can turn the mover and stop it: the turn
    // is left for its next steer, the stop (0x0071aee0) handed to it now.
    if (!isClickToMoving()) return;
    // 0x0071aee0 for a kind that goes anywhere (+0 of its row), unless the
    // player's UNIT_FIELD_FLAGS_2 has 0x40.
    bool stopMover = stopMoving && ctm::row(ctm_.kind).used;
    if (stopMover) {
        auto me = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(playerGuid));
        const uint16_t flags2 = fieldIndex(UF::UNIT_FIELD_FLAGS_2);
        if (me && flags2 != 0xFFFF && (me->getField(flags2) & 0x40u) != 0) stopMover = false;
    }
    if (ctm_.kind == ctm::Kind::Follow) fireAddonEvent("AUTOFOLLOW_END", {});
    if (faceSubject) {
        if (const auto at = clickToMoveSubject(ctm_.guid)) {
            const glm::vec3 me = toServer(glm::vec3(movementInfo.x, movementInfo.y, movementInfo.z));
            ctmFaceOnStop_ = ctm::wrapTwoPi(ctm::facingTo(me, *at));
        }
    }
    ctm_ = ctm::State{};
    if (stopMover && ctmStopMover_) ctmStopMover_();
}

void GameHandler::stopClickToMoveFollow() {
    if (isClickToMoving() && ctm_.kind == ctm::Kind::Follow) stopClickToMove(false, true);
}

bool GameHandler::clickToMoveApproach(uint64_t guid, ctm::Kind kind, std::optional<float> spellRange) {
    // 0x0072b4a0: toward a unit or a corpse, by the player as the mover.
    if (guid == playerGuid || !movementHandler_ || movementHandler_->activeMover() != playerGuid) return false;
    auto subject = getEntityManager().getEntity(guid);
    if (!subject) return false;
    const auto* unit = dynamic_cast<const Unit*>(subject.get());
    switch (kind) {
    case ctm::Kind::Kind0:
    case ctm::Kind::Kind1:
    case ctm::Kind::NpcInteract:
        if (!unit) return false;
        break;
    case ctm::Kind::Loot:
    case ctm::Kind::Skin:
        if (subject->getType() != ObjectType::CORPSE && (!unit || static_cast<int32_t>(unit->getHealth()) > 0))
            return false;
        break;
    case ctm::Kind::Follow:
        // Its checks - a player on the player's side, the player alive, not
        // stunned, not channeling - are made by the caller with their lines
        // (MovementHandler::followGuid).
        break;
    default:
        return false;
    }
    if (!playerAliveAsMover()) return false;
    if (!startClickToMove(kind, guid, glm::vec3(subject->getX(), subject->getY(), subject->getZ()), 0.0f,
                          spellRange))
        return false;
    if (kind == ctm::Kind::Follow) {
        // AUTOFOLLOW_BEGIN with the name followed (0x0072a000(0, 1)).
        std::string name;
        if (const auto* p = dynamic_cast<const Player*>(subject.get())) name = p->getName();
        else if (unit) name = unit->getName();
        LOG_INFO("Following ", name, " (GUID: 0x", std::hex, guid, std::dec, ")");
        fireAddonEvent("AUTOFOLLOW_BEGIN", {name});
    }
    return true;
}

void GameHandler::clickToMoveTo(const glm::vec3& canonicalAt) {
    // 0x0072b6d0, from a right click on the ground (0x00527360).
    if (!clickToMoveAllowed()) return;
    startClickToMove(ctm::Kind::Move, 0, canonicalAt, 0.0f);
}

void GameHandler::clickToMoveFace(float facing) {
    // 0x0072b660.
    if (!clickToMoveAllowed()) return;
    startClickToMove(ctm::Kind::Face, 0, glm::vec3(0.0f), facing);
}

void GameHandler::clickToMoveFaceDirection(const glm::vec3& canonicalDir) {
    // 0x0072f040, from a right click on nothing (0x005278c0): turn that way,
    // and run on. Not with a spell waiting for a place (0x00bd0748), nor
    // for no direction.
    if (!clickToMoveAllowed() || isGroundTargeting()) return;
    const glm::vec3 dir = toServer(canonicalDir);
    if (glm::dot(dir, dir) == 0.0f) return;
    const glm::vec3 n = glm::normalize(dir);
    // Swimming or flying (0x2200000), the mover is pitched that way first,
    // at once (0x0072ead0, which ends any click-to-move under way).
    if ((movementInfo.flags & kSwimOrFly) != 0) {
        stopClickToMove();
        const float p = ctm::pitchOfDirection(n);
        if (movementHandler_) movementHandler_->setMoverPitchNow(p);
        ctmPitchOnce_ = movementInfo.pitch;
    }
    startClickToMove(ctm::Kind::FaceDirection, 0, glm::vec3(0.0f), ctm::facingOfDirection(n));
}

uint32_t GameHandler::autoRangedAttackSpell() const {
    // 0x006d71e0: the CVar (0x00bd091c, default "1") and the spell.
    if (!spellHandler_ || addons::storedCVarValue("autoRangedCombat", "1") == "0") return 0;
    return spellHandler_->spellBookRecords().rangedAttack;
}

void GameHandler::clickToMoveAttack(uint64_t guid) {
    // 0x0072ee20, after the attack the right click started (0x00731260).
    auto target = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(guid));
    auto me = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(playerGuid));
    if (!target || !me) return;
    const float reach = ctm::meleeReach(target->getCombatReach(), me->getCombatReach());
    const glm::vec3 mine = toServer(glm::vec3(movementInfo.x, movementInfo.y, movementInfo.z));
    const glm::vec3 theirs = toServer(glm::vec3(target->getX(), target->getY(), target->getZ()));
    const glm::vec3 d = mine - theirs;
    if (glm::dot(d, d) < reach * reach) {
        // In reach already: only turned to it.
        clickToMoveFace(ctm::wrapTwoPi(ctm::facingTo(mine, theirs)));
        return;
    }
    if (!clickToMoveAllowed()) return;
    // The player as the mover with a ranged attack (0x006d71e0) closes in to
    // shoot (kind 11): to nine tenths of that attack's reach at the unit
    // (0x0071b3b0 -> 0x00802c30). Otherwise to melee (kind 10).
    const uint32_t ranged = movementHandler_ && movementHandler_->activeMover() == playerGuid
                                ? autoRangedAttackSpell()
                                : 0;
    std::optional<float> spellRange;
    if (ranged != 0 && spellHandler_) spellRange = spellHandler_->spellReachAt(ranged, playerGuid, guid).max;
    // The place is the unit's put on the world's surface there: 0x00783a40
    // asked at the point (flags 0x100111), the first facet's plane read at
    // the unit's x and y. Nothing there, the unit's own height.
    glm::vec3 at(target->getX(), target->getY(), target->getZ());
    if (groundHeightQuery_) {
        if (const auto z = groundHeightQuery_(at)) at.z = *z;
    }
    startClickToMove(ranged != 0 ? ctm::Kind::AttackGuid : ctm::Kind::AttackPosition, guid, at, 0.0f, spellRange);
}

bool GameHandler::clickToMoveToObject(uint64_t guid, float reach) {
    // 0x0072b730, from an object out of reach (0x0070f680).
    if (!clickToMoveAllowed()) return false;
    auto go = getEntityManager().getEntity(guid);
    if (!go || go->getType() != ObjectType::GAMEOBJECT) return false;
    if (!startClickToMove(ctm::Kind::ObjectInteract, guid, glm::vec3(go->getX(), go->getY(), go->getZ()), 0.0f))
        return false;
    // Set after the start, which has already read the one before.
    ctmObjectReach_ = reach * 0.9f;
    return true;
}

uint32_t GameHandler::skinSpellFor(uint64_t guid) const {
    // 0x0053bce0 by the object's type mask (+8): a player or a corpse
    // (0x90), the insignia spell; a unit (0x8), by 0x00715e50 - the
    // creature's type flags 0x100 herbs, 0x200 ore, 0x8000 parts, else its
    // skin; anything else, none.
    auto e = getEntityManager().getEntity(guid);
    if (!e || !spellHandler_) return 0;
    const spell_book::Records& book = spellHandler_->spellBookRecords();
    if (e->getType() == ObjectType::PLAYER || e->getType() == ObjectType::CORPSE) return book.skinPlayerCorpse;
    const auto* unit = dynamic_cast<const Unit*>(e.get());
    if (!unit) return 0;
    uint32_t typeFlags = 0;
    const auto& cache = getCreatureInfoCache();
    if (const auto it = cache.find(unit->getEntry()); it != cache.end()) typeFlags = it->second.typeFlags;
    const int i = (typeFlags & 0x100u) ? 1 : (typeFlags & 0x200u) ? 2 : (typeFlags & 0x8000u) ? 3 : 0;
    return book.skin[i];
}

void GameHandler::clickDeadUnit(uint64_t guid, bool clickToMove) {
    auto unit = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(guid));
    if (!unit) return;
    // 0x00731260 -> 0x00717b60: dead, its loot not held back (+0xac0, which
    // the client only ever sets to the time it saw the unit die) and
    // lootable - looted. A seated player stands first where autoStand lets
    // it (0x0071b090), else is told it must stand (ERR_LOOT_NOTSTANDING,
    // 0x8e) and nothing more is done.
    if (ctm::lootableNow(static_cast<int32_t>(unit->getHealth()), 0, 0, unit->getDynamicFlags())) {
        if (getStandState() != 0) {
            if (addons::storedCVarValue("autoStand", "1") == "0") {
                playGameErrorSound("ERR_LOOT_NOTSTANDING");
                raiseUiError(interfaceString("ERR_LOOT_NOTSTANDING",
                                             "You need to be standing up to loot something!"));
                return;
            }
            setStandState(0);  // 0x006dcb40(0)
        }
        lootCorpse(guid, clickToMove);
        return;
    }
    // Skinnable (UNIT_FLAG_SKINNABLE, 0x4000000), with a spell for it.
    if ((unit->getUnitFlags() & 0x04000000u) == 0) return;
    const uint32_t spell = skinSpellFor(guid);
    if (spell == 0) return;
    if (unit->getType() == ObjectType::PLAYER) {
        // Not the insignia of one the player may help (0x007293d0).
        if (unitMayAssist(playerGuid, guid)) return;
        skinCorpse(guid, clickToMove, true);
        return;
    }
    // The spell's first skinning effect (95) is the skill it stands for
    // (0x008016c0); the creature asks 1 up to level 10, ten a level past
    // ten under 20, five a level from then on.
    int32_t skill = 0;
    if (const auto it = spellNameCache_.find(spell); it != spellNameCache_.end()) {
        for (int k = 0; k < 3; ++k) {
            if (it->second.effectIds[k] == spell_book::kEffectSkinning) {
                skill = openLockEffectValue(spell, k);
                break;
            }
        }
    }
    const int32_t level = static_cast<int32_t>(unit->getLevel());
    const int32_t need = level < 11 ? 1 : level < 20 ? (level * 5 - 50) * 2 : level * 5;
    if (skill < need) {
        // ERR_USE_LOCKED_WITH_SPELL_KNOWN_SI (0xef): "Requires <spell> <n>".
        sayGameObjectError(go_use::err::kLockedWithSpellKnown, {getSpellName(spell), std::to_string(need)});
        return;
    }
    skinCorpse(guid, clickToMove, true);
}

void GameHandler::skinCorpse(uint64_t guid, bool clickToMove, bool setLoot) {
    // 0x006d6f40: the spell's reach at the corpse (0x00802c30); within it,
    // or without click-to-move, the spell is cast at it (0x0080da40), else
    // the player walks there (0x0072b4a0, kind 9).
    auto subject = getEntityManager().getEntity(guid);
    if (!subject || !spellHandler_) return;
    const uint32_t spell = skinSpellFor(guid);
    const float range = spellHandler_->spellReachAt(spell, playerGuid, guid).max;
    const float dx = movementInfo.x - subject->getX();
    const float dy = movementInfo.y - subject->getY();
    const float dz = movementInfo.z - subject->getZ();
    if (dx * dx + dy * dy + dz * dz < range * range || !clickToMove) {
        if (spell != 0) castSpell(spell, guid);
    } else {
        clickToMoveApproach(guid, ctm::Kind::Skin, range);
    }
    // Whether the loot it gives is taken at once (0x00513700).
    if (setLoot) setAutoLoot(clickAutoLoots());
}

GameHandler::ClickToMoveSteer GameHandler::stepClickToMove(float facingYawDeg, float dtSeconds, bool mouseSteering,
                                                           const ClickToMoveMoverIn& mover) {
    ClickToMoveSteer steer;
    if (ctmPitchOnce_) {
        steer.pitchRad = ctmPitchOnce_;
        ctmPitchOnce_.reset();
    }
    const auto toYawDeg = [](float serverFacing) {
        return core::coords::canonicalToCharacterYawDeg(core::coords::serverToCanonicalYaw(serverFacing));
    };
    if (ctmFaceOnStop_) {
        steer.facingYawDeg = toYawDeg(*ctmFaceOnStop_);
        ctmFaceOnStop_.reset();
    }
    // 0x00734390 runs the step for the active player that is not on a taxi
    // (0x00728f70 ends it when the flag comes).
    if (!isClickToMoving()) return steer;
    if (isOnTaxiFlight()) {
        stopClickToMove();
        return steer;
    }
    auto me = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(playerGuid));
    const uint32_t flags = movementInfo.flags;
    ctm::StepIn in;
    in.pos = toServer(glm::vec3(movementInfo.x, movementInfo.y, movementInfo.z));
    in.facing = ctm::wrapTwoPi(core::coords::canonicalToServerYaw(core::coords::characterYawDegToCanonical(facingYawDeg)));
    in.dtSeconds = dtSeconds;
    in.subject = clickToMoveSubject(ctm_.guid);
    in.swimOrFly = (flags & kSwimOrFly) != 0;
    in.canFly = (flags & static_cast<uint32_t>(MovementFlags::CAN_FLY)) != 0;
    in.mouseSteering = mouseSteering;
    in.movingAny = (flags & 0xfu) != 0;
    in.movingForwardBack = (flags & 0x3u) != 0;
    in.fullSpeedTurning = (movementInfo.flags2 & 0x8u) != 0;
    in.unitTurnRate = movementHandler_->getServerTurnRate();
    const bool walking = (flags & static_cast<uint32_t>(MovementFlags::WALKING)) != 0;
    in.groundSpeed = walking ? movementHandler_->getServerWalkSpeed() : movementHandler_->getServerRunSpeed();
    // 0x00987570: the speed for how it moves now.
    if ((flags & static_cast<uint32_t>(MovementFlags::FLYING)) != 0)
        in.currentSpeed = movementHandler_->getServerFlightSpeed();
    else if ((flags & static_cast<uint32_t>(MovementFlags::SWIMMING)) != 0)
        in.currentSpeed = movementHandler_->getServerSwimSpeed();
    else
        in.currentSpeed = in.groundSpeed;
    // 0x005fa9e0 and the rest of the forward test: alive, not rooted, on no
    // transport (flags 0x100a00), not stood dead (7), on no spline, with
    // control of the character.
    const uint8_t stand = me ? me->getStandState() : 0;
    in.mayMove = me && static_cast<int32_t>(me->getHealth()) > 0 && (flags & 0x100a00u) == 0 && stand != 7 &&
                 !movementHandler_->onMoveSpline(playerGuid) && isServerMovementAllowed();
    in.pitch = mover.pitchRad;
    in.liquidAbove = mover.liquidAbove;
    in.swimDepth = mover.swimDepth;

    const ctm::Kind kindNow = ctm_.kind;
    const ctm::StepOut out = ctm::step(ctm_, in);
    steer.active = true;
    if (out.facing) steer.facingYawDeg = toYawDeg(*out.facing);
    if (out.forward) steer.forward = *out.forward;
    // 0x0071aee0: the step's own stop, an arrival's, and 0x007272c0's for a
    // kind that goes anywhere (0x00adaab8).
    steer.stopForward = (out.forward && !*out.forward) ||
                        (out.outcome != ctm::Outcome::Continue && ctm::row(kindNow).used);
    steer.trackAsMove = kindNow == ctm::Kind::Follow && !autoInteractOn();
    // 0x0072ed40 -> 0x0072ead0: the pitch set and sent, the mover allowed
    // to move by itself (0x005fa060).
    if (out.pitch && isServerMovementAllowed()) {
        movementHandler_->setMoverPitchNow(*out.pitch);
        steer.pitchRad = movementInfo.pitch;
    }
    steer.takeOff = out.takeOff;
    switch (out.outcome) {
    case ctm::Outcome::Continue:
        break;
    case ctm::Outcome::Stop:
        stopClickToMove(false, true);
        steer.active = false;
        break;
    case ctm::Outcome::Arrived:
        // 0x0072b3a0: what is left to do once stopped, the subject faced
        // and the mover stopped; the rest only stop moving and go on.
        if (ctm::arrive(ctm_.kind, ctm_.guid, ctmPending_)) {
            stopClickToMove(true, true);
            if (ctmFaceOnStop_) {
                steer.facingYawDeg = toYawDeg(*ctmFaceOnStop_);
                ctmFaceOnStop_.reset();
            }
            steer.active = false;
        } else {
            steer.forward = false;
        }
        break;
    }
    return steer;
}

void GameHandler::clickToMoveMoverStopped() {
    // 0x0071b1e0, for the active player at rest (0x007413f0 after
    // MSG_MOVE_STOP, MSG_MOVE_STOP_STRAFE or a root acknowledged): one of
    // the actions an arrival left, and the arrival's subject done with.
    const ctm::Taken t = ctm::take(ctmPending_);
    switch (t.action) {
    case ctm::Action::None:
        break;
    case ctm::Action::Attack:
        // 0x006e4950, which starts the attack.
        if (getEntityManager().getEntity(t.guid)) startAutoAttack(t.guid);
        break;
    case ctm::Action::Loot:
        if (getEntityManager().getEntity(t.guid)) lootCorpse(t.guid, false);
        break;
    case ctm::Action::Object:
        if (getEntityManager().getEntity(t.guid)) interactWithGameObject(t.guid);
        break;
    case ctm::Action::Skin:
        // 0x006d6f40, the spell cast where it now is.
        if (getEntityManager().getEntity(t.guid)) skinCorpse(t.guid, false, false);
        break;
    case ctm::Action::Npc:
        if (getEntityManager().getEntity(t.guid)) interactWithNpc(t.guid, false);
        break;
    }
}

void GameHandler::clickToMoveCastStarted(uint32_t spellId, std::optional<uint32_t> targetFlags,
                                         uint64_t targetGuid) {
    // 0x00806700, the caster the active player (0x004cee50): Attributes
    // 0x400000 faces the unit the targets name (flag 0x2), or the target
    // where they name none (0x00bd07b0), through 0x0072b4a0 kind 0.
    const auto it = spellNameCache_.find(spellId);
    if (it == spellNameCache_.end() || (it->second.attr & 0x400000u) == 0) return;
    const bool unitNamed = targetFlags ? (*targetFlags & 0x2u) != 0 : true;
    const uint64_t at = unitNamed && targetGuid != 0 ? targetGuid : getTargetGuid();
    if (at != 0) clickToMoveApproach(at, ctm::Kind::Kind0);
}

void GameHandler::clickToMoveCastSent(uint32_t spellId, uint64_t targetGuid) {
    // 0x007fa2e0, the caster the active player (0x004cee50): a spell with
    // Attributes 0x400000 at a unit (target flag 0x2, a guid) faces it
    // through 0x0072b4a0 kind 0 - for FacingCasterFlags 0x1, only where the
    // unit is before the player already (0x0071bce0 -> 0x0071bc50).
    // The rest - the player as the mover, alive - is 0x0072b4a0's to ask.
    if (targetGuid == 0) return;
    loadSpellNameCache();
    const auto it = spellNameCache_.find(spellId);
    if (it == spellNameCache_.end() || (it->second.attr & 0x400000u) == 0) return;
    if ((it->second.facingCasterFlags & 0x1u) != 0) {
        auto target = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(targetGuid));
        if (!target) return;
        const glm::vec3 me = toServer(glm::vec3(movementInfo.x, movementInfo.y, movementInfo.z));
        const glm::vec3 at = toServer(glm::vec3(target->getX(), target->getY(), target->getZ()));
        if (!ctm::inFrontHalf(me, core::coords::canonicalToServerYaw(movementInfo.orientation), at)) return;
    }
    clickToMoveApproach(targetGuid, ctm::Kind::Kind0);
}

void GameHandler::castVisualEnded(uint64_t caster, uint8_t castCount, uint32_t spellId) {
    if (!castVisuals_.ended(caster, castCount, spellId)) return;
    // 0x007fec00 -> 0x0072e430(spell, not channeling, 1).
    auto unit = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(caster));
    const uint16_t channelField = fieldIndex(UF::UNIT_CHANNEL_SPELL);
    const bool channeling = unit && channelField != 0xFFFF && unit->getField(channelField) != 0;
    if (channeling || caster != playerGuid || !isClickToMoving() || ctm_.kind != ctm::Kind::Kind0) return;
    // Facing the unit it tracked, once, then done (0x00727400 kind 2).
    if (const auto at = clickToMoveSubject(ctm_.guid)) {
        const glm::vec3 me = toServer(glm::vec3(movementInfo.x, movementInfo.y, movementInfo.z));
        startClickToMove(ctm::Kind::Face, 0, glm::vec3(0.0f), ctm::wrapTwoPi(ctm::facingTo(me, *at)));
    }
}

void GameHandler::clickToMoveWatchChannel() {
    // The player's channel spell and channel object, as their changes come
    // in (0x0072bc70 for the spell, 0x0073f4f0 -> 0x0073a520 for the
    // object); each, for the active player, faces the object where it is a
    // unit and the spell has AttributesEx 0x4000 (0x0072b4a0 kind 1).
    auto me = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(playerGuid));
    const uint16_t spellField = fieldIndex(UF::UNIT_CHANNEL_SPELL);
    const uint16_t objectField = fieldIndex(UF::UNIT_FIELD_CHANNEL_OBJECT);
    if (!me || spellField == 0xFFFF || objectField == 0xFFFF) return;
    const uint32_t spell = me->getField(spellField);
    const uint64_t object = static_cast<uint64_t>(me->getField(objectField)) |
                            (static_cast<uint64_t>(me->getField(static_cast<uint16_t>(objectField + 1))) << 32);
    const bool spellChanged = spell != ctmChannelSpellSeen_;
    const bool objectChanged = object != ctmChannelObjectSeen_;
    ctmChannelSpellSeen_ = spell;
    ctmChannelObjectSeen_ = object;
    if ((!spellChanged && !objectChanged) || spell == 0 || object == 0) return;
    const auto it = spellNameCache_.find(spell);
    if (it == spellNameCache_.end() || (it->second.attrEx & 0x4000u) == 0) return;
    clickToMoveApproach(object, ctm::Kind::Kind1);
}

std::optional<std::pair<glm::vec3, float>> GameHandler::clickToMoveMarker() const {
    // 0x007275c0: the ring on the place a Move or an AttackPosition is
    // going to, while it goes.
    if (!isClickToMoving() || (ctm_.kind != ctm::Kind::Move && ctm_.kind != ctm::Kind::AttackPosition))
        return std::nullopt;
    const glm::vec3 canonical = core::coords::serverToCanonical(ctm_.dest);
    return std::make_pair(canonical, ctm::markerRadius(movementHandler_->movementClockMs()));
}

}  // namespace game
}  // namespace wowee
