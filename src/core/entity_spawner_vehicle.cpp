// Vehicle passengers: into, in and out of their seats (0x0074a7f0, the
// passenger component's states, 0x007489c0 and its neighbours).
//
// The client's passenger component hangs the passenger's model on the
// vehicle model's attachment for the seat (0x00831630) by the matrix
// 0x007490f0 builds, plays the seat's ride animation in place of the unit's
// own (0x00747b20, from 0x007385c0), hides the passenger where the seat's
// Flags 0x200 put +0x10 0x400 (0x007489c0, read by 0x00730f30), and where
// the seat's FlagsB 0x10000 asks, copies the vehicle's animations onto it and
// hands it each the vehicle plays after (0x00748620, 0x00736d30). A change
// of transport that is moved through flies the passenger along an arc into
// the seat or out of it (0x00749aa0, 0x0074af70, 0x0074a200).
#include "core/entity_spawner.hpp"

#include "core/coordinates.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "game/group_defines.hpp"
#include "game/handler_types.hpp"
#include "game/vehicle_camera.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/vehicle_seat.hpp"

#include <chrono>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace wowee::core {

void EntitySpawner::loadVehicleTables() {
    if (vehicleTablesLoaded_ || !assetManager_ || !assetManager_->isInitialized()) return;
    vehicleTablesLoaded_ = true;
    if (gameHandler_) gameHandler_->vehicleRoster().setTables(&vehicleTables_);
    const auto* layout = pipeline::getActiveDBCLayout();
    const pipeline::DBCFieldMap* vehicleLayout = layout ? layout->getLayout("Vehicle") : nullptr;
    const pipeline::DBCFieldMap* seatLayout = layout ? layout->getLayout("VehicleSeat") : nullptr;
    // Only WotLK's tables have vehicles.
    if (!vehicleLayout || !seatLayout) return;

    auto vehicleDbc = assetManager_->loadDBC("Vehicle.dbc");
    auto seatDbc = assetManager_->loadDBC("VehicleSeat.dbc");
    if (!vehicleDbc || !vehicleDbc->isLoaded() || !seatDbc || !seatDbc->isLoaded()) return;

    auto column = [](const pipeline::DBCFieldMap& map, const pipeline::DBCFile& dbc, const char* name) {
        const uint32_t f = map.tryField(name);
        return f < dbc.getFieldCount() ? f : 0xFFFFFFFFu;
    };
    auto u32 = [](const pipeline::DBCFile& dbc, uint32_t row, uint32_t f, uint32_t fallback) {
        return f != 0xFFFFFFFFu ? dbc.getUInt32(row, f) : fallback;
    };
    auto i32 = [](const pipeline::DBCFile& dbc, uint32_t row, uint32_t f) {
        return f != 0xFFFFFFFFu ? dbc.getInt32(row, f) : -1;
    };
    auto f32 = [](const pipeline::DBCFile& dbc, uint32_t row, uint32_t f) {
        return f != 0xFFFFFFFFu ? dbc.getFloat(row, f) : 0.0f;
    };

    std::array<uint32_t, game::vehicle::kSeatCount> seatColumns{};
    for (uint32_t i = 0; i < game::vehicle::kSeatCount; ++i) {
        seatColumns[i] = column(*vehicleLayout, *vehicleDbc, ("SeatID" + std::to_string(i)).c_str());
    }
    const uint32_t vId = column(*vehicleLayout, *vehicleDbc, "ID");
    const uint32_t vFlags = column(*vehicleLayout, *vehicleDbc, "Flags");
    const uint32_t vPitchMin = column(*vehicleLayout, *vehicleDbc, "PitchMin");
    const uint32_t vPitchMax = column(*vehicleLayout, *vehicleDbc, "PitchMax");
    const uint32_t vFadeMin = column(*vehicleLayout, *vehicleDbc, "CameraFadeDistScalarMin");
    const uint32_t vFadeMax = column(*vehicleLayout, *vehicleDbc, "CameraFadeDistScalarMax");
    const uint32_t vCamPitch = column(*vehicleLayout, *vehicleDbc, "CameraPitchOffset");
    const uint32_t vCamYaw = column(*vehicleLayout, *vehicleDbc, "CameraYawOffset");
    const uint32_t vLocomotion = column(*vehicleLayout, *vehicleDbc, "UiLocomotionType");
    for (uint32_t row = 0; row < vehicleDbc->getRecordCount(); ++row) {
        game::vehicle::VehicleRecord rec;
        rec.id = u32(*vehicleDbc, row, vId, 0);
        rec.flags = u32(*vehicleDbc, row, vFlags, 0);
        rec.pitchMin = f32(*vehicleDbc, row, vPitchMin);
        rec.pitchMax = f32(*vehicleDbc, row, vPitchMax);
        rec.cameraFadeDistScalarMin = f32(*vehicleDbc, row, vFadeMin);
        rec.cameraFadeDistScalarMax = f32(*vehicleDbc, row, vFadeMax);
        rec.cameraPitchOffset = f32(*vehicleDbc, row, vCamPitch);
        rec.cameraYawOffset = f32(*vehicleDbc, row, vCamYaw);
        rec.uiLocomotionType = u32(*vehicleDbc, row, vLocomotion, 0);
        rec.uiIndicatorId = u32(*vehicleDbc, row, column(*vehicleLayout, *vehicleDbc, "VehicleUIIndicatorID"), 0);
        for (uint32_t i = 0; i < game::vehicle::kSeatCount; ++i) {
            rec.seatIds[i] = u32(*vehicleDbc, row, seatColumns[i], 0);
        }
        if (rec.id != 0) vehicleTables_.addVehicle(rec);
    }

    const auto& sl = *seatLayout;
    const auto& sd = *seatDbc;
    const uint32_t sId = column(sl, sd, "ID");
    const uint32_t sFlags = column(sl, sd, "Flags");
    const uint32_t sAttach = column(sl, sd, "AttachmentID");
    const uint32_t sOffX = column(sl, sd, "AttachmentOffsetX");
    const uint32_t sOffY = column(sl, sd, "AttachmentOffsetY");
    const uint32_t sOffZ = column(sl, sd, "AttachmentOffsetZ");
    const uint32_t sRideStart = column(sl, sd, "RideAnimStart");
    const uint32_t sRideLoop = column(sl, sd, "RideAnimLoop");
    const uint32_t sUpperStart = column(sl, sd, "RideUpperAnimStart");
    const uint32_t sUpperLoop = column(sl, sd, "RideUpperAnimLoop");
    const uint32_t sYaw = column(sl, sd, "PassengerYaw");
    const uint32_t sPitch = column(sl, sd, "PassengerPitch");
    const uint32_t sRoll = column(sl, sd, "PassengerRoll");
    const uint32_t sPassAttach = column(sl, sd, "PassengerAttachmentID");
    const uint32_t sFlagsB = column(sl, sd, "FlagsB");
    for (uint32_t row = 0; row < sd.getRecordCount(); ++row) {
        game::vehicle::SeatRecord rec;
        rec.id = u32(sd, row, sId, 0);
        if (rec.id == 0) continue;
        rec.flags = u32(sd, row, sFlags, 0);
        rec.attachmentId = i32(sd, row, sAttach);
        rec.attachmentOffset = glm::vec3(f32(sd, row, sOffX), f32(sd, row, sOffY), f32(sd, row, sOffZ));
        rec.rideAnimStart = i32(sd, row, sRideStart);
        rec.rideAnimLoop = i32(sd, row, sRideLoop);
        rec.rideUpperAnimStart = i32(sd, row, sUpperStart);
        rec.rideUpperAnimLoop = i32(sd, row, sUpperLoop);
        rec.passengerYaw = f32(sd, row, sYaw);
        rec.passengerPitch = f32(sd, row, sPitch);
        rec.passengerRoll = f32(sd, row, sRoll);
        rec.passengerAttachmentId = i32(sd, row, sPassAttach);
        rec.flagsB = u32(sd, row, sFlagsB, 0);
        auto fcol = [&](const char* name) { return f32(sd, row, column(sl, sd, name)); };
        auto icol = [&](const char* name) { return i32(sd, row, column(sl, sd, name)); };
        rec.enterPreDelay = fcol("EnterPreDelay");
        rec.enterSpeed = fcol("EnterSpeed");
        rec.enterGravity = fcol("EnterGravity");
        rec.enterMinDuration = fcol("EnterMinDuration");
        rec.enterMaxDuration = fcol("EnterMaxDuration");
        rec.enterMinArcHeight = fcol("EnterMinArcHeight");
        rec.enterMaxArcHeight = fcol("EnterMaxArcHeight");
        rec.enterAnimStart = icol("EnterAnimStart");
        rec.enterAnimLoop = icol("EnterAnimLoop");
        rec.exitPreDelay = fcol("ExitPreDelay");
        rec.exitSpeed = fcol("ExitSpeed");
        rec.exitGravity = fcol("ExitGravity");
        rec.exitMinDuration = fcol("ExitMinDuration");
        rec.exitMaxDuration = fcol("ExitMaxDuration");
        rec.exitMinArcHeight = fcol("ExitMinArcHeight");
        rec.exitMaxArcHeight = fcol("ExitMaxArcHeight");
        rec.exitAnimStart = icol("ExitAnimStart");
        rec.exitAnimLoop = icol("ExitAnimLoop");
        rec.exitAnimEnd = icol("ExitAnimEnd");
        rec.vehicleEnterAnim = icol("VehicleEnterAnim");
        rec.vehicleExitAnim = icol("VehicleExitAnim");
        rec.vehicleRideAnimLoop = icol("VehicleRideAnimLoop");
        rec.vehicleEnterAnimBone = icol("VehicleEnterAnimBone");
        rec.vehicleExitAnimBone = icol("VehicleExitAnimBone");
        rec.vehicleRideAnimLoopBone = icol("VehicleRideAnimLoopBone");
        rec.cameraEnteringDelay = fcol("CameraEnteringDelay");
        rec.cameraEnteringDuration = fcol("CameraEnteringDuration");
        rec.cameraExitingDelay = fcol("CameraExitingDelay");
        rec.cameraExitingDuration = fcol("CameraExitingDuration");
        rec.cameraOffset = glm::vec3(fcol("CameraOffsetX"), fcol("CameraOffsetY"), fcol("CameraOffsetZ"));
        rec.cameraPosChaseRate = fcol("CameraPosChaseRate");
        rec.cameraFacingChaseRate = fcol("CameraFacingChaseRate");
        rec.cameraEnteringZoom = fcol("CameraEnteringZoom");
        rec.cameraSeatZoomMin = fcol("CameraSeatZoomMin");
        rec.cameraSeatZoomMax = fcol("CameraSeatZoomMax");
        rec.enterUISoundId = u32(sd, row, column(sl, sd, "EnterUISoundID"), 0);
        rec.exitUISoundId = u32(sd, row, column(sl, sd, "ExitUISoundID"), 0);
        rec.uiSkin = u32(sd, row, column(sl, sd, "UiSkin"), 0);
        rec.vehicleAbilityDisplay = u32(sd, row, column(sl, sd, "VehicleAbilityDisplay"), 0);
        rec.vehicleEnterAnimDelay = fcol("VehicleEnterAnimDelay");
        rec.vehicleExitAnimDelay = fcol("VehicleExitAnimDelay");
        vehicleTables_.addSeat(rec);
    }

    // The seat map VehicleMenuBar draws (GetVehicleUIIndicator, 0x00614d90).
    const pipeline::DBCFieldMap* indLayout = layout->getLayout("VehicleUIIndicator");
    const pipeline::DBCFieldMap* indSeatLayout = layout->getLayout("VehicleUIIndSeat");
    auto indDbc = indLayout ? assetManager_->loadDBC("VehicleUIIndicator.dbc") : nullptr;
    auto indSeatDbc = indSeatLayout ? assetManager_->loadDBC("VehicleUIIndSeat.dbc") : nullptr;
    if (indDbc && indDbc->isLoaded()) {
        const uint32_t iId = column(*indLayout, *indDbc, "ID");
        const uint32_t iTex = column(*indLayout, *indDbc, "BackgroundTexture");
        for (uint32_t row = 0; row < indDbc->getRecordCount(); ++row) {
            const uint32_t id = u32(*indDbc, row, iId, 0);
            if (id == 0) continue;
            vehicleTables_.addIndicator(id, iTex != 0xFFFFFFFFu ? indDbc->getString(row, iTex) : std::string());
        }
    }
    if (indSeatDbc && indSeatDbc->isLoaded()) {
        const auto& ql = *indSeatLayout;
        const auto& qd = *indSeatDbc;
        const uint32_t qId = column(ql, qd, "ID");
        const uint32_t qInd = column(ql, qd, "VehicleUIIndicatorID");
        const uint32_t qSeat = column(ql, qd, "VirtualSeatIndex");
        const uint32_t qX = column(ql, qd, "XPos");
        const uint32_t qY = column(ql, qd, "YPos");
        for (uint32_t row = 0; row < qd.getRecordCount(); ++row) {
            game::vehicle::UIIndicatorSeat seat;
            seat.id = u32(qd, row, qId, 0);
            seat.indicatorId = u32(qd, row, qInd, 0);
            seat.virtualSeatIndex = u32(qd, row, qSeat, 0);
            seat.x = f32(qd, row, qX);
            seat.y = f32(qd, row, qY);
            if (seat.id != 0) vehicleTables_.addIndicatorSeat(seat);
        }
    }
}

