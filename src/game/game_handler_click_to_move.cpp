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
    // 0x00727400. Whatever the mover was doing ends first, a follow with
    // AUTOFOLLOW_END (0x0081b530(0x166)) - here WoWee's own follow, which is
    // the client's kind 3.
    if (kind != ctm::Kind::Follow && followTargetGuid_ != 0) cancelFollow();
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
    // 0x007272c0. Only the camera can turn the mover and stop it, so the
    // two halves are left for the next steer to carry out.
    (void)stopMoving;  // the steer stops whatever click-to-move started
    if (!isClickToMoving()) return;
    if (ctm_.kind == ctm::Kind::Follow) fireAddonEvent("AUTOFOLLOW_END", {});
    if (faceSubject) {
        if (const auto at = clickToMoveSubject(ctm_.guid)) {
            const glm::vec3 me = toServer(glm::vec3(movementInfo.x, movementInfo.y, movementInfo.z));
            ctmFaceOnStop_ = ctm::wrapTwoPi(ctm::facingTo(me, *at));
        }
    }
    ctm_ = ctm::State{};
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
    default:
        return false;
    }
    if (!playerAliveAsMover()) return false;
    return startClickToMove(kind, guid, glm::vec3(subject->getX(), subject->getY(), subject->getZ()), 0.0f,
                            spellRange);
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
    startClickToMove(ranged != 0 ? ctm::Kind::AttackGuid : ctm::Kind::AttackPosition, guid,
                     glm::vec3(target->getX(), target->getY(), target->getZ()), 0.0f, spellRange);
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

GameHandler::ClickToMoveSteer GameHandler::stepClickToMove(float facingYawDeg, float dtSeconds, bool mouseSteering) {
    ClickToMoveSteer steer;
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

    const ctm::StepOut out = ctm::step(ctm_, in);
    steer.active = true;
    if (out.facing) steer.facingYawDeg = toYawDeg(*out.facing);
    if (out.forward) steer.forward = *out.forward;
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
        // 0x006d6f40, skinning - not a right click this client has.
        break;
    case ctm::Action::Npc:
        if (getEntityManager().getEntity(t.guid)) interactWithNpc(t.guid, false);
        break;
    }
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
