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

void VehicleRoster::setTransport(uint64_t guid, uint64_t transportGuid, int8_t seat, float facing) {
    if (guid == 0) return;
    if (transportGuid == 0 || transportGuid == guid || seat < 0 ||
        static_cast<uint32_t>(seat) >= kSeatCount) {
        passengers_.erase(guid);
        return;
    }
    passengers_[guid] = Passenger{.vehicleGuid = transportGuid,
                                  .seat = static_cast<uint8_t>(seat),
                                  .facing = facing};
}

std::optional<Passenger> VehicleRoster::passenger(uint64_t guid) const {
    const auto it = passengers_.find(guid);
    if (it == passengers_.end()) return std::nullopt;
    return it->second;
}

void VehicleRoster::forget(uint64_t guid) {
    vehicleIds_.erase(guid);
    passengers_.erase(guid);
}

void VehicleRoster::clear() {
    vehicleIds_.clear();
    passengers_.clear();
}

}  // namespace wowee::game::vehicle
