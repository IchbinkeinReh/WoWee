#pragma once

#include "game/protocol_constants.hpp"
#include "game/update_field_table.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <string>
#include <array>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <optional>
#include <utility>
#include <mutex>
#include <chrono>
#include "math/spline.hpp"
#include "game/flat_field_map.hpp"

namespace wowee {
namespace game {

/**
 * Object type IDs for WoW 3.3.5a
 */
enum class ObjectType : uint8_t {
    OBJECT = 0,
    ITEM = 1,
    CONTAINER = 2,
    UNIT = 3,
    PLAYER = 4,
    GAMEOBJECT = 5,
    DYNAMICOBJECT = 6,
    CORPSE = 7
};

/**
 * Object type masks for update packets
 */
enum class TypeMask : uint16_t {
    OBJECT = 0x0001,
    ITEM = 0x0002,
    CONTAINER = 0x0004,
    UNIT = 0x0008,
    PLAYER = 0x0010,
    GAMEOBJECT = 0x0020,
    DYNAMICOBJECT = 0x0040,
    CORPSE = 0x0080
};

// UNIT_DYNAMIC_FLAGS values shared by the supported legacy expansions.
// Keep these named: 0x08 is TAPPED_BY_PLAYER, while the actual DEAD bit is
// 0x20. Confusing the two leaves pre-existing corpses standing after login.
inline constexpr uint32_t UNIT_DYNFLAG_LOOTABLE         = 0x00000001;
inline constexpr uint32_t UNIT_DYNFLAG_TAPPED           = 0x00000004;
inline constexpr uint32_t UNIT_DYNFLAG_TAPPED_BY_PLAYER = 0x00000008;
inline constexpr uint32_t UNIT_DYNFLAG_DEAD             = 0x00000020;

/// CREATE updates may omit zero-valued health. In that case the dynamic corpse
/// flags are the authoritative indication that the unit must spawn dead.
inline bool isUnitCorpseState(uint32_t health, uint32_t maxHealth, uint32_t dynamicFlags) {
    return (maxHealth > 0 && health == 0) ||
           (dynamicFlags & (UNIT_DYNFLAG_DEAD | UNIT_DYNFLAG_LOOTABLE)) != 0;
}

/**
 * Update types for SMSG_UPDATE_OBJECT
 */
enum class UpdateType : uint8_t {
    VALUES = 0,              // Partial update (changed fields only)
    MOVEMENT = 1,            // Movement update
    CREATE_OBJECT = 2,       // Create new object (full data)
    CREATE_OBJECT2 = 3,      // Create new object (alternate format)
    OUT_OF_RANGE_OBJECTS = 4, // Objects left view range
    NEAR_OBJECTS = 5         // Objects entered view range
};

/// The update type's own name, for a log line.
///
/// Beside the enum rather than in the two parsers that print it - the WotLK one
/// and the vanilla one had an identical copy each, and they are the two files
/// where a value added to this list would need noticing.
inline const char* updateTypeName(UpdateType type) {
    switch (type) {
        case UpdateType::VALUES:               return "VALUES";
        case UpdateType::MOVEMENT:             return "MOVEMENT";
        case UpdateType::CREATE_OBJECT:        return "CREATE_OBJECT";
        case UpdateType::CREATE_OBJECT2:       return "CREATE_OBJECT2";
        case UpdateType::OUT_OF_RANGE_OBJECTS: return "OUT_OF_RANGE_OBJECTS";
        case UpdateType::NEAR_OBJECTS:         return "NEAR_OBJECTS";
    }
    return "UNKNOWN";
}

/// A unit's movement speeds as the client's CMovement keeps them, walk at
/// +0x90 through flight-back at +0xa8 and the turn rate at +0xac: yards a
/// second, and radians a second for the turn rate. The defaults are the base
/// speeds every 3.3.5a unit starts with; the create block's movement section
/// and the speed-change opcodes replace them.
struct MovementSpeeds {
    float walk = 2.5f;
    float run = 7.0f;
    float runBack = 4.5f;
    float swim = 4.722222f;
    float swimBack = 2.5f;
    float flight = 7.0f;
    float flightBack = 4.5f;
    float turnRate = 3.14159265f;
};

/// What a movement packet from a unit did to its jump; see
/// Entity::reportMoveFlags.
enum class ReportedJump : uint8_t {
    None,
    Launch,     // MSG_MOVE_JUMP: JumpStart, then the Jump loop
    Land,       // Down from a jump or a long fall: the landing animation
    TakeOff,    // Into flight (CMSG_MOVE_SET_FLY with FLYING): JumpStart
    FlightLand, // Out of flight (CMSG_MOVE_SET_FLY without it): JumpLandRun
};

/// What the client's handler for another unit's movement opcode
/// (FUN_0073ed10, called after the packet is applied) does with the unit's
/// animation, as Entity::reportMoveFlags takes it.
enum class MoveOpcodeKind : uint8_t {
    None,               // Heartbeat, facing, pitch, landing...: nothing more
    Jump,               // MSG_MOVE_JUMP (0xbb): JumpStart
    SetFly,             // CMSG_MOVE_SET_FLY as relayed (0x346): take off or land
    Locomotion,         // Start, stop, turn, swim, ascend...: locomotion again
    LocomotionIfMoving, // Walk or run mode: locomotion again if under way
};

/// The client's fall (FUN_00986f00): how far a unit has dropped t seconds
/// into a fall begun at verticalSpeed - downwards positive, so a jump starts
/// below zero (-7.955547, FUN_009883f0) and the distance is negative while
/// it rises - under 19.291105 yd/s^2 of gravity, up to a terminal speed of
/// 60.148003, or 7 with SAFE_FALL (0x20000000; DAT_00b2d9e8, DAT_00b2d9ec).
[[nodiscard]] inline float fallDistance(float t, float verticalSpeed, bool safeFall) noexcept {
    constexpr float kGravity = 19.291105f;
    const float terminal = safeFall ? 7.0f : 60.148003f;
    const float v0 = std::min(verticalSpeed, terminal);
    if (kGravity * t + v0 <= terminal) return (0.5f * kGravity * t + v0) * t;
    const float toTerminal = (terminal - v0) / kGravity;
    return (0.5f * kGravity * toTerminal + v0) * toTerminal + (t - toTerminal) * terminal;
}

/// The jump or fall a movement packet with FALLING (0x1000) carries - its
/// fall time and jump block - as Entity::startMoveByFlags takes it, in
/// canonical axes.
struct FallMotion {
    float fallTimeSec = 0.0f;       // Fallen for this long at the packet
    float verticalSpeed = 0.0f;     // At the start of the fall; see fallDistance
    float dirX = 0.0f, dirY = 0.0f; // The jump's horizontal direction
    float horizontalSpeed = 0.0f;
};

/// The jump block the client sends with MSG_MOVE_JUMP, in the server's axes
/// and in wire order: the vertical speed, the x and y of the jump's direction
/// (MovementInfo calls them jumpSinAngle and jumpCosAngle, in that order -
/// the names are the wrong way round), and the horizontal speed.
struct OutgoingJump {
    float verticalSpeed = 0.0f;
    float dirX = 0.0f, dirY = 0.0f;
    float horizontalSpeed = 0.0f;
};

/// What the client puts in it on a jump. The vertical speed is the jump
/// impulse, -7.955547 - the fall's speeds are downward-positive (FUN_009883f0
/// -> FUN_00988370). The direction is the way the keys move the unit, not
/// only the facing: backing up turns it by pi, a strafe by pi/2 to the left,
/// diagonals half way (FUN_00988df0). The speed is the unit's current one
/// (FUN_00987570): walk when walking, run-back when only backing up, else
/// run; and none when no key is moving it, so the jump goes straight up.
inline OutgoingJump outgoingJumpBlock(uint32_t flags, float orientation, float walkSpeed,
                                      float runSpeed, float runBackSpeed) {
    constexpr uint32_t kForward = 0x1, kBackward = 0x2, kStrafeLeft = 0x4,
                       kStrafeRight = 0x8, kWalking = 0x100;
    OutgoingJump j;
    j.verticalSpeed = -7.955547f;
    const float fwd = ((flags & kForward) ? 1.0f : 0.0f) - ((flags & kBackward) ? 1.0f : 0.0f);
    const float side = ((flags & kStrafeLeft) ? 1.0f : 0.0f) - ((flags & kStrafeRight) ? 1.0f : 0.0f);
    if (fwd == 0.0f && side == 0.0f) return j;
    const float dir = orientation + std::atan2(side, fwd);
    j.dirX = std::cos(dir);
    j.dirY = std::sin(dir);
    const bool onlyBack = (flags & kBackward) && !(flags & kForward);
    j.horizontalSpeed = (flags & kWalking) ? walkSpeed : onlyBack ? runBackSpeed : runSpeed;
    return j;
}

/**
 * Base entity class for all game objects
 */
class Entity {
public:
    Entity() = default;
    explicit Entity(uint64_t guid) : guid(guid) {}
    virtual ~Entity() = default;