namespace {

using game::vehicle::SeatState;

uint32_t seatClockMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

/// Whether a time has come (the client's signed difference of its ms clock).
bool reached(uint32_t nowMs, uint32_t atMs) { return static_cast<int32_t>(nowMs - atMs) >= 0; }

float yawOf(const glm::mat4& m) { return std::atan2(m[0][1], m[0][0]); }

bool entersOrSits(SeatState s) {
    return s == SeatState::EnterDelay || s == SeatState::Entering || s == SeatState::Seated;
}

std::optional<rendering::vehicle_seat::RideAnimation> toRide(const std::optional<game::vehicle::SeatAnimation>& a) {
    if (!a) return std::nullopt;
    return rendering::vehicle_seat::RideAnimation{
        .start = a->start, .loop = a->loop, .upperStart = a->upperStart, .upperLoop = a->upperLoop, .seated = a->seated};
}

}  // namespace

uint32_t EntitySpawner::vehicleInstanceFor(uint64_t guid) const {
    if (gameHandler_ && guid == gameHandler_->getPlayerGuid() && mountInstanceId_ != 0) return mountInstanceId_;
    if (const uint32_t mount = remotePlayerMountInstance(guid)) return mount;
    return characterInstanceIdForGuid(guid);
}

glm::vec3 EntitySpawner::unitRenderPosition(uint64_t guid) const {
    if (gameHandler_ && guid == gameHandler_->getPlayerGuid() && renderer_) return renderer_->getCharacterPosition();
    if (gameHandler_) {
        if (auto entity = gameHandler_->getEntityManager().getEntity(guid)) {
            return core::coords::canonicalToRender(glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
        }
    }
    glm::vec3 pos(0.0f);
    if (auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr) {
        cr->getInstancePosition(characterInstanceIdForGuid(guid), pos);
    }
    return pos;
}

void EntitySpawner::releaseSeatedPassenger(uint64_t guid, SeatedPassenger& seated, bool landing) {
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!cr || seated.instanceId == 0) return;
    const uint32_t id = seated.instanceId;
    cr->setInstanceSeatFlight(id, std::nullopt);
    cr->clearInstanceSeat(id);
    cr->setSeatAnimation(id, std::nullopt);
    if (seated.hidden) cr->setInstanceVisible(id, true);
    seated.hidden = false;
    // Back where its own movement has it.
    if (!(gameHandler_ && guid == gameHandler_->getPlayerGuid())) cr->setInstancePosition(id, unitRenderPosition(guid));
    // Out of state 5 it lands by the seat's ExitAnimEnd or as it moves
    // (0x007489c0 -> 0x0074b9c0); otherwise it animates as itself again
    // (0x0073ac30).
    std::optional<game::vehicle::LandingAnimation> land;
    if (landing) {
        uint32_t moveFlags = 0;
        if (gameHandler_) {
            if (auto entity = gameHandler_->getEntityManager().getEntity(guid)) moveFlags = entity->getReportedMoveFlags();
        }
        land = game::vehicle::landingAnimation(vehicleTables_.seatById(seated.seatId), (moveFlags & 0xf) != 0,
                                               (moveFlags & 0x1000) != 0, (moveFlags & 0x200000) != 0,
                                               (moveFlags & 0x2000000) != 0);
    }
    if (land) cr->playAnimation(id, land->id, !land->oneShot);
    else cr->playAnimation(id, rendering::anim::STAND, true);
}

