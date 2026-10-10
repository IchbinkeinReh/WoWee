#pragma once

// Vehicles and their passengers as the client keeps them.
//
// A unit is a vehicle while it has a Vehicle.dbc row: the create block's
// UPDATEFLAG_VEHICLE names it, and SMSG_SET_VEHICLE_REC_ID (0x4a7, wowee's
// SMSG_PLAYER_VEHICLE_DATA) sets or resets it for any unit (0x00716db0 ->
// 0x0074c750 / 0x0074c7b0, the vehicle component at CGUnit_C +0xf5c).
//
// A unit rides one while its movement says it is on a transport that is a
// vehicle: the transport guid and the seat byte (0x007489c0 state 3, the
// vehicle's guid at +0x20/+0x28 and the seat at +0x30/+0x31 of the
// passenger component, CGUnit_C +0xf60). The seat's VehicleSeat.dbc row is
// the vehicle row's seat column for it (0x00756ec0).

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>

#include <glm/glm.hpp>

namespace wowee::game::vehicle {

/// Vehicle.dbc has eight seat columns, and a seat byte of 8 or more names
/// none (0x00756ec0, 0x007489c0).
inline constexpr uint32_t kSeatCount = 8;

/// VehicleSeat.dbc Flags (+4) the client draws by.
inline constexpr uint32_t kSeatFlagHasRideAnim = 0x2;         ///< 0x00747b20 case 3
inline constexpr uint32_t kSeatFlagHasUpperRideAnim = 0x4;    ///< 0x00747bd0
inline constexpr uint32_t kSeatFlagHidePassenger = 0x200;     ///< 0x007489c0 -> +0x10 0x400 -> 0x00730f30
/// FlagsB (+0xb4): the passenger mirrors the vehicle's animations
/// (0x007489c0 -> 0x00748620).
inline constexpr uint32_t kSeatFlagBMirrorsVehicle = 0x10000;

/// A VehicleSeat.dbc row (WotLK: 58 columns, 232 bytes, 0x00654a60), the
/// columns the client draws a passenger by.
struct SeatRecord {
    uint32_t id = 0;
    uint32_t flags = 0;                   ///< +0x04
    int32_t attachmentId = -1;            ///< +0x08, an index into 0x00a2d3f0
    glm::vec3 attachmentOffset{0.0f};     ///< +0x0c
    int32_t rideAnimStart = -1;           ///< +0x3c
    int32_t rideAnimLoop = -1;            ///< +0x40
    int32_t rideUpperAnimStart = -1;      ///< +0x44
    int32_t rideUpperAnimLoop = -1;       ///< +0x48
    float passengerYaw = 0.0f;            ///< +0x74
    float passengerPitch = 0.0f;          ///< +0x78
    float passengerRoll = 0.0f;           ///< +0x7c
    int32_t passengerAttachmentId = -1;   ///< +0x80, the passenger model's own
    uint32_t flagsB = 0;                  ///< +0xb4
};

/// A Vehicle.dbc row (WotLK: 40 columns, 160 bytes, 0x00654800).
struct VehicleRecord {
    uint32_t id = 0;
    uint32_t flags = 0;
    std::array<uint32_t, kSeatCount> seatIds{};  ///< +0x18
};

/// Vehicle.dbc and VehicleSeat.dbc by id.
class VehicleTables {
public:
    void addVehicle(const VehicleRecord& row) { vehicles_[row.id] = row; }
    void addSeat(const SeatRecord& row) { seats_[row.id] = row; }
    [[nodiscard]] const VehicleRecord* vehicle(uint32_t id) const;
    [[nodiscard]] const SeatRecord* seatById(uint32_t id) const;
    /// The row for a vehicle's seat (0x00756ec0): its seat column, nothing
    /// for a seat of 8 or more, an unknown vehicle or an empty column.
    [[nodiscard]] const SeatRecord* seat(uint32_t vehicleId, uint8_t seatIndex) const;
    [[nodiscard]] bool empty() const { return vehicles_.empty(); }

private:
    std::unordered_map<uint32_t, VehicleRecord> vehicles_;
    std::unordered_map<uint32_t, SeatRecord> seats_;
};

/// A unit in a seat: its vehicle, the seat and its facing on it - the
/// movement's transport orientation, which the client turns the passenger by
/// (+0xaa0, 0x0074a7f0).
struct Passenger {
    uint64_t vehicleGuid = 0;
    uint8_t seat = 0;
    float facing = 0.0f;
};

/// Which units are vehicles and who rides them.
class VehicleRoster {
public:
    /// The unit's Vehicle.dbc row; 0 makes it no vehicle (0x0074c7b0).
    void setVehicleId(uint64_t guid, uint32_t vehicleId);
    [[nodiscard]] uint32_t vehicleId(uint64_t guid) const;

    /// What a unit's movement says of its transport. A transport and a seat
    /// byte under 8 seat it; anything else takes it out of any seat. Whether
    /// the transport is a vehicle is asked when it is drawn, since the
    /// passenger can come into view before its vehicle does.
    void setTransport(uint64_t guid, uint64_t transportGuid, int8_t seat, float facing);
    void leave(uint64_t guid) { passengers_.erase(guid); }
    [[nodiscard]] std::optional<Passenger> passenger(uint64_t guid) const;
    [[nodiscard]] const std::unordered_map<uint64_t, Passenger>& passengers() const { return passengers_; }

    /// The unit went out of view: neither a vehicle nor in a seat.
    void forget(uint64_t guid);
    void clear();

private:
    std::unordered_map<uint64_t, uint32_t> vehicleIds_;
    std::unordered_map<uint64_t, Passenger> passengers_;
};

}  // namespace wowee::game::vehicle