    // GUID access
    [[nodiscard]] uint64_t getGuid() const { return guid; }
    void setGuid(uint64_t g) { guid = g; }

    // Position
    [[nodiscard]] float getX() const { return x; }
    [[nodiscard]] float getY() const { return y; }
    [[nodiscard]] float getZ() const { return z; }
    [[nodiscard]] float getOrientation() const { return orientation; }
    // Update orientation only, without disrupting an in-progress movement interpolation.
    void setOrientation(float o) { orientation = o; }

    void setPosition(float px, float py, float pz, float o) {
        x = px;
        y = py;
        z = pz;
        orientation = o;
        isMoving_ = false; // Instant position set cancels interpolation
        usePathMode_ = false;
        activeSpline_.reset();
        resetMotionExtras();
    }

    // Multi-segment path movement (Catmull-Rom spline interpolation)
    void startMoveAlongPath(const std::vector<std::array<float, 3>>& packetPath, float destO, float totalDuration) {
        if (packetPath.empty()) return;
        if (packetPath.size() == 1 || totalDuration <= 0.0f) {
            startMoveTo(packetPath.back()[0], packetPath.back()[1], packetPath.back()[2], destO, totalDuration);
            return;
        }
        resetMotionExtras();
        // Snap position if in overrun phase: the renderer showed moveEnd.
        if (isMoving_ && moveElapsed_ >= moveDuration_) {
            x = moveEndX_; y = moveEndY_; z = moveEndZ_;
        }
        // The client's spline starts where the unit is drawn, not where the
        // packet says the server had it: CGUnit_C's monster-move handler
        // (FUN_0073c8e0) puts the unit's current position (vfunc +0x30) first
        // and keeps the packet's start after it only when the two are more
        // than 0.00077160494 apart squared. Starting at the packet's start
        // jumped a unit redirected mid-move back or forward to it.
        std::vector<std::array<float, 3>> path;
        path.reserve(packetPath.size() + 1);
        {
            const float sx = packetPath[0][0] - x;
            const float sy = packetPath[0][1] - y;
            const float sz = packetPath[0][2] - z;
            constexpr float kSameStartDistanceSq = 0.00077160494f;
            path.push_back({x, y, z});
            if (sx * sx + sy * sy + sz * sz >= kSameStartDistanceSq) path.push_back(packetPath[0]);
            path.insert(path.end(), packetPath.begin() + 1, packetPath.end());
        }
        // Build cumulative distances for proportional time assignment.
        // (Stored in a tiny stack/heap vector - typical N is <=15 waypoints,
        // and keeping float precision matters for the timeMs rescale below.)
        std::vector<float> cumDist(path.size(), 0.0f);
        float totalDist = 0.0f;
        for (size_t i = 1; i < path.size(); i++) {
            float dx = path[i][0] - path[i - 1][0];
            float dy = path[i][1] - path[i - 1][1];
            float dz = path[i][2] - path[i - 1][2];
            totalDist += std::sqrt(dx * dx + dy * dy + dz * dz);
            cumDist[i] = totalDist;
        }
        if (totalDist < 0.001f) {
            startMoveTo(path.back()[0], path.back()[1], path.back()[2], destO, totalDuration);
            return;
        }
        // Build SplineKeys with distance-proportional time
        uint32_t durationMs = static_cast<uint32_t>(totalDuration * 1000.0f);
        const float invTotalDist = static_cast<float>(durationMs) / totalDist;
        std::vector<math::SplineKey> keys(path.size());
        for (size_t i = 0; i < path.size(); i++) {
            keys[i].timeMs = static_cast<uint32_t>(cumDist[i] * invTotalDist);
            keys[i].position = {path[i][0], path[i][1], path[i][2]};
        }
        activeSpline_.emplace(std::move(keys), /*timeClosed=*/false);
        splineDurationMs_ = durationMs;

        // Velocity for dead-reckoning after path completes, from the previous
        // destination as startMoveTo takes it.
        const float fromX = isMoving_ ? moveEndX_ : x;
        const float fromY = isMoving_ ? moveEndY_ : y;
        const float impliedVX = (path.back()[0] - fromX) / totalDuration;
        const float impliedVY = (path.back()[1] - fromY) / totalDuration;
        const float impliedVZ = (path.back()[2] - path[0][2]) / totalDuration;
        const float alpha = 0.65f;
        velX_ = alpha * impliedVX + (1.0f - alpha) * velX_;
        velY_ = alpha * impliedVY + (1.0f - alpha) * velY_;
        velZ_ = alpha * impliedVZ + (1.0f - alpha) * velZ_;

        moveEndX_ = path.back()[0]; moveEndY_ = path.back()[1]; moveEndZ_ = path.back()[2];
        moveDuration_ = totalDuration;
        moveElapsed_ = 0.0f;
        // The spline's length over its duration: what the client's
        // CMovement::GetCurrentSpeed (FUN_00987570) answers for a unit on one.
        moveSpeed_ = totalDist / totalDuration;
        orientation = destO;
        isMoving_ = true;
        usePathMode_ = true;
    }

