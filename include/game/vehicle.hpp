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
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

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
    float enterPreDelay = 0.0f;           ///< +0x18
    float enterSpeed = 0.0f;              ///< +0x1c
    float enterGravity = 0.0f;            ///< +0x20
    float enterMinDuration = 0.0f;        ///< +0x24
    float enterMaxDuration = 0.0f;        ///< +0x28
    float enterMinArcHeight = 0.0f;       ///< +0x2c
    float enterMaxArcHeight = 0.0f;       ///< +0x30
    int32_t enterAnimStart = -1;          ///< +0x34
    int32_t enterAnimLoop = -1;           ///< +0x38
    int32_t rideAnimStart = -1;           ///< +0x3c
    int32_t rideAnimLoop = -1;            ///< +0x40
    int32_t rideUpperAnimStart = -1;      ///< +0x44
    int32_t rideUpperAnimLoop = -1;       ///< +0x48
    float exitPreDelay = 0.0f;            ///< +0x4c
    float exitSpeed = 0.0f;               ///< +0x50
    float exitGravity = 0.0f;             ///< +0x54
    float exitMinDuration = 0.0f;         ///< +0x58
    float exitMaxDuration = 0.0f;         ///< +0x5c
    float exitMinArcHeight = 0.0f;        ///< +0x60
    float exitMaxArcHeight = 0.0f;        ///< +0x64
    int32_t exitAnimStart = -1;           ///< +0x68
    int32_t exitAnimLoop = -1;            ///< +0x6c
    int32_t exitAnimEnd = -1;             ///< +0x70
    float passengerYaw = 0.0f;            ///< +0x74
    float passengerPitch = 0.0f;          ///< +0x78
    float passengerRoll = 0.0f;           ///< +0x7c
    int32_t passengerAttachmentId = -1;   ///< +0x80, the passenger model's own
    int32_t vehicleEnterAnim = -1;        ///< +0x84
    int32_t vehicleExitAnim = -1;         ///< +0x88
    int32_t vehicleRideAnimLoop = -1;     ///< +0x8c
    int32_t vehicleEnterAnimBone = 0;     ///< +0x90, a key bone
    int32_t vehicleExitAnimBone = 0;      ///< +0x94
    int32_t vehicleRideAnimLoopBone = 0;  ///< +0x98
    float vehicleEnterAnimDelay = 0.0f;   ///< +0x9c
    float vehicleExitAnimDelay = 0.0f;    ///< +0xa0
    uint32_t vehicleAbilityDisplay = 0;   ///< +0xa4, 1 hands the action bar to the vehicle (0x005d4ad0)
    uint32_t enterUISoundId = 0;          ///< +0xa8
    uint32_t exitUISoundId = 0;           ///< +0xac
    uint32_t uiSkin = 0;                  ///< +0xb0
    uint32_t flagsB = 0;                  ///< +0xb4
    float cameraEnteringDelay = 0.0f;     ///< +0xb8
    float cameraEnteringDuration = 0.0f;  ///< +0xbc
    float cameraExitingDelay = 0.0f;      ///< +0xc0
    float cameraExitingDuration = 0.0f;   ///< +0xc4
    glm::vec3 cameraOffset{0.0f};         ///< +0xc8
    float cameraPosChaseRate = 0.0f;      ///< +0xd4
    float cameraFacingChaseRate = 0.0f;   ///< +0xd8
    float cameraEnteringZoom = 0.0f;      ///< +0xdc
    float cameraSeatZoomMin = 0.0f;       ///< +0xe0
    float cameraSeatZoomMax = 0.0f;       ///< +0xe4
};

/// A Vehicle.dbc row (WotLK: 40 columns, 160 bytes, 0x00654800).
struct VehicleRecord {
    uint32_t id = 0;
    uint32_t flags = 0;
    float pitchMin = 0.0f;                       ///< +0x10
    float pitchMax = 0.0f;                       ///< +0x14
    std::array<uint32_t, kSeatCount> seatIds{};  ///< +0x18
    float cameraFadeDistScalarMin = 0.0f;        ///< +0x3c
    float cameraFadeDistScalarMax = 0.0f;        ///< +0x40
    float cameraPitchOffset = 0.0f;              ///< +0x44
    float cameraYawOffset = 0.0f;                ///< +0x84
    uint32_t uiLocomotionType = 0;               ///< +0x88, the minimap's icon (0x00581e80)
    uint32_t uiIndicatorId = 0;                  ///< +0x90
};

/// VehicleSeat Flags and FlagsB the vehicle interface reads.
inline constexpr uint32_t kSeatFlagCanSwitch = 0x4000000;     ///< 0x0074ca90, 0x007579e0
inline constexpr uint32_t kSeatFlagHasVehicleUI = 0x20000000; ///< 0x00613740
inline constexpr uint32_t kSeatFlagBEjectable = 0x20;         ///< 0x00613d20, 0x007579e0
/// Vehicle.dbc Flags the aim reads (0x005fb3a0, 0x005f9f70, 0x005f9fe0).
inline constexpr uint32_t kVehicleFlagPitchLimits = 0x40;
inline constexpr uint32_t kVehicleFlagAimAngle = 0x400;
inline constexpr uint32_t kVehicleFlagAimPower = 0x800;

