#include "game/vehicle.hpp"

#include <algorithm>

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

void VehicleTables::addIndicator(uint32_t id, std::string backgroundTexture) {
    UIIndicator& row = indicators_[id];
    row.id = id;
    row.backgroundTexture = std::move(backgroundTexture);
    row.present = true;
}

void VehicleTables::addIndicatorSeat(const UIIndicatorSeat& seat) {
    // 0x00614d90 walks VehicleUIIndSeat by id and keeps its indicator's.
    auto& seats = indicators_[seat.indicatorId].seats;
    const auto at = std::lower_bound(seats.begin(), seats.end(), seat.id,
                                     [](const UIIndicatorSeat& s, uint32_t id) { return s.id < id; });
    seats.insert(at, seat);
}

const UIIndicator* VehicleTables::indicator(uint32_t id) const {
    const auto it = indicators_.find(id);
    return it != indicators_.end() && it->second.present ? &it->second : nullptr;
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

void VehicleRoster::setPitch(uint64_t guid, float pitch) {
    if (guid == 0) return;
    if (pitch == 0.0f) pitches_.erase(guid);
    else pitches_[guid] = pitch;
}

float VehicleRoster::pitch(uint64_t guid) const {
    const auto it = pitches_.find(guid);
    return it != pitches_.end() ? it->second : 0.0f;
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

uint64_t VehicleRoster::rootVehicle(uint64_t guid, uint64_t stopAt,
                                   const std::function<bool(uint64_t)>& known) const {
    // 0x0074c650.
    if (guid == 0) return 0;
    if (guid == stopAt && vehicleId(guid) != 0) return guid;
    auto transportOf = [this](uint64_t g) -> uint64_t {
        const auto p = passengers_.find(g);
        return p != passengers_.end() ? p->second.vehicleGuid : 0;
    };
    const uint64_t first = transportOf(guid);
    if (isVehicleGuid(first) || isPlayerGuid(first)) {
        for (uint64_t cur = first; cur != 0 && known(cur);) {
            if (cur == stopAt) return cur;
            const uint64_t next = transportOf(cur);
            if (!isVehicleGuid(next) && !isPlayerGuid(next)) return cur;
            cur = next;
        }
        return 0;
    }
    return vehicleId(guid) != 0 ? guid : 0;
}

uint64_t VehicleRoster::occupant(uint64_t vehicleGuid, uint8_t seat) const {
    // 0x00757680: the first on the vehicle's list with that seat byte.
    for (const auto& [guid, p] : passengers_) {
        if (p.vehicleGuid == vehicleGuid && p.seat == seat) return guid;
    }
    return 0;
}

uint32_t VehicleRoster::uiSeatCount(uint64_t vehicleGuid, const std::function<bool(uint64_t)>& isPlayer) const {
    // 0x00757470.
    const VehicleRecord* row = tables_ ? tables_->vehicle(vehicleId(vehicleGuid)) : nullptr;
    if (!row) return 0;
    uint32_t count = 0;
    for (const uint32_t id : row->seatIds) {
        if (static_cast<int32_t>(id) > 0) ++count;
    }
    for (const auto& [guid, p] : passengers_) {
        if (p.vehicleGuid != vehicleGuid || guid == vehicleGuid) continue;
        if (tables_->vehicle(vehicleId(guid)) && !isPlayer(guid)) count = count - 1 + uiSeatCount(guid, isPlayer);
    }
    return count;
}

namespace {
bool findUISeat(const VehicleRoster& roster, const VehicleTables& tables, uint64_t vehicleGuid, uint32_t& index,
                const std::function<bool(uint64_t)>& isPlayer, VehicleRoster::UISeat& out) {
    // 0x00757550: seat by seat, a seat a vehicle (no player) rides in giving
    // way to that vehicle's seats.
    const VehicleRecord* row = tables.vehicle(roster.vehicleId(vehicleGuid));
    if (!row) return false;
    std::array<uint64_t, kSeatCount> riders{};
    for (const auto& [guid, p] : roster.passengers()) {
        if (p.vehicleGuid != vehicleGuid || guid == vehicleGuid || p.seat >= kSeatCount) continue;
        if (tables.vehicle(roster.vehicleId(guid)) && !isPlayer(guid)) riders[p.seat] = guid;
    }
    for (uint8_t i = 0; i < kSeatCount; ++i) {
        if (riders[i] == 0) {
            if (static_cast<int32_t>(row->seatIds[i]) > 0) {
                if (index == 0) {
                    out = VehicleRoster::UISeat{.vehicleGuid = vehicleGuid, .seat = i};
                    return true;
                }
                --index;
            }
        } else if (findUISeat(roster, tables, riders[i], index, isPlayer, out)) {
            return true;
        }
    }
    return false;
}
}  // namespace

std::optional<VehicleRoster::UISeat> VehicleRoster::uiSeat(uint64_t vehicleGuid, uint32_t index,
                                                           const std::function<bool(uint64_t)>& isPlayer) const {
    if (!tables_) return std::nullopt;
    UISeat out;
    if (!findUISeat(*this, *tables_, vehicleGuid, index, isPlayer, out)) return std::nullopt;
    return out;
}

std::optional<UISeatInfo> uiSeatInfo(const VehicleRoster& roster, uint64_t rootGuid, uint32_t index,
                                     const std::function<bool(uint64_t)>& isPlayer) {
    // 0x007579e0.
    const auto at = roster.uiSeat(rootGuid, index, isPlayer);
    if (!at || !roster.tables()) return std::nullopt;
    const SeatRecord* seat = roster.tables()->seat(roster.vehicleId(at->vehicleGuid), at->seat);
    if (!seat) return std::nullopt;
    UISeatInfo info;
    if (seat->flags & 0x800) info.controlType = at->vehicleGuid == rootGuid ? "Root" : "Child";
    info.occupant = roster.occupant(at->vehicleGuid, at->seat);
    info.ejectable = (seat->flagsB & kSeatFlagBEjectable) != 0;
    info.canSwitchSeats = (seat->flags & kSeatFlagCanSwitch) != 0;
    return info;
}

void VehicleRoster::forget(uint64_t guid) {
    vehicleIds_.erase(guid);
    passengers_.erase(guid);
    moveFlags2_.erase(guid);
    pitches_.erase(guid);
    changes_.erase(guid);
    seatStates_.erase(guid);
}

void VehicleRoster::clear() {
    vehicleIds_.clear();
    passengers_.clear();
    moveFlags2_.clear();
    pitches_.clear();
    changes_.clear();
    seatStates_.clear();
}

}  // namespace wowee::game::vehicle