    // Movement interpolation (syncs entity position with renderer during movement)
    void startMoveTo(float destX, float destY, float destZ, float destO, float durationSec) {
        usePathMode_ = false;
        activeSpline_.reset();
        resetMotionExtras();
        if (durationSec <= 0.0f) {
            setPosition(destX, destY, destZ, destO);
            return;
        }
        // Movement heartbeats and repeated spline destinations can carry a
        // positive duration without any actual displacement. Treat these as
        // authoritative stops; otherwise isActivelyMoving() drives Run/Walk
        // while the model has nowhere to move. This applies equally to nearby
        // players and creatures; a player whose flags say it is still under
        // way goes through startMoveByFlags instead.
        const float remainingX = destX - x;
        const float remainingY = destY - y;
        const float remainingZ = destZ - z;
        constexpr float kNoOpMoveDistanceSq = 0.02f * 0.02f;
        if (remainingX * remainingX + remainingY * remainingY +
                remainingZ * remainingZ <= kNoOpMoveDistanceSq) {
            setPosition(destX, destY, destZ, destO);
            return;
        }
        // If we're in the dead-reckoning overrun phase, snap x/y/z back to the
        // destination before using them as the new start.  The renderer was showing
        // the entity at moveEnd (via getLatest) during overrun, so the new
        // interpolation must start there to avoid a visible teleport. A unit
        // moved on by its flags was drawn where x/y/z are, so it carries on
        // from there.
        snapOverrunToDestination();
        movingByFlags_ = false;
        // Derive velocity from the displacement this packet implies.
        // Use the previous destination (not current lerped pos) as the "from" so
        // variable network timing doesn't inflate/shrink the implied speed.
        float fromX = isMoving_ ? moveEndX_ : x;
        float fromY = isMoving_ ? moveEndY_ : y;
        float fromZ = isMoving_ ? moveEndZ_ : z;
        float impliedVX = (destX - fromX) / durationSec;
        float impliedVY = (destY - fromY) / durationSec;
        float impliedVZ = (destZ - fromZ) / durationSec;
        // Exponential moving average on velocity - 65% new sample, 35% previous.
        // Smooths out jitter from irregular server update intervals (~200-600ms)
        // without introducing visible lag on direction changes.
        const float alpha = 0.65f;
        velX_ = alpha * impliedVX + (1.0f - alpha) * velX_;
        velY_ = alpha * impliedVY + (1.0f - alpha) * velY_;
        velZ_ = alpha * impliedVZ + (1.0f - alpha) * velZ_;

        moveStartX_ = x; moveStartY_ = y; moveStartZ_ = z;
        moveEndX_ = destX; moveEndY_ = destY; moveEndZ_ = destZ;
        moveDuration_ = durationSec;
        moveElapsed_ = 0.0f;
        const float segX = destX - x, segY = destY - y, segZ = destZ - z;
        moveSpeed_ = std::sqrt(segX * segX + segY * segY + segZ * segZ) / durationSec;
        orientation = destO;
        isMoving_ = true;
    }

    /// Moves a unit on from a movement packet by the packet's flags, as the
    /// client moves another player between that player's packets: from the
    /// packet's position and facing, at the speed CMovement::GetCurrentSpeed
    /// (FUN_00987570, speedForFlags) gives for the flags, along the direction
    /// FUN_00988df0 sets up for them - ahead, behind, to either side, or
    /// halfway between - and up or down at 45 degrees while ascending or
    /// descending (FUN_00987700). FUN_00987b50 picks the displacement for the
    /// time since the packet; while TURN_LEFT or TURN_RIGHT is set the facing
    /// turns at the unit's turn rate (FUN_00987770) and the path curves with
    /// it (FUN_00987a00), except while falling, when only the facing turns.
    ///
    /// The unit starts moving on the packet that sets the flags - a start
    /// packet, which carries no displacement of its own - instead of on the
    /// next heartbeat's. It is drawn where it was when the packet arrived and
    /// the difference to the packet's track is taken out over correctionSec,
    /// as a heartbeat's correction is. It is moved on for at most
    /// kFlagDeadReckonLimitSec after the packet. With no direction flag the
    /// packet is a startMoveTo, turning if a turn flag is set.
    ///
    /// Falling, with the packet's fall given, the unit drops by gravity
    /// instead: FUN_00988990 takes the fall time, vertical speed, direction
    /// and speed from the jump block and puts the fall's start fallDistance
    /// above the packet's height, and FUN_007618b0 sets the unit that far
    /// under it each step. Across it moves only while a direction flag (0xf)
    /// is set - FUN_00988df0 keeps the jump's direction while falling, and
    /// FUN_00987b50 moves along it at the jump's speed (FUN_009876b0) and
    /// never in an arc - so a jump in place goes straight up and down.
    void startMoveByFlags(float px, float py, float pz, float po,
                          uint32_t flags, uint32_t flags2, float correctionSec,
                          const std::optional<FallMotion>& fall = std::nullopt) {
        const float speed = speedForFlags(speeds_, flags);
        // Backward wins over forward and left over right, as FUN_00988df0
        // tests them; ascending over descending, as FUN_00987a00 does.
        const float ahead = (flags & kBackward) ? -1.0f : (flags & kForward) ? 1.0f : 0.0f;
        const float left = (flags & kStrafeLeft) ? 1.0f : (flags & kStrafeRight) ? -1.0f : 0.0f;
        const float up = (flags & kAscending) ? 1.0f : (flags & kDescending) ? -1.0f : 0.0f;
        const bool horizontal = ahead != 0.0f || left != 0.0f;

        // FUN_00987770: the turn rate, positive to the left, at three
        // quarters while the unit is also moving or falling (0xc0100f)
        // unless MOVEMENTFLAG2_FULL_SPEED_TURNING (0x8) is set.
        float turn = 0.0f;
        if (flags & kTurnLeft) turn = speeds_.turnRate;
        else if (flags & kTurnRight) turn = -speeds_.turnRate;
        if ((flags & kTurnSlowingFlags) != 0 && (flags2 & kFullSpeedTurning) == 0) turn *= 0.75f;

        if (!fall && (speed <= 0.0f || (!horizontal && up == 0.0f))) {
            startMoveTo(px, py, pz, po, correctionSec);
        } else {
            snapOverrunToDestination();
            usePathMode_ = false;
            activeSpline_.reset();
            resetMotionExtras();
            const bool corrects = correctionSec > 0.0f;
            corrX_ = corrects ? x - px : 0.0f;
            corrY_ = corrects ? y - py : 0.0f;
            corrZ_ = corrects ? z - pz : 0.0f;
            moveStartX_ = px; moveStartY_ = py; moveStartZ_ = pz;
            moveEndX_ = px; moveEndY_ = py; moveEndZ_ = pz;
            moveDuration_ = correctionSec;
            moveElapsed_ = 0.0f;
            // Up or down alone at the full speed (FUN_00987b50's 0x10 case);
            // with a horizontal direction, each at 0.70710677 of it.
            constexpr float kDiagonal = 0.70710677f;
            flagHeading_ = horizontal ? po + std::atan2(left, ahead) : po;
            flagSpeed_ = horizontal ? (up != 0.0f ? speed * kDiagonal : speed) : 0.0f;
            flagClimb_ = up * (horizontal ? speed * kDiagonal : speed);
            moveSpeed_ = speed;
            falling_ = fall.has_value();
            if (fall) {
                const bool across = (flags & kHorizontalFlags) != 0 &&
                                    fall->dirX * fall->dirX + fall->dirY * fall->dirY > 1e-6f;
                flagHeading_ = across ? std::atan2(-fall->dirY, fall->dirX) : po;
                flagSpeed_ = across ? fall->horizontalSpeed : 0.0f;
                flagClimb_ = 0.0f;
                moveSpeed_ = flagSpeed_;
                fallElapsedSec_ = fall->fallTimeSec;
                fallSpeed_ = fall->verticalSpeed;
                safeFall_ = (flags & kSafeFall) != 0;
                fallTopZ_ = pz + fallDistance(fallElapsedSec_, fallSpeed_, safeFall_);
            }
            // FUN_00987b50 only takes the arc when not falling (0x1000).
            flagArcRate_ = (flags & kFalling) ? 0.0f : turn;
            velX_ = flagSpeed_ * std::cos(flagHeading_);
            velY_ = -flagSpeed_ * std::sin(flagHeading_);
            velZ_ = falling_ ? -fallSpeed_ : flagClimb_;
            orientation = po;
            isMoving_ = true;
            movingByFlags_ = true;
        }
        if (turn != 0.0f) {
            turnRate_ = turn;
            turnBase_ = po;
            turnElapsed_ = 0.0f;
        }
    }