/// VehicleUIIndicator.dbc (ID, BackgroundTexture) and its VehicleUIIndSeat
/// rows (ID, VehicleUIIndicatorID, VirtualSeatIndex, XPos, YPos), which
/// 0x00614d90 gathers for an indicator in their id order.
struct UIIndicatorSeat {
    uint32_t id = 0;
    uint32_t indicatorId = 0;
    uint32_t virtualSeatIndex = 0;  ///< +0x8
    float x = 0.0f;                 ///< +0xc
    float y = 0.0f;                 ///< +0x10
};
struct UIIndicator {
    uint32_t id = 0;
    std::string backgroundTexture;
    std::vector<UIIndicatorSeat> seats;
    bool present = false;  ///< it has a VehicleUIIndicator row
};

/// Vehicle.dbc and VehicleSeat.dbc by id.
class VehicleTables {
public:
    void addVehicle(const VehicleRecord& row) { vehicles_[row.id] = row; }
    void addSeat(const SeatRecord& row) { seats_[row.id] = row; }
    void addIndicator(uint32_t id, std::string backgroundTexture);
    /// An indicator seat, kept with its indicator in id order.
    void addIndicatorSeat(const UIIndicatorSeat& seat);
    [[nodiscard]] const UIIndicator* indicator(uint32_t id) const;
    [[nodiscard]] const VehicleRecord* vehicle(uint32_t id) const;
    [[nodiscard]] const SeatRecord* seatById(uint32_t id) const;
    /// The row for a vehicle's seat (0x00756ec0): its seat column, nothing
    /// for a seat of 8 or more, an unknown vehicle or an empty column.
    [[nodiscard]] const SeatRecord* seat(uint32_t vehicleId, uint8_t seatIndex) const;
    [[nodiscard]] bool empty() const { return vehicles_.empty(); }

private:
    std::unordered_map<uint32_t, VehicleRecord> vehicles_;
    std::unordered_map<uint32_t, SeatRecord> seats_;
    std::unordered_map<uint32_t, UIIndicator> indicators_;
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
    /// `animated` says the change is moved through (+0xa30 0x20000000,
    /// 0x0074b840): from a monster move, an 0xED movement packet.
    void setTransport(uint64_t guid, uint64_t transportGuid, int8_t seat, float facing, bool animated = false);
    void leave(uint64_t guid) { setTransport(guid, 0, -1, 0.0f); }
    /// The unit's movement flags2 (+0x7d0), whose 0x40 makes a leaving one
    /// of its own will (0x0074bb60).
    void setMoveFlags2(uint64_t guid, uint32_t flags2);
    [[nodiscard]] uint32_t moveFlags2(uint64_t guid) const;
    /// The unit's pitch as its last movement said (+0x7c0 of its movement).
    void setPitch(uint64_t guid, float pitch);
    [[nodiscard]] float pitch(uint64_t guid) const;
    /// How the unit's last change of seat came about, until it is taken.
    struct SeatChange {
        bool animated = false;
        bool voluntary = false;
    };
    [[nodiscard]] std::optional<SeatChange> takeChange(uint64_t guid);
    [[nodiscard]] std::optional<Passenger> passenger(uint64_t guid) const;
    [[nodiscard]] const std::unordered_map<uint64_t, Passenger>& passengers() const { return passengers_; }

    /// Where each unit is on its way into, in or out of its seat - its
    /// passenger component's state (+0xf60 +0x14), 0 for none - as the
    /// drawing side steps it; and the tables the seats are read from.
    void setSeatState(uint64_t guid, uint8_t state);
    [[nodiscard]] uint8_t seatState(uint64_t guid) const;
    /// A seated unit asked its vehicle for a seat change (0x00747fa0, its
    /// passenger component's +0x10 0x8 and +0x4c): asked until its seat
    /// state next changes (0x007489c0) or 2 s pass (0x0074af70).
    void markSeatChangeAsked(uint64_t guid, uint32_t nowMs);
    [[nodiscard]] bool seatChangeAsked(uint64_t guid, uint32_t nowMs) const;
    void setTables(const VehicleTables* tables) { tables_ = tables; }
    [[nodiscard]] const VehicleTables* tables() const { return tables_; }
    /// The VehicleSeat row of the unit's seat, whatever its state
    /// (0x00613600): its transport's vehicle row's column for its seat.
    [[nodiscard]] const SeatRecord* seatOf(uint64_t guid) const;
    [[nodiscard]] const VehicleRecord* vehicleOf(uint64_t guid) const;