uint64_t EntitySpawner::seatRootOf(uint64_t guid) const {
    if (!gameHandler_) return 0;
    const auto it = seatedPassengers_.find(guid);
    if (it != seatedPassengers_.end() && it->second.state != SeatState::None) return it->second.rootGuid;
    auto& entities = gameHandler_->getEntityManager();
    return gameHandler_->vehicleRoster().rootVehicle(
        guid, 0, [&entities](uint64_t g) { return entities.getEntity(g) != nullptr; });
}

void EntitySpawner::setSeatState(uint64_t guid, SeatedPassenger& p, SeatState state, uint32_t nowMs) {
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    const SeatState old = p.state;
    const game::vehicle::SeatRecord* seat = vehicleTables_.seatById(p.seatId);
    // Whatever it mirrored, it mirrors no more (+0x10 0x800, 0x007489c0).
    if (cr && p.instanceId) cr->setMirrorsSeatParent(p.instanceId, false);
    // Moving on, from where it is drawn now (0x007446c0) and as it faces.
    if (state != SeatState::None && state != SeatState::Seated) {
        glm::vec3 drawn = unitRenderPosition(guid);
        float drawnYaw = p.drawnFacing;
        if (cr && p.instanceId) {
            cr->getInstancePosition(p.instanceId, drawn);
            glm::mat4 frame(1.0f);
            if (cr->getInstanceFrame(p.instanceId, frame)) drawnYaw = yawOf(frame);
        }
        p.from = drawn;
        // +0x40: the unit's transport - into a seat, the vehicle.
        p.anchorGuid = entersOrSits(state) ? p.vehicleGuid : 0;
        if (old == SeatState::None || old == SeatState::Seated) {
            p.startFacing = p.drawnFacing = drawnYaw;
            if (auto passenger = gameHandler_->vehicleRoster().passenger(guid)) p.seatFacing = passenger->facing;
        } else {
            p.seatFacing = p.startFacing = p.drawnFacing;
        }
    }
    // Out of its seat, the vehicle's animation for it stops (0x007489c0 ->
    // 0x007577e0); into it, it starts (0x00756f80).
    if (old == SeatState::Seated && state != SeatState::Seated) stopVehicleRideLoop(guid, p);
    p.state = state;
    p.startMs = nowMs;
    p.endMs = nowMs;
    p.planned = false;
    gameHandler_->vehicleRoster().setSeatState(guid, static_cast<uint8_t>(state));
    if (old != state) fireSeatEvents(guid, p, old, state);
    // The mover's aim is shown again for its new state (0x007489c0 ->
    // 0x0074c5a0); the mover here is the player.
    if (old != state && guid == gameHandler_->getPlayerGuid()) {
        gameHandler_->announceVehicleAim();
    }
    // VEHICLE_PASSENGERS_CHANGED (0x007489c0): into or out of a seat - to or
    // from states 0 and 3 - of a vehicle whose seats count from the one the
    // player's do (0x004f6230).
    if (old != state && (old == SeatState::None || state == SeatState::None || old == SeatState::Seated ||
                         state == SeatState::Seated)) {
        const uint64_t vehicle = p.vehicleGuid != 0 ? p.vehicleGuid : p.prevVehicleGuid;  // 0x00747ff0
        const uint64_t root = vehicle != 0 ? seatRootOf(vehicle) : 0;
        if (root != 0 && root == seatRootOf(gameHandler_->getPlayerGuid())) {
            gameHandler_->fireAddonEvent("VEHICLE_PASSENGERS_CHANGED", {});
        }
    }
    if (state == SeatState::Seated && old != SeatState::Seated && seat) startVehicleRideLoop(p, *seat);
    if (!cr || p.instanceId == 0) return;
    // What it plays: the state's animation, the start once more but from 1
    // to 2 and out of 4 (0x007489c0 clears +0x10 0x6, 0x00747b20).
    if (seat && state != SeatState::None && !game::vehicle::keepsAnimation(old, state)) {
        cr->setSeatAnimation(p.instanceId, toRide(game::vehicle::seatAnimation(state, *seat, p.voluntary)));
    }
    // Flags 0x200 hides it seated, and while it moves to another seat of
    // the vehicle it is hidden in (0x007489c0 -> +0x10 0x400).
    const bool hide = seat && (seat->flags & game::vehicle::kSeatFlagHidePassenger) != 0 &&
                      (state == SeatState::Seated ||
                       ((state == SeatState::EnterDelay || state == SeatState::Entering) && p.hidden &&
                        p.prevVehicleGuid == p.vehicleGuid));
    if (hide != p.hidden) cr->setInstanceVisible(p.instanceId, !hide);
    p.hidden = hide;
}

void EntitySpawner::startVehicleRideLoop(SeatedPassenger& p, const game::vehicle::SeatRecord& seat) {
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    // Seat Flags 0x20000 and a VehicleRideAnimLoop below 0x1fa (0x007489c0).
    if (!cr || (seat.flags & game::vehicle::kSeatFlagHasVehicleRideAnim) == 0 || seat.vehicleRideAnimLoop < 0 ||
        seat.vehicleRideAnimLoop >= game::vehicle::kNoAnimation)
        return;
    const uint32_t vehicleInstance = vehicleInstanceFor(p.vehicleGuid);
    if (vehicleInstance == 0) return;
    // 0x00756f80: on the VehicleRideAnimLoopBone key bone - past 0x22 it is
    // 0x1a, the whole model.
    const int32_t bone = game::vehicle::vehicleAnimBone(seat.vehicleRideAnimLoopBone);
    const auto anim = static_cast<uint32_t>(seat.vehicleRideAnimLoop);
    if (bone == game::vehicle::kWholeModelBone) cr->playAnimation(vehicleInstance, anim, true);
    else if (!cr->playVehicleKeyBoneAnimation(vehicleInstance, bone, anim)) return;
    p.rideLoopVehicleInstance = vehicleInstance;
    p.rideLoopBone = bone;
}