    /// The speed a unit moves at under these movement flags, as the client's
    /// CMovement::GetCurrentSpeed (FUN_00987570) answers for a unit not on a
    /// spline: nothing without a direction flag (0xc0000f); flying (0x2000000)
    /// before swimming (0x200000) before the ground; backwards at the back
    /// speed when that is the slower; walking (0x100) at the walk speed, or
    /// the run speed when that is the slower.
    [[nodiscard]] static float speedForFlags(const MovementSpeeds& speeds, uint32_t flags) {
        if ((flags & kDirectionFlags) == 0) return 0.0f;
        const bool back = (flags & kBackward) != 0;
        if (flags & kFlying) {
            return back && speeds.flightBack <= speeds.flight ? speeds.flightBack : speeds.flight;
        }
        if (flags & kSwimming) {
            return back && speeds.swimBack <= speeds.swim ? speeds.swimBack : speeds.swim;
        }
        if (flags & kWalking) return speeds.walk < speeds.run ? speeds.walk : speeds.run;
        return back && speeds.runBack <= speeds.run ? speeds.runBack : speeds.run;
    }

    /// Takes the flags of a movement packet (MSG_MOVE_*) from the unit, what
    /// kind of opcode it came in, and the vertical speed of its jump block
    /// when that was read, and says what the packet did to the unit's jump,
    /// as the client's handlers do for another unit.
    ///
    /// MSG_MOVE_JUMP launches it (FUN_0073ed10, case 0xbb). A unit falling
    /// (0x1000) with a vertical speed (+0xb8, set from the jump block by
    /// FUN_00988990) was launched - a jump or a knockback, whichever opcode
    /// said so - as FUN_00723350 and FUN_006eb730 test it; without the jump
    /// block (before WotLK, where it is not read) the jump opcode stands for
    /// it. A packet that clears FALLING lands the unit (FUN_006eb730 calling
    /// FUN_0073d3d0), and FUN_0073d2b0 plays a landing only after a launch or
    /// a fall far (0x2000), and not into water or into flight (0x2200000). A
    /// fall off a ledge lands on its feet still moving.
    ///
    /// CMSG_MOVE_SET_FLY, relayed, takes the unit off with FLYING (0x2000000)
    /// or lands it without (FUN_0073ed10, case 0x346). Start, stop, turn and
    /// the rest of FUN_0073ed10's cases ask for the unit's locomotion again
    /// at once (FUN_0073ac30) - which cuts a landing short; see
    /// takeLocomotionRequest.
    ReportedJump reportMoveFlags(uint32_t flags, MoveOpcodeKind kind,
                                 std::optional<float> verticalSpeed = std::nullopt) {
        const uint32_t before = reportedMoveFlags_;
        const bool wasLaunched = launched_;
        reportedMoveFlags_ = flags;
        const bool falling = (flags & kFalling) != 0;
        if (verticalSpeed) launched_ = falling && *verticalSpeed != 0.0f;
        else if (kind == MoveOpcodeKind::Jump) launched_ = falling;
        else if (!falling) launched_ = false;
        locomotionRequested_ =
            locomotionRequested_ || kind == MoveOpcodeKind::Locomotion ||
            (kind == MoveOpcodeKind::LocomotionIfMoving && (flags & kTurnSlowingFlags) != 0);
        if (kind == MoveOpcodeKind::SetFly) {
            return (flags & kFlying) != 0 ? ReportedJump::TakeOff : ReportedJump::FlightLand;
        }
        if (kind == MoveOpcodeKind::Jump) return ReportedJump::Launch;
        if (falling || (before & kFalling) == 0) return ReportedJump::None;
        if (!wasLaunched && (before & kFallingFar) == 0) return ReportedJump::None;
        return (flags & (kSwimming | kFlying)) == 0 ? ReportedJump::Land : ReportedJump::None;
    }

    /// The flags of the unit's last movement packet; see reportMoveFlags.
    [[nodiscard]] uint32_t getReportedMoveFlags() const { return reportedMoveFlags_; }

    /// In the air as the client's locomotion choice sees a unit
    /// (FUN_00723350): falling with a vertical speed, or falling far.
    [[nodiscard]] bool isAirborne() const {
        return (reportedMoveFlags_ & kFalling) != 0 &&
               (launched_ || (reportedMoveFlags_ & kFallingFar) != 0);
    }

    /// Whether the unit's last movement packets asked for its locomotion
    /// again (see reportMoveFlags), once: the ask is cleared by reading it.
    [[nodiscard]] bool takeLocomotionRequest() {
        return std::exchange(locomotionRequested_, false);
    }

    /// Moved down by gravity from a packet's fall (startMoveByFlags), rather
    /// than along the ground.
    [[nodiscard]] bool isFallingByFlags() const { return movingByFlags_ && falling_; }

    [[nodiscard]] const MovementSpeeds& getMovementSpeeds() const { return speeds_; }
    MovementSpeeds& movementSpeeds() { return speeds_; }

