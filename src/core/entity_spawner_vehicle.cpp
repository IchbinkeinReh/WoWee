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

namespace wowee::core {

void EntitySpawner::loadVehicleTables() {
    if (vehicleTablesLoaded_ || !assetManager_ || !assetManager_->isInitialized()) return;
    vehicleTablesLoaded_ = true;
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
        vehicleTables_.addSeat(rec);
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
    return rendering::vehicle_seat::RideAnimation{.start = a->start, .loop = a->loop};
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
    p.state = state;
    p.startMs = nowMs;
    p.endMs = nowMs;
    p.planned = false;
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
                // The seat's speed less the vehicle's own along the way
                // (0x006fed70).
                glm::vec3 velocity(0.0f);
                if (auto it = vehicleMotion_.find(p.vehicleGuid); it != vehicleMotion_.end()) {
                    velocity = it->second.velocity;
                }
                along = distance > 0.0001f ? seat->enterSpeed - glm::dot(velocity, way / distance) : 0.0f;
            }
            p.arc = game::vehicle::planArc(*seat, entering, distance, along);
            p.endMs = p.startMs + p.arc.durationMs;
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

void EntitySpawner::updateVehiclePassengers() {
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!cr || !gameHandler_) return;
    auto& roster = gameHandler_->vehicleRoster();
    if (roster.passengers().empty() && seatedPassengers_.empty()) return;
    loadVehicleTables();
    const uint32_t now = seatClockMs();

    // The vehicles' speeds, for the way into their seats (0x006fed70).
    for (const auto& [guid, passenger] : roster.passengers()) {
        glm::vec3 place(0.0f);
        if (!cr->getInstancePosition(vehicleInstanceFor(passenger.vehicleGuid), place)) continue;
        VehicleMotion& m = vehicleMotion_[passenger.vehicleGuid];
        if (m.known && now != m.ms) {
            const float dt = static_cast<float>(now - m.ms) * 0.001f;
            if (dt > 0.0f && dt < 1.0f) m.velocity = (place - m.place) / dt;
        }
        m.place = place;
        m.ms = now;
        m.known = true;
    }

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
        if (!leaving) p.seatId = seatId;
        p.vehicleGuid = vehicleGuid;
        p.seat = seatIndex;
        p.voluntary = how.voluntary;
        const game::vehicle::SeatRecord* seat = vehicleTables_.seatById(p.seatId);
        const bool animated = game::vehicle::changeIsAnimated(how.animated, leaving, seat);
        setSeatState(guid, p, game::vehicle::firstState(!leaving, seat, animated, p.voluntary), now);
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
            releaseSeatedPassenger(guid, p, false);
            p.instanceId = instanceId;
            const game::vehicle::SeatRecord* seat = vehicleTables_.seatById(p.seatId);
            if (instanceId && seat && p.state != SeatState::None) {
                cr->setSeatAnimation(instanceId, toRide(game::vehicle::seatAnimation(p.state, *seat, p.voluntary)));
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

        if (p.state == SeatState::None || p.instanceId == 0) {
            releaseSeatedPassenger(guid, p, landed);
            it = seatedPassengers_.erase(it);
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

}  // namespace wowee::core