void EntitySpawner::stopVehicleRideLoop(uint64_t guid, SeatedPassenger& p) {
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    const uint32_t vehicleInstance = std::exchange(p.rideLoopVehicleInstance, 0);
    const int32_t bone = std::exchange(p.rideLoopBone, -1);
    if (!cr || vehicleInstance == 0) return;
    // 0x007577e0: not while another passenger still holds it on that bone.
    for (const auto& [other, q] : seatedPassengers_) {
        if (other != guid && q.rideLoopVehicleInstance == vehicleInstance && q.rideLoopBone == bone) return;
    }
    // The bone back to the body (0x00735a60); where that cannot be, the
    // vehicle animates as itself again (0x00757280, 0x0073ac30).
    if (bone != game::vehicle::kWholeModelBone && cr->stopKeyBoneAnimation(vehicleInstance, bone)) return;
    uint32_t own = rendering::anim::STAND;
    if (auto entity = gameHandler_ ? gameHandler_->getEntityManager().getEntity(p.vehicleGuid) : nullptr) {
        if (entity->isActivelyMoving()) own = entity->movesAtWalkPace() ? rendering::anim::WALK : rendering::anim::RUN;
    }
    cr->playAnimation(vehicleInstance, own, true);
}

std::string EntitySpawner::soundEntryName(uint32_t id) {
    if (!soundEntryNamesLoaded_ && assetManager_ && assetManager_->isInitialized()) {
        soundEntryNamesLoaded_ = true;
        const auto* layout = pipeline::getActiveDBCLayout();
        const pipeline::DBCFieldMap* map = layout ? layout->getLayout("SoundEntries") : nullptr;
        auto dbc = map ? assetManager_->loadDBC("SoundEntries.dbc") : nullptr;
        if (dbc && dbc->isLoaded()) {
            const uint32_t idCol = map->tryField("ID");
            const uint32_t nameCol = map->tryField("Name");
            if (idCol < dbc->getFieldCount() && nameCol < dbc->getFieldCount()) {
                for (uint32_t row = 0; row < dbc->getRecordCount(); ++row) {
                    soundEntryNames_[dbc->getUInt32(row, idCol)] = dbc->getString(row, nameCol);
                }
            }
        }
    }
    const auto it = soundEntryNames_.find(id);
    return it != soundEntryNames_.end() ? it->second : std::string();
}

void EntitySpawner::fireSeatEvents(uint64_t guid, const SeatedPassenger& p, SeatState from, SeatState to) {
    if (!gameHandler_) return;
    // The unit's tokens (0x0060bb70): the player, a party or raid member.
    std::vector<std::string> units;
    if (guid == gameHandler_->getPlayerGuid()) units.emplace_back("player");
    const auto& group = gameHandler_->getPartyData();
    if (game::groupIsRaid(group.groupType)) {
        for (size_t i = 0; i < group.members.size(); ++i) {
            if (group.members[i].guid == guid) units.push_back("raid" + std::to_string(i + 1));
        }
    } else {
        int n = 0;
        for (const auto& m : group.members) {
            if (m.guid == gameHandler_->getPlayerGuid()) continue;
            ++n;
            if (m.guid == guid) units.push_back("party" + std::to_string(n));
        }
    }
    if (units.empty()) return;
    // 0x00749fb0: which of the four a change from one state to another says.
    constexpr uint32_t kEntering = 1, kEntered = 2, kExiting = 4, kExited = 8;
    uint32_t which = 0;
    switch (to) {
        case SeatState::None: which = from == SeatState::Seated ? kExiting | kExited : kExited; break;
        case SeatState::EnterDelay: which = kEntering; break;
        case SeatState::Entering: which = from == SeatState::EnterDelay ? 0 : kEntering; break;
        case SeatState::Seated:
            which = from != SeatState::None && from != SeatState::Seated ? kEntered : kEntering | kEntered;
            break;
        case SeatState::ExitDelay: which = kExiting; break;
        case SeatState::Exiting: which = from == SeatState::ExitDelay ? 0 : kExiting; break;
    }
    const game::vehicle::SeatRecord* seat = vehicleTables_.seatById(p.seatId);
    const game::vehicle::VehicleRecord* vehicle =
        vehicleTables_.vehicle(gameHandler_->vehicleRoster().vehicleId(p.vehicleGuid));
    // 0x00748810's arguments: the unit; whether the seat has the vehicle UI
    // (Flags 0x20000000); its UiSkin; its EnterUISoundID's name; whether it is a control seat (Flags
    // 0x800); the vehicle's VehicleUIIndicatorID.
    auto entering = [&](const char* event) {
        static constexpr const char* kSkins[2] = {"Natural", "Mechanical"};
        for (const auto& unit : units) {
            gameHandler_->fireAddonEvent(
                event, {unit,
                        game::eventBool(seat && (seat->flags & game::vehicle::kSeatFlagHasVehicleUI) != 0),
                        seat && seat->uiSkin < 2 ? kSkins[seat->uiSkin] : "",
                        seat ? soundEntryName(seat->enterUISoundId) : std::string(),
                        game::eventBool(seat && (seat->flags & 0x800) != 0),
                        std::to_string(vehicle ? vehicle->uiIndicatorId : 0u)});
        }
    };
    if (which & kEntering) entering("UNIT_ENTERING_VEHICLE");
    if (which & kEntered) entering("UNIT_ENTERED_VEHICLE");
    if (which & kExiting) {
        for (const auto& unit : units) gameHandler_->fireAddonEvent("UNIT_EXITING_VEHICLE", {unit});
    }
    if (which & kExited) {
        // The unit and its seat's ExitUISoundID's name (0x00749650 0x8).
        const std::string sound = seat ? soundEntryName(seat->exitUISoundId) : std::string();
        for (const auto& unit : units) gameHandler_->fireAddonEvent("UNIT_EXITED_VEHICLE", {unit, sound});
    }
}

void EntitySpawner::planSeatState(uint64_t guid, SeatedPassenger& p, uint32_t nowMs) {
    p.planned = true;
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    const game::vehicle::SeatRecord* seat = vehicleTables_.seatById(p.seatId);
    if (!cr || !seat || p.state == SeatState::None || p.state == SeatState::Seated) return;
    // +0x90: where it began, on its transport.
    glm::vec3 anchorPos(0.0f);
    if (p.anchorGuid != 0) cr->getInstancePosition(vehicleInstanceFor(p.anchorGuid), anchorPos);
    p.fromLocal = p.from - anchorPos;
    // Where it goes (0x00749e40): the seat entering, else the unit.
    glm::vec3 target = unitRenderPosition(guid);
    if (p.state == SeatState::Entering && p.instanceId) {
        if (const auto seatAt = cr->seatTargetMatrix(p.instanceId)) target = glm::vec3((*seatAt)[3]);
    }
    switch (p.state) {
        case SeatState::EnterDelay:
            p.endMs = p.startMs + game::vehicle::preDelayMs(seat->enterPreDelay);
            break;
        case SeatState::ExitDelay:
            p.endMs = p.startMs + game::vehicle::preDelayMs(seat->exitPreDelay);
            break;
        default: {
            const bool entering = p.state == SeatState::Entering;
            const glm::vec3 way = target - p.from;
            const float distance = glm::length(way);
            std::optional<float> along;
            if (entering && p.vehicleGuid != 0) {
                // The seat's speed less the vehicle's own along the way: its
                // movement's direction times its speed (0x006fed70), on the
                // render axes.
                glm::vec3 velocity(0.0f);
                if (auto vehicle = gameHandler_->getEntityManager().getEntity(p.vehicleGuid)) {
                    velocity = core::coords::canonicalToRender(vehicle->getMoveVelocity());
                }
                along = distance > 0.0001f ? seat->enterSpeed - glm::dot(velocity, way / distance) : 0.0f;
            }
            p.arc = game::vehicle::planArc(*seat, entering, distance, along);
            p.endMs = p.startMs + p.arc.durationMs;
            // The local player's camera takes the flight's time where its
            // own is not final (0x0074a200 -> 0x0075af00).
            if (gameHandler_ && guid == gameHandler_->getPlayerGuid() && localSeatCamera_.pending &&
                !localSeatCamera_.fixed) {
                localSeatCamera_.durationMs = p.arc.durationMs;
                localSeatCamera_.fixed = true;
            }
            // 0x0075af00: its vehicle camera's too, and the way in or out
            // it is on takes it.
            if (gameHandler_ && guid == gameHandler_->getPlayerGuid() && localVehicleCamera_.active &&
                !localVehicleCamera_.fixed) {
                auto& vc = localVehicleCamera_;
                vc.fixed = true;
                vc.durationMs = p.arc.durationMs;
                if (vc.clock.mode == game::vehicle::CameraMode::Entering ||
                    vc.clock.mode == game::vehicle::CameraMode::Exiting)
                    vc.clock.ms = p.arc.durationMs;
            }
            break;
        }
    }
    // 0x0074ad70: more than 150 yards to go, it is there at once.
    const glm::vec3 d = target - p.from;
    if (glm::dot(d, d) > 22500.0f) {
        setSeatState(guid, p, entersOrSits(p.state) ? SeatState::Seated : SeatState::None, nowMs);
        p.planned = true;
    }
}