    /// Faces a unit along the move just started, as the client faces one on
    /// a spline: each step FUN_0098ca00 sets the movement facing to the
    /// direction of the spline's tangent when its x/y length squared is over
    /// 0.001849, the other way for an inverted spline - unless the spline is
    /// falling or keeps its orientation, when the unit keeps fixedFacing, the
    /// one it had. On arrival FUN_006eb0b0 sets the final facing the monster
    /// move asked for (FINAL_ANGLE, FINAL_TARGET or FINAL_POINT); without one
    /// the unit keeps the last. Call after startMoveTo or startMoveAlongPath.
    /// A move that did not start has already taken its orientation, as the
    /// client's monster move handler (FUN_0073c8e0) faces a unit at once when
    /// it launches no spline.
    void faceAlongMove(std::optional<float> finalFacing, std::optional<float> fixedFacing,
                       bool inverted) {
        if (!isMoving_ || movingByFlags_) return;
        finalFacing_ = finalFacing;
        pathFacingOffset_ = inverted ? kPi : 0.0f;
        if (fixedFacing) {
            orientation = *fixedFacing;
        } else if (usePathMode_ && activeSpline_) {
            faceAlongPath_ = true;
            const math::SplineEvalResult start = activeSpline_->evaluate(0);
            faceDirection(start.tangent.x, start.tangent.y);
        } else {
            faceDirection(moveEndX_ - moveStartX_, moveEndY_ - moveStartY_);
        }
    }

    /// The facing the unit's model is drawn at. The client draws every unit
    /// but the active player turning towards its movement facing rather than
    /// at it: each frame (CWorldFrame's update, FUN_004fa5f0, through
    /// FUN_00739630) FUN_00735f60 moves the model half of the way there,
    /// averaging the last four differences while they keep the same sign and
    /// never stepping past the target, and sets it when within 0.01. The
    /// per-frame update is updateMovement.
    [[nodiscard]] float getModelFacing() const { return modelFacingSet_ ? modelFacing_ : orientation; }

    /// The speed of the current movement, in yards a second, while the
    /// entity is actively moving; zero otherwise. Set from each segment's
    /// length over its duration, or by setMoveSpeed when the caller knows it
    /// better.
    [[nodiscard]] float getMoveSpeed() const { return isActivelyMoving() ? moveSpeed_ : 0.0f; }
    void setMoveSpeed(float yardsPerSecond) {
        if (std::isfinite(yardsPerSecond) && yardsPerSecond >= 0.0f) moveSpeed_ = yardsPerSecond;
    }

    void updateMovement(float deltaTime) {
        if (turnRate_ != 0.0f) {
            turnElapsed_ += deltaTime;
            orientation = wrapAngle(turnBase_ + turnRate_ * turnElapsed_);
        }
        if (isMoving_) {
            if (movingByFlags_) advanceByFlags(deltaTime);
            else advanceSegment(deltaTime);
        }
        updateModelFacing();
    }

    [[nodiscard]] bool isEntityMoving() const { return isMoving_; }

    /// True only during the active interpolation phase (before reaching destination).
    /// Unlike isEntityMoving(), this does NOT include the dead-reckoning overrun window,
    /// so animations (Run/Walk) should use this to avoid "running in place" after arrival.
    [[nodiscard]] bool isActivelyMoving() const {
        return isMoving_ && (moveElapsed_ < moveDuration_ || movingByFlags_);
    }

    // Returns the latest server-authoritative position: destination if moving, current if not.
    // Unlike getX/Y/Z (which only update via updateMovement), this always reflects the
    // last known server position regardless of distance culling.
    [[nodiscard]] float getLatestX() const { return isMoving_ ? moveEndX_ : x; }
    [[nodiscard]] float getLatestY() const { return isMoving_ ? moveEndY_ : y; }
    [[nodiscard]] float getLatestZ() const { return isMoving_ ? moveEndZ_ : z; }

    // Object type
    [[nodiscard]] ObjectType getType() const { return type; }
    void setType(ObjectType t) { type = t; }

    /// True if this entity is a Unit or Player (both derive from Unit).
    [[nodiscard]] bool isUnit() const { return type == ObjectType::UNIT || type == ObjectType::PLAYER; }

    // Fields (for update values)
    void setField(uint16_t index, uint32_t value) {
        fields[index] = value;
    }

    [[nodiscard]] uint32_t getField(uint16_t index) const {
        auto it = fields.find(index);
        return (it != fields.end()) ? it->second : 0;
    }

    [[nodiscard]] bool hasField(uint16_t index) const {
        return fields.find(index) != fields.end();
    }

    [[nodiscard]] const FlatFieldMap& getFields() const {
        return fields;
    }

protected:
    uint64_t guid = 0;
    ObjectType type = ObjectType::OBJECT;

    // Position
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float orientation = 0.0f;

    // Update fields (dynamic values) - flat sorted vector. See FlatFieldMap docs.
    FlatFieldMap fields;

    // Movement interpolation state
    bool isMoving_ = false;
    bool usePathMode_ = false;
    float moveStartX_ = 0, moveStartY_ = 0, moveStartZ_ = 0;
    float moveEndX_ = 0, moveEndY_ = 0, moveEndZ_ = 0;
    float moveDuration_ = 0;
    float moveElapsed_ = 0;
    float velX_ = 0, velY_ = 0, velZ_ = 0;  // Smoothed velocity for dead reckoning
    float moveSpeed_ = 0;                   // Current movement's speed; see getMoveSpeed
    // CatmullRom spline for multi-segment path movement (replaces linear pathPoints_/pathSegDists_)
    std::optional<math::CatmullRomSpline> activeSpline_;
    uint32_t splineDurationMs_ = 0;

private:
    static constexpr float kPi = 3.14159265f;
    // Movement flag bits, as MovementFlags has them (world_packets.hpp).
    static constexpr uint32_t kForward = 0x1, kBackward = 0x2;
    static constexpr uint32_t kStrafeLeft = 0x4, kStrafeRight = 0x8;
    static constexpr uint32_t kTurnLeft = 0x10, kTurnRight = 0x20;
    static constexpr uint32_t kWalking = 0x100, kFalling = 0x1000, kFallingFar = 0x2000;
    static constexpr uint32_t kSwimming = 0x200000, kFlying = 0x2000000;
    static constexpr uint32_t kAscending = 0x400000, kDescending = 0x800000;
    static constexpr uint32_t kHorizontalFlags = 0xf;
    static constexpr uint32_t kSafeFall = 0x20000000;
    static constexpr uint32_t kDirectionFlags = 0xc0000f;
    static constexpr uint32_t kTurnSlowingFlags = 0xc0100f;
    static constexpr uint32_t kFullSpeedTurning = 0x8;  // MOVEMENTFLAG2_FULL_SPEED_TURNING
    /// How long a unit is moved on by its flags after its last packet. The
    /// client sends a heartbeat every 500 ms while moving (FUN_006f09f0), so
    /// this is two missed ones.
    static constexpr float kFlagDeadReckonLimitSec = 1.0f;