    /// The vehicle a unit's seats are counted from (0x0074c650): up its
    /// chain of transports while each is a vehicle's or a player's guid, to
    /// `stopAt` where it is met; else the unit itself where it is a vehicle.
    /// `known` says whether an object is in view (0x004d4db0).
    [[nodiscard]] uint64_t rootVehicle(uint64_t guid, uint64_t stopAt,
                                       const std::function<bool(uint64_t)>& known) const;
    /// The passenger in a vehicle's seat (0x00757680), 0 for none.
    [[nodiscard]] uint64_t occupant(uint64_t vehicleGuid, uint8_t seat) const;
    /// How many seats the interface shows for a vehicle (0x00757470): its
    /// seat columns that name a seat, each seat a vehicle that is no player
    /// rides in taken by that vehicle's own.
    [[nodiscard]] uint32_t uiSeatCount(uint64_t vehicleGuid, const std::function<bool(uint64_t)>& isPlayer) const;
    /// The vehicle and seat of the interface's seat `index` (0-based,
    /// 0x00757550), in that same order.
    struct UISeat {
        uint64_t vehicleGuid = 0;
        uint8_t seat = 0;
    };
    [[nodiscard]] std::optional<UISeat> uiSeat(uint64_t vehicleGuid, uint32_t index,
                                               const std::function<bool(uint64_t)>& isPlayer) const;

    /// The unit went out of view: neither a vehicle nor in a seat.
    void forget(uint64_t guid);
    void clear();

private:
    std::unordered_map<uint64_t, uint32_t> vehicleIds_;
    std::unordered_map<uint64_t, Passenger> passengers_;
    std::unordered_map<uint64_t, uint32_t> moveFlags2_;
    std::unordered_map<uint64_t, float> pitches_;
    std::unordered_map<uint64_t, SeatChange> changes_;
    std::unordered_map<uint64_t, uint8_t> seatStates_;
    std::unordered_map<uint64_t, uint32_t> seatChangeAskedMs_;
    const VehicleTables* tables_ = nullptr;
};

/// The pitch a vehicle row lets its aim take (0x005fb3a0, 0x005f9e60):
/// PitchMin..PitchMax with Flags 0x40, else a quarter turn either way.
struct AimLimits {
    float min = -1.5707964f;
    float max = 1.5707964f;
};
[[nodiscard]] inline AimLimits aimLimits(const VehicleRecord* row) {
    if (row && (row->flags & kVehicleFlagPitchLimits)) return {row->pitchMin, row->pitchMax};
    return {};
}
/// A pitch asked of a unit, held in its row's limits; a unit with no row
/// takes it as asked (0x005fb3a0).
[[nodiscard]] inline float clampAim(const VehicleRecord* row, float pitch) {
    if (!row) return pitch;
    const AimLimits l = aimLimits(row);
    if (pitch < l.min) return l.min;
    if (l.max <= pitch) return l.max;
    return pitch;
}
/// A pitch as a share of the limits, 0 where they are one (0x005fa910,
/// 0x005f9e60).
[[nodiscard]] inline float normalizedAim(const VehicleRecord* row, float pitch) {
    const AimLimits l = aimLimits(row);
    const float range = l.max - l.min;
    if (range <= 0.0001f && range >= -0.0001f) return 0.0f;
    return (pitch - l.min) / range;
}
/// The aim's power as VehicleAimSetNormPower keeps it (0x005f9f10): below 0
/// is 0 and 1 or more is 1.
[[nodiscard]] inline float clampAimPower(float power) {
    if (power < 0.0f) return 0.0f;
    if (1.0f <= power) return 1.0f;
    return power;
}

/// Whether the bonus bar holds the pet's actions (0x005d4ad0): the player
/// looks through its pet (PLAYER_FARSIGHT is the pet) and the pet lives;
/// where the pet is the vehicle the player sits in, only from a seat whose
/// VehicleAbilityDisplay is 1 (a seat with no row lets it).
[[nodiscard]] inline bool petActionsOnBonusBar(uint64_t farsight, uint64_t pet, bool petAlive,
                                               bool petIsPlayersVehicle, const SeatRecord* playerSeat) {
    if (farsight == 0 || pet != farsight || !petAlive) return false;
    if (petIsPlayersVehicle && playerSeat) return playerSeat->vehicleAbilityDisplay == 1;
    return true;
}

/// What UnitVehicleSeatInfo answers for a seat (0x007579e0).
struct UISeatInfo {
    const char* controlType = "None";  ///< "Root"/"Child" for Flags 0x800
    uint64_t occupant = 0;
    bool ejectable = false;      ///< FlagsB 0x20
    bool canSwitchSeats = false; ///< Flags 0x4000000
};
[[nodiscard]] std::optional<UISeatInfo> uiSeatInfo(const VehicleRoster& roster, uint64_t rootGuid, uint32_t index,
                                                   const std::function<bool(uint64_t)>& isPlayer);

/// A guid the transport chain is walked through (0x0074c650): a vehicle's
/// (high 0xF15x) or a player's.
[[nodiscard]] inline bool isVehicleGuid(uint64_t guid) {
    return (static_cast<uint32_t>(guid >> 32) & 0xf0f00000u) == 0xf0500000u;
}
[[nodiscard]] inline bool isPlayerGuid(uint64_t guid) {
    return (guid & 0xf000000000000000ull) == 0 && (guid & 0xf07fffffffffffffull) != 0;
}

}  // namespace wowee::game::vehicle