std::optional<game::vehicle::SeatState> EntitySpawner::localPassengerState() const {
    if (!gameHandler_) return std::nullopt;
    const auto it = seatedPassengers_.find(gameHandler_->getPlayerGuid());
    if (it == seatedPassengers_.end() || it->second.state == SeatState::None) return std::nullopt;
    return it->second.state;
}

std::optional<glm::vec3> EntitySpawner::localPassengerDrawnPosition() const {
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!cr || !localPassengerState()) return std::nullopt;
    const auto it = seatedPassengers_.find(gameHandler_->getPlayerGuid());
    glm::vec3 pos(0.0f);
    if (!cr->getInstancePosition(it->second.instanceId, pos)) return std::nullopt;
    return pos;
}

glm::vec3 EntitySpawner::framePosition(uint64_t guid) const {
    glm::vec3 pos(0.0f);
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    const uint32_t instance = guid ? vehicleInstanceFor(guid) : 0;
    if (cr && instance) cr->getInstancePosition(instance, pos);
    return pos;
}

std::optional<EntitySpawner::VehicleCameraView> EntitySpawner::localVehicleCameraView(bool keysTurn, bool freeLook) {
    using game::vehicle::CameraMode;
    auto& vc = localVehicleCamera_;
    if (!vc.active || !gameHandler_) {
        vc.active = false;
        vc.snap = true;
        return std::nullopt;
    }
    auto* cr = renderer_->getCharacterRenderer();
    const uint64_t player = gameHandler_->getPlayerGuid();
    // The passenger record may be gone while the camera runs out its way
    // out (0x0074b0b0 frees the one, 0x0075af40 ends the other): it looks
    // at the unit where it is then.
    const auto pit = seatedPassengers_.find(player);
    const SeatedPassenger* p = pit != seatedPassengers_.end() ? &pit->second : nullptr;
    const auto drawnPassenger = localPassengerDrawnPosition();
    const glm::vec3 drawn = drawnPassenger ? *drawnPassenger : unitRenderPosition(player);
    const uint32_t now = seatClockMs();
    const float dt = static_cast<float>(now - vc.lastMs) * 0.001f;
    vc.lastMs = now;
    // 0x0075af40: its mode moves on with its time; leaving, it ends.
    game::vehicle::stepCameraMode(vc.clock, now, vc.fixed, vc.durationMs);
    const CameraMode mode = vc.clock.mode;
    if (mode == CameraMode::Off) {
        vc.active = false;
        vc.snap = true;
        return std::nullopt;
    }
    const game::vehicle::SeatRecord* seat = vehicleTables_.seatById(vc.seatId);
    // With no passenger record it is on no vehicle, and pulls in from none
    // (0x00759580 finds no vehicle for the unit).
    const uint64_t vehicleGuid = p ? (p->vehicleGuid != 0 ? p->vehicleGuid : p->prevVehicleGuid) : 0;
    const uint32_t vehicleInstance = vehicleGuid ? vehicleInstanceFor(vehicleGuid) : 0;
    glm::vec3 vehiclePos(0.0f);
    glm::mat4 vehicleFrame(1.0f);
    const bool haveVehicle = seat && vehicleInstance != 0 && cr->getInstancePosition(vehicleInstance, vehiclePos) &&
                             cr->getInstanceFrame(vehicleInstance, vehicleFrame);
    // The unit's own place (vfunc +0x30, 0x004f4460): on its transport - the
    // vehicle into its seat and until it leaves it - the vehicle's place and
    // the offset its movement holds on it, turned by the vehicle; else where
    // it is.
    glm::vec3 own = drawn;
    const bool onVehicle = p && (p->state == SeatState::EnterDelay || p->state == SeatState::Entering ||
                                 p->state == SeatState::Seated || p->state == SeatState::ExitDelay);
    if (haveVehicle && onVehicle) {
        glm::vec3 offset(0.0f);
        const auto& attachments = gameHandler_->transportAttachmentsRef();
        if (const auto it = attachments.find(player);
            it != attachments.end() && it->second.transportGuid == vehicleGuid)
            offset = core::coords::canonicalToServer(it->second.localOffset);
        own = vehiclePos + game::vehicle::transportOffsetAbout(offset, yawOf(vehicleFrame));
    }
    // The unit it watches faces (0x0075a1c0): the vehicle from a control
    // seat (0x0075a930), else the unit itself.
    const bool watchesVehicle = vc.watchesVehicle && haveVehicle;
    float facing = vc.facing.target;
    if (watchesVehicle) {
        facing = yawOf(vehicleFrame);
    } else if (glm::mat4 frame(1.0f);
               cr->getInstanceFrame(p ? p->instanceId : characterInstanceIdForGuid(player), frame)) {
        facing = yawOf(frame);
    }
    // 0x00759200: what it looks at. Waiting, entering or seated, the seat's
    // CameraOffset about the unit it watches, where it is and as it faces -
    // the vehicle's where FlagsB 0x200 asks or it watches the vehicle -
    // from its own place, or with FlagsB 0x100 from where the seat draws it
    // (0x00749d80); chased at 20 / CameraPosChaseRate in the frame of what
    // the unit it watches is on (0x00759d80, 0x00759bd0): its vehicle, or
    // for the vehicle its own transport. Leaving, the unit's own place.
    glm::vec3 target = own;
    const bool seatModes =
        mode == CameraMode::EnterWait || mode == CameraMode::Entering || mode == CameraMode::Seated;
    if (seatModes && haveVehicle) {
        glm::vec3 at = (seat->flagsB & 0x100) != 0 ? drawn : own;
        float yaw = facing;
        if (watchesVehicle || (seat->flagsB & 0x200) != 0) {
            at = vehiclePos;
            yaw = yawOf(vehicleFrame);
        }
        const glm::vec3 chaseOrigin = watchesVehicle ? framePosition(vehicleTransportOf(vehicleGuid)) : vehiclePos;
        const glm::vec3 rel = at + game::vehicle::seatCameraOffset(*seat, yaw) - chaseOrigin;
        if (const auto rate = game::vehicle::seatChaseRate(seat->cameraPosChaseRate); rate && !vc.snap) {
            game::vehicle::chaseStep(vc.rel, vc.relVelocity, rel, *rate, dt);
        } else {
            vc.rel = rel;
            vc.relVelocity = glm::vec3(0.0f);
        }
        target = chaseOrigin + vc.rel;
    }
    // The way there (0x00759d80 with 0x007598f0): held where it looked at
    // the change while it waits; on the way in or out, from there to the
    // point by how far along it is - evenly, or sprung with FlagsB 0x20000
    // in and 0x40000 out.
    const glm::vec3 framePos = framePosition(vc.startFrame);
    const glm::vec3 from = vc.start + framePos;
    float t = 1.0f;
    if (mode != CameraMode::Seated) {
        // Its own way's share (+0x48).
        float share = 1.0f;
        if (p && p->endMs != p->startMs)
            share = std::clamp(static_cast<float>(now - p->startMs) / static_cast<float>(p->endMs - p->startMs), 0.0f,
                               1.0f);
        t = game::vehicle::cameraModeProgress(vc.clock, now, p ? p->state : SeatState::None, seat, share);
    }
    switch (mode) {
        case CameraMode::Seated:
            vc.point = target;
            break;
        case CameraMode::EnterWait:
        case CameraMode::ExitWait:
            vc.point = from;
            break;
        default:
            if (game::vehicle::cameraSpringsOnTheWay(mode, seat)) {
                const glm::vec3 rel = target - framePos;
                if (t >= 0.999999f) {
                    vc.spring = rel;
                    vc.springVelocity = glm::vec3(0.0f);
                    vc.point = target;
                } else {
                    game::vehicle::chaseStep(vc.spring, vc.springVelocity, rel,
                                             game::vehicle::cameraWaySpringRate(vc.clock.ms, t), dt);
                    vc.point = vc.spring + framePos;
                }
            } else {
                vc.point = from + (target - from) * t;
            }
            break;
    }
    // 0x0075a1c0: its facing, in every mode, and the camera turned with it.
    const float turned = game::vehicle::stepCameraFacing(vc.facing, facing, mode, seat, t, vc.clock.ms, vc.snap, dt,
                                                         keysTurn, freeLook);
    vc.snap = false;
    // The pull-in (0x00759580) looks from the vehicle in every mode.
    std::optional<glm::vec3> origin;
    if (haveVehicle) {
        float vehicleHeight = 0.0f;
        if (!cr->getInstanceHeight(vehicleInstance, vehicleHeight)) vehicleHeight = 0.0f;
        origin = vehiclePos + glm::vec3(0.0f, 0.0f, game::vehicle::vehicleCameraHeight(vehicleHeight));
    }
    return VehicleCameraView{.pivot = vc.point, .turned = turned, .collisionOrigin = origin};
}

