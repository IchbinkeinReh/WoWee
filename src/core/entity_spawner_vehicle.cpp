// Vehicle passengers drawn in their seats (0x0074a7f0 state 3).
//
// The client's passenger component hangs the passenger's model on the
// vehicle model's attachment for the seat (0x00831630) by the matrix
// 0x007490f0 builds, plays the seat's ride animation in place of the unit's
// own (0x00747b20, from 0x007385c0), hides the passenger where the seat's
// Flags 0x200 put +0x10 0x400 (0x007489c0, read by 0x00730f30), and where
// the seat's FlagsB 0x10000 asks, copies the vehicle's animations onto it and
// hands it each the vehicle plays after (0x00748620, 0x00736d30).
#include "core/entity_spawner.hpp"

#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/vehicle_seat.hpp"

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
    for (uint32_t row = 0; row < vehicleDbc->getRecordCount(); ++row) {
        game::vehicle::VehicleRecord rec;
        rec.id = u32(*vehicleDbc, row, vId, 0);
        rec.flags = u32(*vehicleDbc, row, vFlags, 0);
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
        vehicleTables_.addSeat(rec);
    }
}

void EntitySpawner::releaseSeatedPassenger(const SeatedPassenger& seated) {
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!cr) return;
    cr->clearInstanceSeat(seated.instanceId);
    if (seated.hidden) cr->setInstanceVisible(seated.instanceId, true);
    // Out of the seat the unit animates as itself again (0x007489c0 asks
    // 0x0073ac30 for its animation).
    cr->playAnimation(seated.instanceId, rendering::anim::STAND, true);
}

void EntitySpawner::updateVehiclePassengers() {
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!cr || !gameHandler_) return;
    const auto& roster = gameHandler_->vehicleRoster();
    if (roster.passengers().empty() && seatedPassengers_.empty()) return;
    loadVehicleTables();

    const uint64_t playerGuid = gameHandler_->getPlayerGuid();
    // The model a vehicle is drawn by: its mount's while it rides one.
    auto vehicleInstanceFor = [&](uint64_t guid) -> uint32_t {
        if (guid == playerGuid && mountInstanceId_ != 0) return mountInstanceId_;
        if (const uint32_t mount = remotePlayerMountInstance(guid)) return mount;
        return characterInstanceIdForGuid(guid);
    };

    std::unordered_set<uint64_t> seatedNow;
    for (const auto& [guid, passenger] : roster.passengers()) {
        // A seat of a unit that is a vehicle (0x00756ec0).
        const uint32_t vehicleId = roster.vehicleId(passenger.vehicleGuid);
        const game::vehicle::SeatRecord* seat =
            vehicleId ? vehicleTables_.seat(vehicleId, passenger.seat) : nullptr;
        if (!seat) continue;
        const uint32_t instanceId = characterInstanceIdForGuid(guid);
        const uint32_t vehicleInstanceId = vehicleInstanceFor(passenger.vehicleGuid);
        if (instanceId == 0 || vehicleInstanceId == 0 || instanceId == vehicleInstanceId) continue;
        seatedNow.insert(guid);

        // 0x007490f0's matrix, on the attachment DAT_00a2d3f0 gives the
        // seat; the passenger's own attachment where it has it (0x00748400).
        rendering::vehicle_seat::PassengerPose pose;
        pose.facing = passenger.facing;
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

        auto it = seatedPassengers_.find(guid);
        const bool fresh = it == seatedPassengers_.end() || it->second.instanceId != instanceId ||
                           it->second.vehicleInstanceId != vehicleInstanceId || it->second.seatId != seat->id;
        if (fresh) {
            if (it != seatedPassengers_.end() && it->second.instanceId != instanceId) {
                releaseSeatedPassenger(it->second);
            }
            SeatedPassenger seated{.instanceId = instanceId, .vehicleInstanceId = vehicleInstanceId,
                                   .seatId = seat->id, .hidden = false};
            // Entering the seat: its ride animation (0x00747b20 state 3).
            cr->setMirrorsSeatParent(instanceId, false);
            cr->setSeatAnimation(instanceId, rendering::vehicle_seat::rideAnimation(
                                                 seat->flags, seat->rideAnimStart, seat->rideAnimLoop));
            // Flags 0x200 hides the passenger (0x007489c0 -> 0x00730f30).
            const bool hide = (seat->flags & game::vehicle::kSeatFlagHidePassenger) != 0;
            const bool wasHidden = it != seatedPassengers_.end() && it->second.hidden;
            if (hide) cr->setInstanceVisible(instanceId, false);
            else if (wasHidden) cr->setInstanceVisible(instanceId, true);
            seated.hidden = hide;
            seatedPassengers_[guid] = seated;
        }

        // FlagsB 0x10000 (0x00748620): the passenger mirrors the vehicle
        // while it lives (0x007487e0 asks its health).
        bool alive = true;
        if (auto entity = gameHandler_->getEntityManager().getEntity(guid)) {
            if (auto unit = std::dynamic_pointer_cast<game::Unit>(entity)) alive = unit->getHealth() > 0;
        }
        cr->setMirrorsSeatParent(instanceId,
                                 alive && (seat->flagsB & game::vehicle::kSeatFlagBMirrorsVehicle) != 0);
    }

    for (auto it = seatedPassengers_.begin(); it != seatedPassengers_.end();) {
        if (seatedNow.count(it->first)) {
            ++it;
            continue;
        }
        releaseSeatedPassenger(it->second);
        it = seatedPassengers_.erase(it);
    }
}

}  // namespace wowee::core
