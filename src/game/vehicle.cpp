#include "game/vehicle.hpp"

namespace wowee::game::vehicle {

const VehicleRecord* VehicleTables::vehicle(uint32_t id) const {
    const auto it = vehicles_.find(id);
    return it != vehicles_.end() ? &it->second : nullptr;
}

const SeatRecord* VehicleTables::seatById(uint32_t id) const {
    const auto it = seats_.find(id);
    return it != seats_.end() ? &it->second : nullptr;
}

const SeatRecord* VehicleTables::seat(uint32_t vehicleId, uint8_t seatIndex) const {
    // 0x00756ec0: seat < 8 and a vehicle row, then VehicleSeat by its column.
    if (seatIndex >= kSeatCount) return nullptr;
    const VehicleRecord* row = vehicle(vehicleId);
    if (!row) return nullptr;
    return seatById(row->seatIds[seatIndex]);
}

void VehicleRoster::setVehicleId(uint64_t guid, uint32_t vehicleId) {
    if (guid == 0) return;
    if (vehicleId == 0) {
        vehicleIds_.erase(guid);
    } else {
        vehicleIds_[guid] = vehicleId;
    }
}

uint32_t VehicleRoster::vehicleId(uint64_t guid) const {
    const auto it = vehicleIds_.find(guid);
    return it != vehicleIds_.end() ? it->second : 0;
}

void VehicleRoster::setTransport(uint64_t guid, uint64_t transportGuid, int8_t seat, float facing, bool animated) {
    if (guid == 0) return;
    const auto it = passengers_.find(guid);
    const bool seated = transportGuid != 0 && transportGuid != guid && seat >= 0 &&
                        static_cast<uint32_t>(seat) < kSeatCount;
    const bool changed = seated ? (it == passengers_.end() || it->second.vehicleGuid != transportGuid ||
                                   it->second.seat != static_cast<uint8_t>(seat))
                                : it != passengers_.end();
    if (changed) {
        const auto f2 = moveFlags2_.find(guid);
        changes_[guid] = SeatChange{.animated = animated,
                                    .voluntary = f2 != moveFlags2_.end() && (f2->second & 0x40) != 0};
    }
    if (!seated) {
        passengers_.erase(guid);
        return;
    }
    passengers_[guid] = Passenger{.vehicleGuid = transportGuid,
                                  .seat = static_cast<uint8_t>(seat),
                                  .facing = facing};
}

void VehicleRoster::setMoveFlags2(uint64_t guid, uint32_t flags2) {
    if (guid == 0) return;
    if (flags2 == 0) moveFlags2_.erase(guid);
    else moveFlags2_[guid] = flags2;
}

std::optional<VehicleRoster::SeatChange> VehicleRoster::takeChange(uint64_t guid) {
    const auto it = changes_.find(guid);
    if (it == changes_.end()) return std::nullopt;
    const SeatChange out = it->second;
    changes_.erase(it);
    return out;
}

std::optional<Passenger> VehicleRoster::passenger(uint64_t guid) const {
    const auto it = passengers_.find(guid);
    if (it == passengers_.end()) return std::nullopt;
    return it->second;
}

void VehicleRoster::setSeatState(uint64_t guid, uint8_t state) {
    if (state == 0) seatStates_.erase(guid);
    else seatStates_[guid] = state;
}

uint8_t VehicleRoster::seatState(uint64_t guid) const {
    const auto it = seatStates_.find(guid);
    return it != seatStates_.end() ? it->second : 0;
}

const SeatRecord* VehicleRoster::seatOf(uint64_t guid) const {
    const auto p = passengers_.find(guid);
    if (!tables_ || p == passengers_.end()) return nullptr;
    return tables_->seat(vehicleId(p->second.vehicleGuid), p->second.seat);
}

const VehicleRecord* VehicleRoster::vehicleOf(uint64_t guid) const {
    const auto p = passengers_.find(guid);
    if (!tables_ || p == passengers_.end()) return nullptr;
    return tables_->vehicle(vehicleId(p->second.vehicleGuid));
}

void VehicleRoster::forget(uint64_t guid) {
    vehicleIds_.erase(guid);
    passengers_.erase(guid);
    moveFlags2_.erase(guid);
    changes_.erase(guid);
    seatStates_.erase(guid);
}

void VehicleRoster::clear() {
    vehicleIds_.clear();
    passengers_.clear();
    moveFlags2_.clear();
    changes_.clear();
    seatStates_.clear();
}

}  // namespace wowee::game::vehicle