uint64_t EntitySpawner::vehicleTransportOf(uint64_t vehicleGuid) const {
    // The vehicle's own transport (vfunc +0x40); none on the world.
    if (!gameHandler_ || vehicleGuid == 0) return 0;
    const auto& attachments = gameHandler_->transportAttachmentsRef();
    const auto it = attachments.find(vehicleGuid);
    return it != attachments.end() ? it->second.transportGuid : 0;
}

std::optional<EntitySpawner::CameraFadeVehicle> EntitySpawner::localCameraFadeVehicle() {
    if (!gameHandler_) return std::nullopt;
    const uint64_t player = gameHandler_->getPlayerGuid();
    uint64_t vehicle = 0;
    // Seated (state 3), its vehicle; else the vehicle it drives.
    if (const auto it = seatedPassengers_.find(player);
        it != seatedPassengers_.end() && it->second.state == SeatState::Seated) {
        vehicle = it->second.vehicleGuid;
    } else if (const uint64_t mover = gameHandler_->activeMover();
               mover != player && gameHandler_->vehicleRoster().vehicleId(mover) != 0) {
        vehicle = mover;
    }
    if (vehicle == 0) return std::nullopt;
    const uint32_t instance = vehicleInstanceFor(vehicle);
    if (instance == 0) return std::nullopt;
    CameraFadeVehicle fade{.instanceId = instance};
    // 0x00737390 goes down the vehicle's seats, and down theirs where a
    // passenger is a vehicle too.
    std::vector<uint64_t> carriers{vehicle};
    for (size_t i = 0; i < carriers.size() && i < 64; ++i) {
        for (const auto& [guid, sp] : seatedPassengers_) {
            if (sp.state != SeatState::Seated || sp.vehicleGuid != carriers[i] || sp.instanceId == 0) continue;
            fade.riders.push_back(sp.instanceId);
            carriers.push_back(guid);
        }
    }
    if (const auto* row = vehicleTables_.vehicle(gameHandler_->vehicleRoster().vehicleId(vehicle));
        row && (row->flags & game::vehicle::kVehicleFlagCameraFade) != 0) {
        fade.scalars = glm::vec2(row->cameraFadeDistScalarMin, row->cameraFadeDistScalarMax);
    }
    return fade;
}

std::optional<EntitySpawner::SeatCameraCue> EntitySpawner::takeSeatCameraCue() {
    if (!localSeatCamera_.pending || !gameHandler_) return std::nullopt;
    if (!reached(seatClockMs(), localSeatCamera_.startMs)) return std::nullopt;
    if (localSeatCamera_.waitsForFlight && !localSeatCamera_.fixed) return std::nullopt;
    localSeatCamera_.pending = false;
    SeatCameraCue cue;
    cue.seconds = static_cast<float>(localSeatCamera_.durationMs) * 0.001f;
    const auto it = seatedPassengers_.find(gameHandler_->getPlayerGuid());
    if (it != seatedPassengers_.end()) {
        // In a seat, or on its way into one (0x0074c0e0's test).
        const SeatState s = it->second.state;
        cue.seated = s == SeatState::EnterDelay || s == SeatState::Entering || s == SeatState::Seated;
        cue.seat = vehicleTables_.seatById(it->second.seatId);
        cue.vehicle = vehicleTables_.vehicle(gameHandler_->vehicleRoster().vehicleId(it->second.vehicleGuid));
    }
    return cue;
}

bool EntitySpawner::holdVehicleMove(uint64_t mover, uint64_t transport, uint8_t seatIndex) {
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!cr || !gameHandler_) return false;
    loadVehicleTables();
    const auto& roster = gameHandler_->vehicleRoster();
    // 0x0074bcb0: a change of transport or seat, into a vehicle's or out of one.
    const auto current = roster.passenger(mover);
    if (current && current->vehicleGuid == transport && current->seat == seatIndex) return false;
    const bool entering = game::vehicle::isVehicleGuid(transport) || game::vehicle::isPlayerGuid(transport);
    uint64_t vehicle = transport;
    if (!entering) {
        if (!current) return false;
        vehicle = current->vehicleGuid;
        seatIndex = current->seat;
    }
    const game::vehicle::SeatRecord* seat = vehicleTables_.seat(roster.vehicleId(vehicle), seatIndex);
    if (!seat) return false;
    const auto anim = game::vehicle::vehicleSeatAnim(*seat, entering, (roster.moveFlags2(mover) & 0x40) != 0);
    const uint32_t vehicleInstance = anim ? vehicleInstanceFor(vehicle) : 0;
    // 0x00756ca0: the vehicle's model has it.
    if (!anim || vehicleInstance == 0 || !cr->hasAnimation(vehicleInstance, anim->animation)) return false;
    // 0x0074be10 -> 0x00756f80: the vehicle plays it once on its bone, or
    // as a whole past 0x22.
    if (anim->bone == game::vehicle::kWholeModelBone) {
        cr->playAnimation(vehicleInstance, anim->animation, false);
    } else if (!cr->playVehicleKeyBoneAnimation(vehicleInstance, anim->bone, anim->animation, true)) {
        return false;
    }
    // 0x007479e0: leaving, its ride animation on that bone is the exit's now.
    if (!entering) {
        if (auto it = seatedPassengers_.find(mover);
            it != seatedPassengers_.end() && it->second.rideLoopVehicleInstance == vehicleInstance &&
            it->second.rideLoopBone == anim->bone) {
            it->second.rideLoopVehicleInstance = 0;
            it->second.rideLoopBone = -1;
        }
    }
    const uint32_t now = seatClockMs();
    HeldVehicleMove held;
    held.vehicleGuid = vehicle;
    held.vehicleInstance = vehicleInstance;
    held.bone = anim->bone;
    held.animation = anim->animation;
    held.timed = anim->timed;
    held.startMs = now;
    held.releaseMs = now + anim->waitMs;  // 0x00748070: +0x64
    if (!held.timed) {
        // The first of "$VG0"/"$VT0" (any seat) and the seat's own.
        for (const uint32_t id : {anim->eventBase, anim->eventBase + ((static_cast<uint32_t>(seatIndex) + 1u) << 24)}) {
            const auto at = cr->getAnimationEventTime(vehicleInstance, anim->animation, id);
            if (at && *at != 0 && (!held.eventMs || *at < *held.eventMs)) held.eventMs = *at;
        }
    }
    heldVehicleMoves_[mover] = held;
    return true;
}