    static float wrapAngle(float a) { return std::remainder(a, 2.0f * kPi); }

    void resetMotionExtras() {
        movingByFlags_ = false;
        falling_ = false;
        turnRate_ = 0.0f;
        faceAlongPath_ = false;
        finalFacing_.reset();
    }

    void snapOverrunToDestination() {
        if (isMoving_ && !movingByFlags_ && moveElapsed_ >= moveDuration_) {
            x = moveEndX_;
            y = moveEndY_;
            z = moveEndZ_;
        }
    }

    void faceDirection(float dx, float dy) {
        constexpr float kMinTangentSq = 0.0018490001f;  // FUN_0098ca00
        if (dx * dx + dy * dy > kMinTangentSq) {
            orientation = wrapAngle(std::atan2(-dy, dx) + pathFacingOffset_);
        }
    }

    void advanceSegment(float deltaTime) {
        moveElapsed_ += deltaTime;
        if (moveElapsed_ < moveDuration_) {
            if (usePathMode_ && activeSpline_) {
                // Catmull-Rom spline interpolation
                uint32_t pathTimeMs = static_cast<uint32_t>(moveElapsed_ * 1000.0f);
                if (pathTimeMs >= splineDurationMs_) pathTimeMs = splineDurationMs_ - 1;
                const math::SplineEvalResult eval = activeSpline_->evaluate(pathTimeMs);
                x = eval.position.x;
                y = eval.position.y;
                z = eval.position.z;
                if (faceAlongPath_) faceDirection(eval.tangent.x, eval.tangent.y);
            } else {
                // Single-segment linear interpolation
                float t = moveElapsed_ / moveDuration_;
                x = moveStartX_ + (moveEndX_ - moveStartX_) * t;
                y = moveStartY_ + (moveEndY_ - moveStartY_) * t;
                z = moveStartZ_ + (moveEndZ_ - moveStartZ_) * t;
            }
            return;
        }
        // Arrived: the facing the move was to end on (FUN_006eb0b0).
        faceAlongPath_ = false;
        if (finalFacing_) {
            orientation = *finalFacing_;
            finalFacing_.reset();
        }
        // Past the interpolation window: dead-reckon at the smoothed velocity
        // rather than freezing in place. Cap to one extra interval so we don't
        // drift endlessly if the entity stops sending packets.
        float overrun = moveElapsed_ - moveDuration_;
        if (overrun < moveDuration_) {
            x = moveEndX_ + velX_ * overrun;
            y = moveEndY_ + velY_ * overrun;
            z = moveEndZ_ + velZ_ * overrun;
        } else {
            // Two intervals with no update - entity has probably stopped.
            x = moveEndX_; y = moveEndY_; z = moveEndZ_;
            velX_ = 0.0f; velY_ = 0.0f; velZ_ = 0.0f;
            isMoving_ = false;
        }
    }

    void advanceByFlags(float deltaTime) {
        moveElapsed_ += deltaTime;
        const float t = std::min(moveElapsed_, kFlagDeadReckonLimitSec);
        // The heading turns with the facing; integrated, the path is an arc
        // of radius speed / turn rate (FUN_00987a00), a line when not turning.
        float dx = flagSpeed_ * t * std::cos(flagHeading_);
        float dy = -flagSpeed_ * t * std::sin(flagHeading_);
        if (std::abs(flagArcRate_) > 1e-4f) {
            const float heading = flagHeading_ + flagArcRate_ * t;
            dx = flagSpeed_ * (std::sin(heading) - std::sin(flagHeading_)) / flagArcRate_;
            dy = flagSpeed_ * (std::cos(heading) - std::cos(flagHeading_)) / flagArcRate_;
        }
        const float correction =
            moveElapsed_ < moveDuration_ ? 1.0f - moveElapsed_ / moveDuration_ : 0.0f;
        x = moveEndX_ + dx + corrX_ * correction;
        y = moveEndY_ + dy + corrY_ * correction;
        const float carriedZ =
            falling_ ? fallTopZ_ - fallDistance(fallElapsedSec_ + t, fallSpeed_, safeFall_)
                     : moveEndZ_ + flagClimb_ * t;
        z = carriedZ + corrZ_ * correction;
        if (moveElapsed_ >= kFlagDeadReckonLimitSec) {
            // No packet for too long: stand where it was carried to.
            isMoving_ = false;
            movingByFlags_ = false;
            falling_ = false;
        }
    }

    void updateModelFacing() {
        if (!modelFacingSet_) {
            modelFacing_ = orientation;
            modelFacingSet_ = true;
            return;
        }
        const float diff = wrapAngle(orientation - modelFacing_);
        if (std::abs(diff) <= 0.01f) {
            facingDiffs_[0] = 0.0f;
            modelFacing_ = orientation;
            return;
        }
        if ((diff >= 0.0f && facingDiffs_[0] < 0.0f) || (diff < 0.0f && facingDiffs_[0] > 0.0f)) {
            facingDiffs_[0] = 0.0f;
        }
        float step = diff;
        if (facingDiffs_[0] == 0.0f) {
            facingDiffs_.fill(diff);
        } else {
            facingDiffs_[3] = facingDiffs_[2];
            facingDiffs_[2] = facingDiffs_[1];
            facingDiffs_[1] = facingDiffs_[0];
            facingDiffs_[0] = diff;
            step = (facingDiffs_[0] + facingDiffs_[1] + facingDiffs_[2] + facingDiffs_[3]) * 0.25f;
            if (diff <= 0.0f ? step < diff : step > diff) step = diff;
        }
        modelFacing_ = wrapAngle(modelFacing_ + step * 0.5f);
    }

    MovementSpeeds speeds_;
    // The last movement packet's flags, and whether the fall in them is a
    // jump's; see reportMoveFlags.
    uint32_t reportedMoveFlags_ = 0;
    bool launched_ = false;
    // Asked for its locomotion again; see takeLocomotionRequest.
    bool locomotionRequested_ = false;
    // Moved on by its flags (startMoveByFlags): moveEnd is the packet's
    // position, the corr* offset is taken out over moveDuration_.
    bool movingByFlags_ = false;
    float corrX_ = 0, corrY_ = 0, corrZ_ = 0;
    float flagHeading_ = 0;                 // Direction of travel at the packet
    float flagSpeed_ = 0;                   // Horizontal speed
    float flagClimb_ = 0;                   // Vertical speed
    float flagArcRate_ = 0;                 // Rate the direction turns at
    // Falling by the packet's fall: from fallTopZ_, where the fall began,
    // fallElapsedSec_ into it at the packet; see fallDistance.
    bool falling_ = false;
    bool safeFall_ = false;
    float fallTopZ_ = 0, fallElapsedSec_ = 0, fallSpeed_ = 0;
    // Turning by TURN_LEFT / TURN_RIGHT, from turnBase_ at the packet.
    float turnRate_ = 0, turnBase_ = 0, turnElapsed_ = 0;
    // Facing along a monster move; see faceAlongMove.
    bool faceAlongPath_ = false;
    float pathFacingOffset_ = 0;
    std::optional<float> finalFacing_;
    // The drawn facing; see getModelFacing.
    float modelFacing_ = 0;
    bool modelFacingSet_ = false;
    std::array<float, 4> facingDiffs_{};
};

/**
 * Unit entity (NPCs, creatures, players)
 */
class Unit : public Entity {
public:
    Unit() { type = ObjectType::UNIT; }
    explicit Unit(uint64_t guid) : Entity(guid) { type = ObjectType::UNIT; }