void EntitySpawner::stepHeldVehicleMoves(uint32_t nowMs) {
    if (heldVehicleMoves_.empty()) return;
    auto* cr = renderer_->getCharacterRenderer();
    std::vector<uint64_t> release;
    for (const auto& [mover, held] : heldVehicleMoves_) {
        bool go = reached(nowMs, held.releaseMs);
        if (!held.timed && !go) {
            // At the event for its seat (0x00757060 / 0x007570f0), or as the
            // animation ends or stops (0x00757280).
            const uint32_t elapsed = nowMs - held.startMs;
            if (held.eventMs && elapsed >= *held.eventMs) go = true;
            if (held.bone == game::vehicle::kWholeModelBone) {
                uint32_t playing = 0;
                float time = 0.0f;
                float duration = 0.0f;
                if (!cr->getAnimationState(held.vehicleInstance, playing, time, duration) ||
                    playing != held.animation || time >= duration)
                    go = true;
            } else if (!cr->keyBoneAnimationTime(held.vehicleInstance, held.bone, held.animation)) {
                go = true;
            }
        }
        if (go) release.push_back(mover);
    }
    for (const uint64_t mover : release) {
        heldVehicleMoves_.erase(mover);
        gameHandler_->releaseVehicleMove(mover);  // 0x00748230 -> 0x00747990
    }
}