    // Name
    [[nodiscard]] const std::string& getName() const { return name; }
    void setName(const std::string& n) { name = n; }

    // Health
    [[nodiscard]] uint32_t getHealth() const { return health; }
    void setHealth(uint32_t h) { health = h; }

    [[nodiscard]] uint32_t getMaxHealth() const { return maxHealth; }
    void setMaxHealth(uint32_t h) { maxHealth = h; }

    // Power (mana/rage/energy) - indexed by power type (0-6)
    [[nodiscard]] uint32_t getPower() const { return powers[powerType < 7 ? powerType : 0]; }
    void setPower(uint32_t p) { powers[powerType < 7 ? powerType : 0] = p; }
    void setPowerByType(uint8_t type, uint32_t p) { if (type < 7) powers[type] = p; }

    [[nodiscard]] uint32_t getMaxPower() const { return maxPowers[powerType < 7 ? powerType : 0]; }
    void setMaxPower(uint32_t p) { maxPowers[powerType < 7 ? powerType : 0] = p; }
    void setMaxPowerByType(uint8_t type, uint32_t p) { if (type < 7) maxPowers[type] = p; }

    [[nodiscard]] uint32_t getPowerByType(uint8_t type) const { return type < 7 ? powers[type] : 0; }
    [[nodiscard]] uint32_t getMaxPowerByType(uint8_t type) const { return type < 7 ? maxPowers[type] : 0; }

    [[nodiscard]] uint8_t getPowerType() const { return powerType; }
    void setPowerType(uint8_t t) { powerType = t; }

    [[nodiscard]] uint32_t getAuraState() const { return auraState; }
    void setAuraState(uint32_t state) { auraState = state; }

    // Level
    [[nodiscard]] uint32_t getLevel() const { return level; }
    void setLevel(uint32_t l) { level = l; }

    // Entry ID (creature template entry)
    [[nodiscard]] uint32_t getEntry() const { return entry; }
    void setEntry(uint32_t e) { entry = e; }

    // Display ID (model display)
    [[nodiscard]] uint32_t getDisplayId() const { return displayId; }
    void setDisplayId(uint32_t id) { displayId = id; }
    /// How far this unit's edge is from its centre, and how far past that it
    /// can reach. Range between two units is measured edge to edge, so both
    /// come off the centre distance - see UNIT_FIELD_COMBATREACH.
    [[nodiscard]] float getCombatReach() const { return combatReach; }
    void setCombatReach(float r) { combatReach = r; }
    [[nodiscard]] float getBoundingRadius() const { return boundingRadius; }
    void setBoundingRadius(float r) { boundingRadius = r; }

    // Mount display ID (UNIT_FIELD_MOUNTDISPLAYID, index 69)
    [[nodiscard]] uint32_t getMountDisplayId() const { return mountDisplayId; }
    void setMountDisplayId(uint32_t id) { mountDisplayId = id; }

    // Unit flags (UNIT_FIELD_FLAGS, index 59)
    [[nodiscard]] uint32_t getUnitFlags() const { return unitFlags; }
    void setUnitFlags(uint32_t f) { unitFlags = f; }

    // Client presentation flags (byte 2 of UNIT_FIELD_BYTES_1).
    [[nodiscard]] uint8_t getVisibilityFlags() const { return visibilityFlags; }
    void setVisibilityFlags(uint8_t f) { visibilityFlags = f; }

    // Stand state (byte 0 of UNIT_FIELD_BYTES_1): 0 stand, 1 sit, 2-6 chairs,
    // 3 sleep, 7 dead, 8 kneel, 9 submerged. Other clients learn of a sit only
    // through this field; SMSG_STANDSTATE_UPDATE goes to the sitter alone.
    [[nodiscard]] uint8_t getStandState() const { return standState; }
    void setStandState(uint8_t s) { standState = s; }
    [[nodiscard]] bool hasCreepVisibility() const { return (visibilityFlags & UNIT_VIS_FLAG_CREEP) != 0; }
    void clearCreepVisibility() {
        visibilityFlags &= static_cast<uint8_t>(~UNIT_VIS_FLAG_CREEP);
    }

    // Dynamic flags (UNIT_DYNAMIC_FLAGS, index 147)
    [[nodiscard]] uint32_t getDynamicFlags() const { return dynamicFlags; }
    void setDynamicFlags(uint32_t f) { dynamicFlags = f; }

    // NPC flags (UNIT_NPC_FLAGS, index 82)
    [[nodiscard]] uint32_t getNpcFlags() const { return npcFlags; }
    void setNpcFlags(uint32_t f) { npcFlags = f; }

    // NPC emote state (UNIT_NPC_EMOTESTATE) - persistent looping animation for NPCs
    [[nodiscard]] uint32_t getNpcEmoteState() const { return npcEmoteState; }
    void setNpcEmoteState(uint32_t e) { npcEmoteState = e; }

    // Returns true if NPC has interaction flags (gossip/vendor/quest/trainer)
    [[nodiscard]] bool isInteractable() const { return npcFlags != 0; }

    // Faction-based hostility
    [[nodiscard]] uint32_t getFactionTemplate() const { return factionTemplate; }
    void setFactionTemplate(uint32_t f) { factionTemplate = f; }
    [[nodiscard]] bool isHostile() const { return hostile; }
    void setHostile(bool h) { hostile = h; }

protected:
    std::string name;
    uint32_t health = 0;
    uint32_t maxHealth = 0;
    uint32_t powers[7] = {};     // Indexed by power type (0=mana,1=rage,2=focus,3=energy,4=happiness,5=runes,6=runic)
    uint32_t maxPowers[7] = {};  // Max values per power type
    uint8_t powerType = 0;       // Active power type
    uint32_t auraState = 0;      // UNIT_FIELD_AURASTATE reactive opportunity mask
    uint32_t level = 1;
    uint32_t entry = 0;
    uint32_t displayId = 0;
    // Zero until the server says otherwise, which is the right default: it
    // makes an edge-to-edge distance fall back to centre to centre rather
    // than inventing reach for a unit whose size has not arrived.
    float combatReach = 0.0f;
    float boundingRadius = 0.0f;
    uint32_t mountDisplayId = 0;
    uint32_t unitFlags = 0;
    uint8_t visibilityFlags = 0;
    uint8_t standState = 0;
    uint32_t dynamicFlags = 0;
    uint32_t npcFlags = 0;
    uint32_t npcEmoteState = 0;
    uint32_t factionTemplate = 0;
    bool hostile = false;
};

/**
 * Player entity
 * Name is inherited from Unit - do NOT redeclare it here or the
 * shadowed field will diverge from Unit::name, causing nameplates
 * and other Unit*-based lookups to read an empty string.
 */
class Player : public Unit {
public:
    Player() { type = ObjectType::PLAYER; }
    explicit Player(uint64_t guid) : Unit(guid) { type = ObjectType::PLAYER; }
};

/**
 * GameObject entity (doors, chests, etc.)
 */
class GameObject : public Entity {
public:
    GameObject() { type = ObjectType::GAMEOBJECT; }
    explicit GameObject(uint64_t guid) : Entity(guid) { type = ObjectType::GAMEOBJECT; }