void EntitySpawner::updateVehiclePassengers() {
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!cr || !gameHandler_) return;
    stepHeldVehicleMoves(seatClockMs());
    auto& roster = gameHandler_->vehicleRoster();
    if (roster.passengers().empty() && seatedPassengers_.empty()) return;
    loadVehicleTables();
    const uint32_t now = seatClockMs();

    // A change of transport (0x0074b200 -> 0x00749aa0): where it comes from
    // (+0x20), where it goes (+0x28), the seat its moves read (+0x54), and
    // the state it starts.
    auto change = [&](uint64_t guid, SeatedPassenger& p, uint64_t vehicleGuid, uint8_t seatIndex, uint32_t seatId) {
        const auto how = roster.takeChange(guid).value_or(game::vehicle::VehicleRoster::SeatChange{});
        switch (p.state) {
            case SeatState::None:
            case SeatState::Exiting:
                p.prevVehicleGuid = 0;
                p.prevSeat = 0xff;
                break;
            case SeatState::Entering:
            case SeatState::Seated:
                p.prevVehicleGuid = p.vehicleGuid;
                p.prevSeat = p.seat;
                break;
            default:
                break;
        }
        const bool leaving = vehicleGuid == 0;
        if (!leaving) {
            p.seatId = seatId;
            // +0x58: where its seats count from (0x0074c650).
            auto& entities = gameHandler_->getEntityManager();
            const uint64_t root = roster.rootVehicle(guid, 0, [&entities](uint64_t g) {
                return entities.getEntity(g) != nullptr;
            });
            p.rootGuid = root != 0 ? root : vehicleGuid;
        }
        p.vehicleGuid = vehicleGuid;
        p.seat = seatIndex;
        p.voluntary = how.voluntary;
        const game::vehicle::SeatRecord* seat = vehicleTables_.seatById(p.seatId);
        const bool animated = game::vehicle::changeIsAnimated(how.animated, leaving, seat);
        const SeatState first = game::vehicle::firstState(!leaving, seat, animated, p.voluntary);
        if (guid == gameHandler_->getPlayerGuid()) {
            // The local player's camera moves with it (0x0074cf30 ->
            // 0x0075aac0): after its wait, over its time - or, not yet
            // final, the flight's (0x0075af00).
            const auto timing = game::vehicle::cameraTiming(first, seat, animated);
            const bool rests = first == SeatState::None || first == SeatState::Seated;
            localSeatCamera_ = LocalSeatCamera{.pending = true,
                                               .startMs = now + timing.delayMs,
                                               .durationMs = timing.fixed || !rests ? timing.durationMs : 0u,
                                               .fixed = timing.fixed,
                                               .waitsForFlight = !timing.fixed && !rests};
            // Its vehicle camera (0x0075b1a0, made looking at the unit's
            // place) takes its mode and holds where it looked on the
            // unit's transport then - the vehicle on the way in or waiting
            // to leave, the world once it leaves (0x0075aac0).
            auto& vc = localVehicleCamera_;
            if (!vc.active) {
                vc.point = unitRenderPosition(guid);
                // 0x0075b1a0: a new camera faces as the unit does.
                glm::mat4 frame(1.0f);
                const uint32_t instance = p.instanceId ? p.instanceId : characterInstanceIdForGuid(guid);
                vc.facing.restartAt(cr->getInstanceFrame(instance, frame) ? yawOf(frame) : 0.0f);
            }
            vc.active = true;
            vc.clock = game::vehicle::cameraModeAtChange(first, timing, now);
            vc.fixed = timing.fixed;
            vc.durationMs = timing.durationMs;
            vc.seatId = p.seatId;
            // 0x0075a930: from a control seat it watches the vehicle, held
            // in the vehicle's own frame; else the unit, held on the
            // vehicle on the way in or waiting to leave, the world once it
            // leaves.
            vc.watchesVehicle = game::vehicle::cameraWatchesVehicle(leaving, seat);
            if (vc.watchesVehicle) vc.startFrame = vehicleTransportOf(vehicleGuid);
            else vc.startFrame = !leaving ? vehicleGuid : (first == SeatState::ExitDelay ? p.prevVehicleGuid : 0);
            vc.start = vc.point - framePosition(vc.startFrame);
            vc.spring = vc.start;
            vc.springVelocity = glm::vec3(0.0f);
            // Its facing starts over from where it faces (+0x7c..+0x94),
            // and a seat's FlagsB 0x1 holds the camera's yaw from here
            // (0x00759880, +0x8 0x2 and 0x4).
            vc.facing.restart();
            vc.facing.holdsYaw = game::vehicle::seatHoldsCameraYaw(vc.clock.mode, seat);
            if (vc.facing.holdsYaw) vc.facing.follows = true;
            vc.snap = true;
        }
        setSeatState(guid, p, first, now);
    };

    std::unordered_set<uint64_t> inSeats;
    for (const auto& [guid, passenger] : roster.passengers()) {
        // A seat of a unit that is a vehicle (0x00756ec0).
        const uint32_t vehicleId = roster.vehicleId(passenger.vehicleGuid);
        const game::vehicle::SeatRecord* seat = vehicleId ? vehicleTables_.seat(vehicleId, passenger.seat) : nullptr;
        if (!seat) continue;
        inSeats.insert(guid);
        auto [it, fresh] = seatedPassengers_.try_emplace(guid);
        SeatedPassenger& p = it->second;
        if (p.instanceId == 0) p.instanceId = characterInstanceIdForGuid(guid);
        if (!fresh && entersOrSits(p.state) && p.vehicleGuid == passenger.vehicleGuid && p.seat == passenger.seat)
            continue;
        change(guid, p, passenger.vehicleGuid, passenger.seat, seat->id);
    }
    for (auto& [guid, p] : seatedPassengers_) {
        if (!inSeats.count(guid) && entersOrSits(p.state)) change(guid, p, 0, 0xff, 0);
    }

    const uint64_t playerGuid = gameHandler_->getPlayerGuid();
    // Hung on a vehicle's seat, facing `facing` on it (0x007490f0); false
    // where the vehicle or the seat is not there to hang on.
    auto seatOn = [&](uint64_t guid, uint32_t instanceId, uint64_t vehicleGuid, uint8_t seatIndex, float facing) {
        const uint32_t vehicleId = roster.vehicleId(vehicleGuid);
        const game::vehicle::SeatRecord* seat = vehicleId ? vehicleTables_.seat(vehicleId, seatIndex) : nullptr;
        const uint32_t vehicleInstanceId = seat ? vehicleInstanceFor(vehicleGuid) : 0;
        if (vehicleInstanceId == 0 || vehicleInstanceId == instanceId) return false;
        rendering::vehicle_seat::PassengerPose pose;
        pose.facing = facing;
        pose.seatYaw = seat->passengerYaw;
        pose.seatPitch = seat->passengerPitch;
        pose.seatRoll = seat->passengerRoll;
        pose.seatOffset = seat->attachmentOffset;
        pose.passengerScale = guid == playerGuid ? 1.0f : unitModelScale(guid);
        if (seat->passengerAttachmentId >= 0) {
            pose.passengerAttachment =
                cr->modelAttachmentPosition(instanceId, static_cast<uint32_t>(seat->passengerAttachmentId));
        }
        cr->setInstanceSeat(instanceId, vehicleInstanceId,
                            rendering::vehicle_seat::seatModelAttachment(seat->attachmentId), pose);
        return true;
    };

    for (auto it = seatedPassengers_.begin(); it != seatedPassengers_.end();) {
        const uint64_t guid = it->first;
        SeatedPassenger& p = it->second;
        const uint32_t instanceId = characterInstanceIdForGuid(guid);
        if (instanceId != p.instanceId) {
            // Its model changed: the old one let go, the new one given the
            // state's animation.
            const bool wasHidden = p.hidden;
            releaseSeatedPassenger(guid, p, false);
            p.instanceId = instanceId;
            const game::vehicle::SeatRecord* seat = vehicleTables_.seatById(p.seatId);
            if (instanceId && seat && p.state != SeatState::None) {
                cr->setSeatAnimation(instanceId, toRide(game::vehicle::seatAnimation(p.state, *seat, p.voluntary)));
                if (wasHidden) cr->setInstanceVisible(instanceId, false);
                p.hidden = wasHidden;
            }
        }
        const auto passenger = roster.passenger(guid);
        const float seatedFacing = passenger ? passenger->facing : p.seatFacing;

        // 0x0074a200 once for the state, then 0x0074af70's steps: each wait
        // and flight on to the next when its time is up, the flight's facing
        // turned on the way (0x00747a30).
        auto step = [&]() {
            if (p.state == SeatState::Entering && p.instanceId) {
                seatOn(guid, p.instanceId, p.vehicleGuid, p.seat, seatedFacing);
            }
            if (!p.planned) planSeatState(guid, p, now);
        };
        step();
        if (p.state == SeatState::EnterDelay && reached(now, p.endMs)) {
            setSeatState(guid, p, SeatState::Entering, now);
            step();
        }
        const game::vehicle::SeatRecord* seat = vehicleTables_.seatById(p.seatId);
        const float progress =
            game::vehicle::progress(p.state, seat ? seat->flags : 0, p.startMs, p.endMs, now);  // 0x00747d70
        if (p.state == SeatState::Entering) {
            if (!reached(now, p.endMs)) {
                // The unit's facing is its seat's.
                float unitYaw = p.drawnFacing;
                if (const auto target = cr->seatTargetMatrix(p.instanceId)) unitYaw = yawOf(*target);
                p.drawnFacing = game::vehicle::flightFacing(p.startFacing, p.drawnFacing, unitYaw, progress);
            } else {
                setSeatState(guid, p, SeatState::Seated, now);
            }
        }
        if (p.state == SeatState::ExitDelay && reached(now, p.endMs)) {
            setSeatState(guid, p, SeatState::Exiting, now);
            step();
        }
        bool landed = false;
        if (p.state == SeatState::Exiting) {
            if (!reached(now, p.endMs)) {
                float unitYaw = p.drawnFacing;
                if (p.instanceId) {
                    if (const auto rotation = cr->getInstanceRotation(p.instanceId)) unitYaw = rotation->z;
                }
                p.drawnFacing = game::vehicle::flightFacing(p.startFacing, p.drawnFacing, unitYaw, progress);
            } else {
                setSeatState(guid, p, SeatState::None, now);
                landed = true;
            }
        }

        if (p.state == SeatState::None) {
            stopVehicleRideLoop(guid, p);
            releaseSeatedPassenger(guid, p, landed);
            it = seatedPassengers_.erase(it);
            continue;
        }
        // A unit with no model is stepped all the same, and drawn when it
        // has one.
        if (p.instanceId == 0) {
            ++it;
            continue;
        }

        // Drawn (0x0074a7f0).
        const float along = game::vehicle::progress(p.state, seat ? seat->flags : 0, p.startMs, p.endMs, now);
        glm::vec3 from = p.from;
        if (p.anchorGuid != 0) {
            glm::vec3 anchorPos(0.0f);
            if (cr->getInstancePosition(vehicleInstanceFor(p.anchorGuid), anchorPos)) from = anchorPos + p.fromLocal;
        }
        switch (p.state) {
            case SeatState::Seated:
                cr->setInstanceSeatFlight(p.instanceId, std::nullopt);
                if (!seatOn(guid, p.instanceId, p.vehicleGuid, p.seat, seatedFacing)) cr->clearInstanceSeat(p.instanceId);
                break;
            case SeatState::Entering:
            case SeatState::Exiting: {
                rendering::vehicle_seat::Flight flight;
                flight.from = from;
                if (p.state == SeatState::Entering) {
                    if (!seatOn(guid, p.instanceId, p.vehicleGuid, p.seat, seatedFacing)) flight.to = from;
                } else {
                    cr->clearInstanceSeat(p.instanceId);
                    flight.to = unitRenderPosition(guid);
                }
                flight.progress = along;
                flight.height = game::vehicle::arcHeight(p.arc, along);
                flight.facing = p.drawnFacing;
                cr->setInstanceSeatFlight(p.instanceId, flight);
                break;
            }
            default: {
                // Waiting: in the seat it leaves where it has one, at its
                // place on the seat's facing (+0xb8); else where it began.
                if (p.prevVehicleGuid != 0 &&
                    seatOn(guid, p.instanceId, p.prevVehicleGuid, p.prevSeat, p.seatFacing)) {
                    cr->setInstanceSeatFlight(p.instanceId, std::nullopt);
                    break;
                }
                cr->clearInstanceSeat(p.instanceId);
                rendering::vehicle_seat::Flight flight;
                flight.from = from;
                flight.to = from;
                flight.facing = p.startFacing;
                cr->setInstanceSeatFlight(p.instanceId, flight);
                break;
            }
        }

        // FlagsB 0x10000 (0x00748620): seated, the passenger mirrors the
        // vehicle while it lives (0x007487e0 asks its health).
        if (p.state == SeatState::Seated && seat) {
            bool alive = true;
            if (auto entity = gameHandler_->getEntityManager().getEntity(guid)) {
                if (auto unit = std::dynamic_pointer_cast<game::Unit>(entity)) alive = unit->getHealth() > 0;
            }
            cr->setMirrorsSeatParent(p.instanceId,
                                     alive && (seat->flagsB & game::vehicle::kSeatFlagBMirrorsVehicle) != 0);
        }
        ++it;
    }
}

uint32_t EntitySpawner::vehicleFlags(uint64_t guid) const {
    if (!gameHandler_) return 0;
    const auto* vehicle = vehicleTables_.vehicle(gameHandler_->vehicleRoster().vehicleId(guid));
    return vehicle ? vehicle->flags : 0u;
}

}  // namespace wowee::core