    [[nodiscard]] const std::string& getName() const { return name; }
    void setName(const std::string& n) { name = n; }

    [[nodiscard]] uint32_t getEntry() const { return entry; }
    void setEntry(uint32_t e) { entry = e; }

    [[nodiscard]] uint32_t getDisplayId() const { return displayId; }
    void setDisplayId(uint32_t id) { displayId = id; }

protected:
    std::string name;
    uint32_t entry = 0;
    uint32_t displayId = 0;
};

/// What to call this entity on screen, whatever kind it turns out to be.
///
/// The name lives on Unit and on GameObject and not on Entity, so anything
/// wanting to write one down has to ask which kind it is holding first. Five
/// places did that for themselves - the four files GameScreen was split into
/// and the chat commands - in identical copies, and two of the four never
/// called their own.
///
/// "Unknown" rather than an empty string, because every caller is putting this
/// in a sentence: an entity whose name has not arrived yet reads as a gap in
/// the line otherwise, with nothing to say a name was expected.
inline std::string entityDisplayName(const std::shared_ptr<Entity>& entity) {
    if (!entity) return "Unknown";
    if (entity->getType() == ObjectType::PLAYER) {
        auto player = std::static_pointer_cast<Player>(entity);
        if (!player->getName().empty()) return player->getName();
    } else if (entity->getType() == ObjectType::UNIT) {
        auto unit = std::static_pointer_cast<Unit>(entity);
        if (!unit->getName().empty()) return unit->getName();
    } else if (entity->getType() == ObjectType::GAMEOBJECT) {
        auto go = std::static_pointer_cast<GameObject>(entity);
        if (!go->getName().empty()) return go->getName();
    }
    return "Unknown";
}

/**
 * Entity manager for tracking all entities in view
 */
class EntityManager {
public:
    // Add entity
    void addEntity(uint64_t guid, std::shared_ptr<Entity> entity);

    // Remove entity
    void removeEntity(uint64_t guid);

    // Get entity
    std::shared_ptr<Entity> getEntity(uint64_t guid) const;

    // Check if entity exists
    bool hasEntity(uint64_t guid) const;

    // Main-thread spatial query. The cell index is refreshed at most four times
    // per second, avoiding a full entity-map distance scan every rendered frame.
    std::vector<std::shared_ptr<Entity>> getEntitiesNear(float x, float y, float radius) const;
    void getEntitiesNear(float x, float y, float radius,
                         std::vector<std::shared_ptr<Entity>>& out) const;

    // Get all entities. MAIN-THREAD-ONLY: mutations happen via dispatchQueuedPackets()
    // on the main thread, and this reference is not lock-protected. Callers on any
    // other thread (e.g. the headless HTTP API thread) must use snapshotEntities()
    // instead, which is safe to call from anywhere.
    const std::unordered_map<uint64_t, std::shared_ptr<Entity>>& getEntities() const {
        return entities;
    }

    // Thread-safe copy of all tracked entities, safe to call from any thread.
    std::vector<std::shared_ptr<Entity>> snapshotEntities() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::shared_ptr<Entity>> snapshot;
        snapshot.reserve(entities.size());
        for (const auto& [guid, entity] : entities) {
            snapshot.push_back(entity);
        }
        return snapshot;
    }

    // Clear all entities
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entities.clear();
        spatialCells_.clear();
        spatialDirty_ = true;
    }

    // Get entity count
    size_t getEntityCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entities.size();
    }

private:
    // MAIN-THREAD-ONLY for getEntities(): all entity map mutations happen via
    // dispatchQueuedPackets() on the main thread. mutex_ guards addEntity/removeEntity/
    // getEntity/hasEntity/snapshotEntities so those are safe to call cross-thread (the
    // headless HTTP API thread reads player HP and nearby entities this way); it is not
    // taken by the unlocked getEntities() reference accessor above, which remains
    // main-thread-only.
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<Entity>> entities;
    static constexpr float kSpatialCellSize = 64.0f;
    mutable std::unordered_map<int64_t, std::vector<std::shared_ptr<Entity>>> spatialCells_;
    mutable std::chrono::steady_clock::time_point lastSpatialRebuild_{};
    mutable bool spatialDirty_ = true;
};

/// Who a unit has selected, from the two halves the wire splits the guid into.
///
/// UNIT_FIELD_TARGET arrives as a low and a high update field, and reading it
/// was written out separately in six files - the combat handler, two unit
/// APIs, the target frames, the nameplates and a slash command. Six copies of
/// a two-field read is how one of them comes to be missing its high half, and
/// a guid missing its high half matches nothing on a server that uses one.
inline uint64_t unitTargetGuid(const Entity& entity) {
    const auto& fields = entity.getFields();
    auto lo = fields.find(fieldIndex(UF::UNIT_FIELD_TARGET_LO));
    if (lo == fields.end()) return 0;
    uint64_t guid = lo->second;
    auto hi = fields.find(fieldIndex(UF::UNIT_FIELD_TARGET_HI));
    if (hi != fields.end()) guid |= (static_cast<uint64_t>(hi->second) << 32);
    return guid;
}

inline uint64_t unitTargetGuid(const std::shared_ptr<Entity>& entity) {
    return entity ? unitTargetGuid(*entity) : 0;
}

/// Who summoned this unit, or 0 for a unit nobody did.
///
/// Answers 0 on an expansion whose table has no entry for the field, which is
/// the same answer as "nobody summoned it" on purpose: the callers use it to
/// decide whether a unit is somebody's pet, and not knowing has to read as no.
inline uint64_t unitSummonedByGuid(const Entity& entity) {
    const auto& fields = entity.getFields();
    auto lo = fields.find(fieldIndex(UF::UNIT_FIELD_SUMMONEDBY_LO));
    if (lo == fields.end()) return 0;
    uint64_t guid = lo->second;
    auto hi = fields.find(fieldIndex(UF::UNIT_FIELD_SUMMONEDBY_HI));
    if (hi != fields.end()) guid |= (static_cast<uint64_t>(hi->second) << 32);
    return guid;
}

} // namespace game
} // namespace wowee
