#pragma once

#include "game/protocol_constants.hpp"
#include "game/update_field_table.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <functional>
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
#include "math/chase_spring.hpp"
#include "game/flat_field_map.hpp"
#include "game/monster_move_flags.hpp"
#include "game/power_prediction.hpp"
#include <glm/gtc/quaternion.hpp>

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
// 0x40 is the bit the client asks for when it wants to know whether that unit
// is recruit-a-friend linked: it reads no other user of it, and nothing else
// asks for it (0x006d7590, behind IsReferAFriendLinked). Named for that one
// read; whether a realm sets it is the realm's own decision.
inline constexpr uint32_t UNIT_DYNFLAG_REFER_A_FRIEND   = 0x00000040;

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
    float pitchRate = 3.14159265f;  ///< CMovement +0xb0, what a pitch over time runs at
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
/// -> FUN_00988370). `orientation` is the wire's. The direction is the way the keys move the unit, not
/// only the facing: backing up turns it by pi, a strafe by pi/2 to the left,
/// diagonals half way (FUN_00988df0). The speed is the unit's current one
/// (FUN_00987570): walk when walking, run-back when only backing up, else
/// run; and none when no key is moving it, so the jump goes straight up.
inline OutgoingJump outgoingJumpBlock(uint32_t flags, float orientation, float walkSpeed,
                                      float runSpeed, float runBackSpeed) {
    constexpr uint32_t kForward = 0x1, kBackward = 0x2, kStrafeLeft = 0x4,
                       kStrafeRight = 0x8, kWalking = 0x100, kSwimming = 0x200000;
    OutgoingJump j;
    // From the water a jump goes up faster, -9.096748 (0xc1118c48; the
    // swimming branch of FUN_009883f0).
    j.verticalSpeed = (flags & kSwimming) ? -9.096748f : -7.955547f;
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

/// Whether a landing goes out as MSG_MOVE_FALL_LAND: only one that ends a
/// fall. The client's land (0x00988490) does nothing for a mover without
/// FALLING (0x1000), and its caller sends the landing (0x006eba60 ->
/// 0x0073d3d0) only when the step took FALLING off. A fall is started by a
/// jump or a walk off a ledge (0x00988370) or by the fall step itself
/// (0x00987410); a mover set down on a lift's car by a teleport, or put on
/// a transport, has started none, and lands on nothing.
inline bool landingSent(uint32_t flagsBefore) {
    constexpr uint32_t kFalling = 0x1000;
    return (flagsBefore & kFalling) != 0;
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
    /// `smooth`: a curve through the points (Catmull-Rom), as the client runs a
    /// spline flagged FLYING or CATMULLROM (0x42000); otherwise straight lines
    /// from point to point at the spline's one speed - FUN_0098c770 sets the
    /// spline's mode (+500) to 1 only for those flags, 0 (linear) for the rest.
    /// `clientCurve`: the spline as the WotLK client builds it - a curve by
    /// startCurvedPath, a straight one from the packet's start.
    void startMoveAlongPath(const std::vector<std::array<float, 3>>& packetPath, float destO, float totalDuration,
                            bool smooth = true, bool clientCurve = false, bool cyclic = false,
                            bool entersCycle = false) {
        if (packetPath.empty()) return;
        if (packetPath.size() == 1 || totalDuration <= 0.0f) {
            startMoveTo(packetPath.back()[0], packetPath.back()[1], packetPath.back()[2], destO, totalDuration);
            return;
        }
        resetMotionExtras();
        if (smooth && clientCurve) {
            startCurvedPath(packetPath, destO, totalDuration, cyclic, entersCycle);
            return;
        }
        curvePoints_.clear();
        curveLengths_.clear();
        // A straight spline as the WotLK client builds it (clientCurve):
        // from the packet's start - where the server has the unit - through
        // its points to its end. FUN_0073c8e0's branch for a spline without
        // 0x42000 puts the packet's start first (with a pad before it that a
        // straight spline never reaches) and asks nothing of where the unit
        // is drawn; only the curved branch starts at the unit (vfunc +0x30).
        // Its speed is the points' length over the packet's duration, at
        // most four times the unit's run speed and never under 28 yd/s.
        // Otherwise (the older expansions) the spline starts where the unit
        // is drawn and keeps the packet's start after it when the two are
        // more than 0.00077160494 apart squared.
        std::vector<std::array<float, 3>> path;
        path.reserve(packetPath.size() + 1);
        if (!smooth && clientCurve) {
            path = packetPath;
            x = path[0][0]; y = path[0][1]; z = path[0][2];
            float length = 0.0f;
            for (size_t i = 1; i < path.size(); ++i) {
                const float dx = path[i][0] - path[i - 1][0];
                const float dy = path[i][1] - path[i - 1][1];
                const float dz = path[i][2] - path[i - 1][2];
                length += std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            const float cap = std::max(speeds_.run * 4.0f, 28.0f);
            float speed = cap;
            if (totalDuration > 0.0f) speed = std::min(speed, length / totalDuration);
            if (length > 0.16666667f && speed > 9.536743e-07f) {
                totalDuration = static_cast<float>(std::max(1L, std::lround(length / speed * 1000.0f))) / 1000.0f;
            }
        } else {
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
        if (smooth) {
            linearKeys_.clear();
        } else {
            linearKeys_ = keys;
        }
        activeSpline_.emplace(std::move(keys), /*timeClosed=*/false);
        splineDurationMs_ = durationMs;

        moveEndX_ = path.back()[0]; moveEndY_ = path.back()[1]; moveEndZ_ = path.back()[2];
        moveDuration_ = totalDuration;
        moveElapsed_ = 0.0f;
        rawDurationMs_ = nextDurationMs_ = durationMs;
        splineCyclic_ = cyclic && clientCurve;
        // The spline's length over its duration: what the client's
        // CMovement::GetCurrentSpeed (FUN_00987570) answers for a unit on one.
        moveSpeed_ = totalDist / totalDuration;
        orientation = destO;
        isMoving_ = true;
        usePathMode_ = true;
    }

    /// A spline the unit was already on as its create or movement block came
    /// (0x004f4b50 reads it into the block, 0x006f1520 -> 0x006f1240 makes it
    /// the unit's). Nothing is worked out from it as a monster move's is: its
    /// duration is the block's and its points are the client's whole array,
    /// of which the first and the last are pads whichever kind it is - both
    /// the curve and the straight line run from point i + 1 to i + 2
    /// (0x004c3fd0) - so a straight one the server writes without a pad
    /// before it is run from its second point, and one of three points stands
    /// at its end. Straight or curved goes by the flags (0x006f1520 sets +500
    /// by 0x42000), the segments' lengths by the block's mode (0x004f4ae0).
    struct BlockSpline {
        std::vector<glm::vec3> points;    // canonical, or the transport's axes (framed)
        glm::vec3 endPoint{0.0f};         // +0x1f8
        bool curved = false;
        bool curvedLengths = false;
        bool cyclic = false;
        bool enterCycle = false;
        bool over = false;                // run out already, whatever its time
        bool framed = false;              // on a transport: placed by setMoveFrame
        uint32_t durationMs = 0;          // +0x2c
        float durationFactor = 1.0f;      // +0x204, what it is multiplied by
        float nextDurationFactor = 1.0f;  // +0x208, the next round's
        uint32_t passedMs = 0;
        // Parabolic: the arc's acceleration and when it starts (+0x20c, +0x210).
        bool arcs = false;
        float arcAcceleration = 0.0f;
        uint32_t arcStartMs = 0;
        // Falling: the height the fall began at.
        std::optional<float> fallTop;
    };
    void startBlockSpline(const BlockSpline& s) {
        resetMotionExtras();
        usePathMode_ = false;
        activeSpline_.reset();
        linearKeys_.clear();
        const size_t n = s.points.size();
        splineFramed_ = s.framed;
        splineCyclic_ = s.cyclic;
        splineEntersCycle_ = s.enterCycle;
        rawDurationMs_ = s.durationMs;
        nextDurationMs_ = scaledDuration(s.durationMs, s.enterCycle ? 1.0f : s.nextDurationFactor);
        splineDurationMs_ = scaledDuration(s.durationMs, s.durationFactor);
        // Where it ends is its last point, or the block's end point where that
        // is three yards off and more (0x006e9470).
        pathEnd_ = s.endPoint;
        if (n >= 3) {
            const glm::vec3 last = s.points[n == 3 ? 2 : n - 2];
            if (s.cyclic || glm::dot(last - pathEnd_, last - pathEnd_) < 9.0f) pathEnd_ = last;
        }
        float length = 0.0f;
        if (n >= 3) {
            curvePoints_ = s.points;
            curveLinear_ = !s.curved;
            length = measureCurve(s.curvedLengths);
            keepCurveKeys(length);
            usePathMode_ = true;
        } else {
            curvePoints_.clear();
            curveLengths_.clear();
        }
        // Arcs by the spline's time against its unmultiplied duration (0x00987d20).
        if (s.arcs) setSplineArc(s.arcAcceleration, s.arcStartMs, s.durationMs);
        fallTop_ = s.fallTop;
        moveStartX_ = x; moveStartY_ = y; moveStartZ_ = z;
        if (s.framed) { moveEndX_ = x; moveEndY_ = y; moveEndZ_ = z; }
        else { moveEndX_ = pathEnd_.x; moveEndY_ = pathEnd_.y; moveEndZ_ = pathEnd_.z; }
        moveDuration_ = static_cast<float>(splineDurationMs_) / 1000.0f;
        moveElapsed_ = static_cast<float>(s.passedMs) / 1000.0f;
        if (s.over || n < 3) moveElapsed_ = std::max(moveElapsed_, moveDuration_);
        // 0x00987570: its length over its duration.
        moveSpeed_ = s.durationMs > 0 ? length / (static_cast<float>(s.durationMs) / 1000.0f) : 0.0f;
        isMoving_ = true;
    }

    /// The transport a spline on one is run on, as its axes lie in the world
    /// now: the spline's place is the one on the transport, put through it
    /// each frame (0x004f4460). Until it is given the unit stays where it is.
    void setMoveFrame(const glm::mat4& frame) {
        if (!isMoving_ || !splineFramed_) return;
        moveFrame_ = frame;
        const glm::vec3 end(frame * glm::vec4(pathEnd_, 1.0f));
        moveEndX_ = end.x; moveEndY_ = end.y; moveEndZ_ = end.z;
        if (moveElapsed_ >= moveDuration_) { x = end.x; y = end.y; z = end.z; }
        else if (usePathMode_) placeOnPath();
    }
    [[nodiscard]] bool onFramedPath() const { return isMoving_ && splineFramed_; }
    /// Where it ends, on its transport.
    [[nodiscard]] glm::vec3 framedEnd() const { return pathEnd_; }

    /// 0x0098ca00: a spline's duration, rounded after its factor.
    [[nodiscard]] static uint32_t scaledDuration(uint32_t ms, float factor) {
        const float scaled = static_cast<float>(ms) * factor + 0.5f;
        return std::isfinite(scaled) && scaled > 0.0f && scaled < 4.0e9f ? static_cast<uint32_t>(scaled) : 0u;
    }

    /// A parabolic spline's arc (0x006e96c0): from `startMs` on the unit is
    /// over the path by what 0x00987d20 gives for the time, coming down on it
    /// as the spline ends. Call after the move is started.
    void setSplineArc(float acceleration, uint32_t startMs, uint32_t durationMs) {
        arcAcceleration_ = acceleration;
        arcStartMs_ = startMs;
        arcDurationMs_ = durationMs;
    }

    /// Whom the move ends facing, asked where it ends and not before
    /// (0x006eb0b0 looks the target up then). Call after faceAlongMove.
    using FacingFrom = std::function<std::optional<float>(const glm::vec3& at)>;
    void faceOnArrival(FacingFrom ask) {
        if (isMoving_ && !movingByFlags_) arrivalFacing_ = std::move(ask);
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
        movingByFlags_ = false;
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
        splineFlagsArrive_ = false;
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
    /// The flags of a create or movement block's movement section, which
    /// the client takes as they are.
    void setBlockMoveFlags(uint32_t flags) {
        reportedMoveFlags_ = flags;
        splineFlagsArrive_ = false;
    }
    /// The flags a monster move leaves (monster_move::afterMonsterMove),
    /// given after its move has been started here. `arrives` - a spline that
    /// is not cyclic - takes forward and back off again once the move is
    /// over (0x006eb0b0), at once if it never got under way.
    void setMonsterMoveFlags(uint32_t flags, bool arrives) {
        reportedMoveFlags_ = flags;
        splineFlagsArrive_ = arrives;
        arriveSplineFlags();
    }

    /// CMovement::GetCurrentSpeed (0x00987570) on the flags last received:
    /// nothing without a direction flag (0xc0000f); on a spline that
    /// spline's speed; else the flags' speed (speedForFlags).
    [[nodiscard]] float currentSpeed() const {
        if ((reportedMoveFlags_ & kDirectionFlags) == 0) return 0.0f;
        if (isActivelyMoving() && !movingByFlags_) return moveSpeed_;
        return speedForFlags(speeds_, reportedMoveFlags_);
    }

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

    /// Its base animation is to be asked again, once what it reads has been
    /// brought up to date (0x0073ac30 of a unit spoken to).
    void requestBaseAnimation() { baseAnimationAsked_ = true; }
    [[nodiscard]] bool takeBaseAnimationRequest() { return std::exchange(baseAnimationAsked_, false); }

    /// Moved down by gravity from a packet's fall (startMoveByFlags), rather
    /// than along the ground.
    [[nodiscard]] bool isFallingByFlags() const { return movingByFlags_ && falling_; }

    /// Moved on by its movement flags (startMoveByFlags), as the client moves
    /// another player between that player's packets - not along a spline and
    /// not towards a packet's position.
    [[nodiscard]] bool isMovingByFlags() const { return isMoving_ && movingByFlags_; }

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
            const math::SplineEvalResult start =
                pathEval(std::min(static_cast<uint32_t>(moveElapsed_ * 1000.0f), splineDurationMs_));
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
    /// The transport under it turned by `radians`: the drawn facing turns
    /// with the deck rather than easing after it (updateModelFacing eases
    /// only the unit's own turns), so a passenger standing still on a
    /// zeppelin in a curve does not turn on the spot.
    void carryModelFacing(float radians) {
        if (modelFacingSet_) modelFacing_ = wrapAngle(modelFacing_ + radians);
    }

    /// The speed of the current movement, in yards a second, while the
    /// entity is actively moving; zero otherwise. Set from each segment's
    /// length over its duration, or by setMoveSpeed when the caller knows it
    /// better.
    [[nodiscard]] float getMoveSpeed() const { return isActivelyMoving() ? moveSpeed_ : 0.0f; }
    /// Walk rather than Run at the pace it is moving (FUN_00717050): Run above
    /// twice its walk speed, Walk at or below it. No flag enters into it - the
    /// client never copies a spline's walk mode into the unit's flags.
    [[nodiscard]] bool movesAtWalkPace() const { return getMoveSpeed() <= 2.0f * speeds_.walk; }
    void setMoveSpeed(float yardsPerSecond) {
        if (std::isfinite(yardsPerSecond) && yardsPerSecond >= 0.0f) moveSpeed_ = yardsPerSecond;
    }

    /// The current move's direction times its speed - CMovement's +0x64
    /// direction and +0x8c speed (0x006fed70) - on the game's axes; nothing
    /// while it is not moving.
    [[nodiscard]] glm::vec3 getMoveVelocity() const {
        if (!isActivelyMoving()) return glm::vec3(0.0f);
        if (movingByFlags_) {
            // The heading turns with the facing (advanceByFlags).
            const float t = std::min(moveElapsed_, kFlagDeadReckonLimitSec);
            const float heading = flagHeading_ + flagArcRate_ * t;
            return {flagSpeed_ * std::cos(heading), -flagSpeed_ * std::sin(heading), flagClimb_};
        }
        glm::vec3 way(moveEndX_ - moveStartX_, moveEndY_ - moveStartY_, moveEndZ_ - moveStartZ_);
        if (usePathMode_ && activeSpline_ && splineDurationMs_ > 0) {
            uint32_t ms = static_cast<uint32_t>(moveElapsed_ * 1000.0f);
            if (ms >= splineDurationMs_) ms = splineDurationMs_ - 1;
            way = pathEval(ms).tangent;
        }
        const float length = std::sqrt(way.x * way.x + way.y * way.y + way.z * way.z);
        if (length < 1e-6f) return glm::vec3(0.0f);
        return way * (moveSpeed_ / length);
    }

    /// How long the path under way takes, as the client's own spline does
    /// (0x0098c770 takes the length over the capped speed, not the packet's
    /// duration); none where no path runs.
    [[nodiscard]] std::optional<uint32_t> pathDurationMs() const {
        if (!isMoving_ || !usePathMode_ || !activeSpline_) return std::nullopt;
        return splineDurationMs_;
    }

    /// The path's direction now and to the place it has the unit a second on
    /// (0x0098ca00, which banks a unit on a flying spline by the two); none
    /// where no path runs.
    struct PathHeading {
        glm::vec3 tangent;
        glm::vec3 ahead;
    };
    [[nodiscard]] std::optional<PathHeading> pathHeading() const {
        if (!isMoving_ || !usePathMode_ || !activeSpline_ || splineDurationMs_ == 0 || moveElapsed_ >= moveDuration_)
            return std::nullopt;
        const uint32_t now = std::min(static_cast<uint32_t>(moveElapsed_ * 1000.0f), splineDurationMs_ - 1);
        const math::SplineEvalResult at = pathEval(now);
        const math::SplineEvalResult next =
            pathEval(splineCyclic_ ? (now + 1000u) % splineDurationMs_ : std::min(now + 1000u, splineDurationMs_));
        return PathHeading{.tangent = at.tangent, .ahead = next.position - at.position};
    }

    void updateMovement(float deltaTime) {
        // What of this step lies before the move under way began.
        const float before = std::clamp(deltaTime, 0.0f, startsIn_);
        startsIn_ -= before;
        const float gone = deltaTime - before;
        if (turnRate_ != 0.0f) {
            turnElapsed_ += gone;
            orientation = wrapAngle(turnBase_ + turnRate_ * turnElapsed_);
        }
        if (isMoving_) {
            if (movingByFlags_) advanceByFlags(gone);
            else advanceSegment(gone);
        }
        arriveSplineFlags();
        updateModelFacing(deltaTime);
    }

    /// The move just started began this long after the moment the unit was
    /// last moved on to (GameHandler::sinceUnitsAdvanced): so much of the next
    /// step is not the move's. The client's splines go by the clock - the time
    /// now less the one the spline was started at (0x0098ca00: +0x28 = now -
    /// +0x24, which 0x0098c770 is given the frame's time for) - so a spline
    /// that comes after three seconds of loading starts at its start.
    void moveBeganAfter(float seconds) { startsIn_ = std::max(seconds, 0.0f); }

    /// The rate its model chases its facing at, in the client's critically
    /// damped spring (0x007160b0), or 0 for the four-sample halving: a
    /// creature's CreatureMovementInfo SmoothFacingChaseRate, or 20 (30 in a
    /// fight) for a player-controlled unit that is not a player (0x00735f60).
    void setFacingChaseRate(float rate) { facingChaseRate_ = rate; }

    [[nodiscard]] bool isEntityMoving() const { return isMoving_; }

    /// Puts a single-segment move's two ends somewhere else, keeping how far
    /// along it is: a move on a transport is run in the transport's frame,
    /// so its ends in the world go where the transport takes them.
    void setMoveEnds(const glm::vec3& start, const glm::vec3& end) {
        if (!isMoving_ || movingByFlags_ || usePathMode_) return;
        moveStartX_ = start.x; moveStartY_ = start.y; moveStartZ_ = start.z;
        moveEndX_ = end.x; moveEndY_ = end.y; moveEndZ_ = end.z;
        if (moveDuration_ > 0.0f && moveElapsed_ < moveDuration_) {
            const float t = moveElapsed_ / moveDuration_;
            x = start.x + (end.x - start.x) * t;
            y = start.y + (end.y - start.y) * t;
            z = start.z + (end.z - start.z) * t;
        } else {
            x = end.x; y = end.y; z = end.z;
        }
    }

    /// On its way, along a spline or moved on by its flags.
    [[nodiscard]] bool isActivelyMoving() const {
        return isMoving_ && (moveElapsed_ < moveDuration_ || movingByFlags_);
    }

    // Where the move under way ends; where it stands if there is none. Not
    // where the unit is: a path may end hundreds of yards from there, and
    // whoever asks how far a unit is, or draws it, asks getX/Y/Z, which is
    // moved on for every unit each frame.
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
    float startsIn_ = 0;                    // moveBeganAfter
    float moveSpeed_ = 0;                   // Current movement's speed; see getMoveSpeed
    // CatmullRom spline for multi-segment path movement (replaces linear pathPoints_/pathSegDists_)
    std::optional<math::CatmullRomSpline> activeSpline_;
    // A linear spline's points and times (startMoveAlongPath); empty for a curved one.
    std::vector<math::SplineKey> linearKeys_;
    // A curved spline's control points, a pad at each end (startCurvedPath),
    // and the running curve length at the end of each segment; else empty.
    std::vector<glm::vec3> curvePoints_;
    std::vector<float> curveLengths_;
    bool curveLinear_ = false;              // curvePoints_ run straight (startBlockSpline)
    uint32_t splineDurationMs_ = 0;
    // A cyclic spline's: its duration without a factor, and its next round's.
    uint32_t rawDurationMs_ = 0;
    uint32_t nextDurationMs_ = 0;
    bool splineCyclic_ = false;
    bool splineEntersCycle_ = false;
    // A spline on a transport (setMoveFrame); pathEnd_ is its end there.
    bool splineFramed_ = false;
    std::optional<glm::mat4> moveFrame_;
    glm::vec3 pathEnd_{0.0f};
    // setSplineArc; none with a duration of 0.
    float arcAcceleration_ = 0.0f;
    uint32_t arcStartMs_ = 0;
    uint32_t arcDurationMs_ = 0;
    std::optional<float> fallTop_;

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

    /// 0x006eb0b0 -> 0x0098bd10: the spline over, forward and back (and a
    /// pending stop) go.
    void arriveSplineFlags() {
        if (!splineFlagsArrive_ || isActivelyMoving()) return;
        reportedMoveFlags_ = monster_move::arrived(reportedMoveFlags_);
        splineFlagsArrive_ = false;
    }

    void resetMotionExtras() {
        movingByFlags_ = false;
        falling_ = false;
        turnRate_ = 0.0f;
        faceAlongPath_ = false;
        finalFacing_.reset();
        arrivalFacing_ = nullptr;
        splineCyclic_ = false;
        splineEntersCycle_ = false;
        splineFramed_ = false;
        moveFrame_.reset();
        curveLinear_ = false;
        arcDurationMs_ = 0;
        fallTop_.reset();
    }

    /// curveLengths_ for curvePoints_: each segment's - straight from point to
    /// point, or its curve's over 20 steps (0x004c40b0, 0x004c3b10).
    float measureCurve(bool curved) {
        curveLengths_.clear();
        float run = 0.0f;
        if (curvePoints_.size() == 3) curveLengths_.push_back(0.0f);
        for (size_t seg = 0; seg + 3 < curvePoints_.size(); ++seg) {
            glm::vec3 prev = curvePoints_[seg + 1];
            if (!curved) {
                run += glm::length(curvePoints_[seg + 2] - prev);
            } else {
                for (int k = 1; k <= 20; ++k) {
                    const glm::vec3 at = curveAt(seg, 0.05f * static_cast<float>(k)).position;
                    run += glm::length(at - prev);
                    prev = at;
                }
            }
            curveLengths_.push_back(run);
        }
        return run;
    }

    /// The curve's points as the spline's keys too, for what reads them.
    void keepCurveKeys(float length) {
        if (length <= 1e-6f) length = 1.0f;
        std::vector<math::SplineKey> keys;
        for (size_t i = 1; i + 1 < curvePoints_.size(); ++i) {
            math::SplineKey k;
            k.timeMs = i == 1 ? 0u
                              : static_cast<uint32_t>(curveLengths_[i - 2] / length *
                                                      static_cast<float>(splineDurationMs_));
            k.position = curvePoints_[i];
            keys.push_back(k);
        }
        activeSpline_.emplace(std::move(keys), /*timeClosed=*/false);
    }

    /// A cyclic spline that has run out begins again with what is over
    /// (0x0098ca00); one entering its cycle leaves its first point out from
    /// then on (0x0098c940).
    void wrapCycle() {
        moveElapsed_ -= moveDuration_;
        if (splineEntersCycle_ && curvePoints_.size() > 4) {
            curvePoints_.erase(curvePoints_.begin());
            curvePoints_[0] = curvePoints_[curvePoints_.size() - 3];
            const float length = measureCurve(!curveLinear_);
            keepCurveKeys(length);
            if (rawDurationMs_ > 0) moveSpeed_ = length / (static_cast<float>(rawDurationMs_) / 1000.0f);
        }
        splineEntersCycle_ = false;
        splineDurationMs_ = nextDurationMs_;
        nextDurationMs_ = rawDurationMs_;
        moveDuration_ = static_cast<float>(splineDurationMs_) / 1000.0f;
        if (moveDuration_ > 0.0f && moveElapsed_ >= moveDuration_) moveElapsed_ = std::fmod(moveElapsed_, moveDuration_);
    }

    /// The path's place now, and the facing along it.
    void placeOnPath() {
        if (splineFramed_ && !moveFrame_) return;
        uint32_t pathTimeMs = static_cast<uint32_t>(moveElapsed_ * 1000.0f);
        if (pathTimeMs >= splineDurationMs_) pathTimeMs = splineDurationMs_ - 1;
        const math::SplineEvalResult eval = pathEval(pathTimeMs);
        x = eval.position.x;
        y = eval.position.y;
        z = eval.position.z;
        if (faceAlongPath_) faceDirection(eval.tangent.x, eval.tangent.y);
    }

    void faceDirection(float dx, float dy) {
        constexpr float kMinTangentSq = 0.0018490001f;  // FUN_0098ca00
        if (dx * dx + dy * dy > kMinTangentSq) {
            orientation = wrapAngle(std::atan2(-dy, dx) + pathFacingOffset_);
        }
    }

    /// The path at `ms`: the curve, or straight between its points when the
    /// spline is linear (startMoveAlongPath).
    [[nodiscard]] math::SplineEvalResult pathEval(uint32_t ms) const {
        math::SplineEvalResult r = flatPathEval(ms);
        // 0x00987d20, up while the spline still runs.
        if (arcDurationMs_ > 0 && ms > arcStartMs_ && ms < arcDurationMs_) {
            const float t = static_cast<float>(ms - arcStartMs_) * 0.001f;
            const float whole = static_cast<float>(arcDurationMs_ - arcStartMs_) * 0.001f;
            r.position.z += 0.5f * arcAcceleration_ * (whole - t) * t;
        }
        if (fallTop_)
            r.position.z = std::max(pathEnd_.z, *fallTop_ - fallDistance(static_cast<float>(ms) * 0.001f, 0.0f, false));
        if (moveFrame_) {
            r.position = glm::vec3(*moveFrame_ * glm::vec4(r.position, 1.0f));
            r.tangent = glm::mat3(*moveFrame_) * r.tangent;
        }
        return r;
    }
    [[nodiscard]] math::SplineEvalResult flatPathEval(uint32_t ms) const {
        if (curvePoints_.size() >= 3 && !curveLengths_.empty() && splineDurationMs_ > 0) {
            // As the client's spline (FUN_004c3bd0): the share of the time
            // gone is the share of the curve's length; the segment is found by
            // the running lengths and its parameter by its own length.
            const float total = curveLengths_.back();
            const float share = std::clamp(static_cast<float>(ms) / static_cast<float>(splineDurationMs_), 0.0f, 1.0f);
            const float s = total * share;
            size_t seg = 0;
            while (seg + 1 < curveLengths_.size() && curveLengths_[seg] <= s) ++seg;
            const float before = seg > 0 ? curveLengths_[seg - 1] : 0.0f;
            const float len = curveLengths_[seg] - before;
            const float u = len > 1e-6f ? std::clamp((s - before) / len, 0.0f, 1.0f) : 1.0f;
            if (curveLinear_ || curvePoints_.size() == 3) {
                math::SplineEvalResult r;
                r.tangent = curvePoints_[seg + 2] - curvePoints_[seg + 1];
                r.position = curvePoints_[seg + 1] + r.tangent * (curvePoints_.size() == 3 ? share : u);
                return r;
            }
            return curveAt(seg, u);
        }
        if (linearKeys_.size() < 2) return activeSpline_->evaluate(ms);
        size_t i = 1;
        while (i + 1 < linearKeys_.size() && linearKeys_[i].timeMs <= ms) ++i;
        const math::SplineKey& a = linearKeys_[i - 1];
        const math::SplineKey& b = linearKeys_[i];
        const float span = static_cast<float>(b.timeMs - a.timeMs);
        const float t = span > 0.0f ? std::clamp((static_cast<float>(ms) - static_cast<float>(a.timeMs)) / span, 0.0f, 1.0f) : 1.0f;
        math::SplineEvalResult r;
        r.position = a.position + (b.position - a.position) * t;
        r.tangent = b.position - a.position;
        return r;
    }

    /// Catmull-Rom segment `seg` of curvePoints_ - from its point seg + 1 to
    /// seg + 2; the first and the last point are pads - at `u`.
    [[nodiscard]] math::SplineEvalResult curveAt(size_t seg, float u) const {
        const glm::vec3& p0 = curvePoints_[seg];
        const glm::vec3& p1 = curvePoints_[seg + 1];
        const glm::vec3& p2 = curvePoints_[seg + 2];
        const glm::vec3& p3 = curvePoints_[seg + 3];
        const float u2 = u * u;
        const float u3 = u2 * u;
        math::SplineEvalResult r;
        r.position = 0.5f * ((2.0f * p1) + (-p0 + p2) * u + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * u2 +
                             (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * u3);
        r.tangent = 0.5f * ((-p0 + p2) + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * 2.0f * u +
                            (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * 3.0f * u2);
        return r;
    }

    /// A FLYING or CATMULLROM monster move (0x42000), as the client's handler
    /// (FUN_0073c8e0) builds it: from where the unit is drawn (vfunc +0x30),
    /// on through the packet's points after its start - the first of them
    /// only when it lies more than 0.0278 yd from the unit - with a pad a yard
    /// behind the unit along its facing before it and the last point repeated
    /// after it. The packet's own start is not a point of the curve. Its speed
    /// is the points' polyline length over the packet's duration, at most
    /// 50 yd/s, and its duration that length at that speed. A segment's length
    /// is its curve's, summed over 20 steps (FUN_004c3b10).
    /// A cyclic one is closed instead with its points after the unit's own
    /// place, the first two of them again, and ends on the first (0x0073cecf).
    void startCurvedPath(const std::vector<std::array<float, 3>>& packetPath, float destO, float totalDuration,
                         bool cyclic, bool entersCycle) {
        constexpr float kSameStartDistanceSq = 0.00077160494f;
        constexpr float kFlyingSpeedCap = 50.0f;
        std::vector<glm::vec3> pts;
        pts.reserve(packetPath.size() + 2);
        const glm::vec3 here(x, y, z);
        // The facing's direction, as startMoveByFlags moves along it.
        pts.push_back(glm::vec3(here.x - std::cos(orientation), here.y + std::sin(orientation), here.z));
        pts.push_back(here);
        for (size_t i = 1; i < packetPath.size(); ++i) {
            const glm::vec3 p(packetPath[i][0], packetPath[i][1], packetPath[i][2]);
            if (i == 1) {
                const glm::vec3 d = p - here;
                if (glm::dot(d, d) < kSameStartDistanceSq) continue;
            }
            pts.push_back(p);
        }
        const bool closes = cyclic && pts.size() >= 4;
        const glm::vec3 dest = closes ? pts[2] : pts.back();
        pts.push_back(dest);
        if (closes) pts.push_back(glm::vec3(pts[3]));
        float chord = 0.0f;
        for (size_t i = 2; i + 1 < pts.size(); ++i) chord += glm::length(pts[i] - pts[i - 1]);
        if (pts.size() < 4 || chord <= 0.16666667f) {
            curvePoints_.clear();
            curveLengths_.clear();
            startMoveTo(dest.x, dest.y, dest.z, destO, totalDuration);
            return;
        }
        float speed = kFlyingSpeedCap;
        if (totalDuration > 0.0f) speed = std::min(speed, chord / totalDuration);
        const uint32_t durationMs =
            std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(chord / speed * 1000.0f)));
        const float duration = static_cast<float>(durationMs) / 1000.0f;

        curvePoints_ = std::move(pts);
        linearKeys_.clear();
        splineDurationMs_ = durationMs;
        keepCurveKeys(measureCurve(true));
        rawDurationMs_ = nextDurationMs_ = durationMs;
        splineCyclic_ = cyclic;
        splineEntersCycle_ = cyclic && entersCycle;

        moveEndX_ = dest.x; moveEndY_ = dest.y; moveEndZ_ = dest.z;
        moveDuration_ = duration;
        moveElapsed_ = 0.0f;
        moveSpeed_ = speed;
        orientation = destO;
        isMoving_ = true;
        usePathMode_ = true;
    }

    void advanceSegment(float deltaTime) {
        moveElapsed_ += deltaTime;
        if (splineCyclic_ && usePathMode_ && moveDuration_ > 0.0f && moveElapsed_ >= moveDuration_) wrapCycle();
        if (moveElapsed_ < moveDuration_) {
            if (usePathMode_ && activeSpline_) {
                placeOnPath();  // Along the path: curved or straight (pathEval)
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
        if (arrivalFacing_) {
            if (const auto facing = std::exchange(arrivalFacing_, nullptr)(glm::vec3(moveEndX_, moveEndY_, moveEndZ_)))
                orientation = *facing;
        }
        // And it stands there: a spline flown out is marked over (0x0098ca00's
        // 0x100, 0x006eae70's 0x400), puts its unit on its last point
        // (0x00987b50) and carries it no further. It ran on at its speed for
        // as long again - drawn at the end all the same, but told of, aimed at
        // and, in flight, flown out there: 60 yards past the flight master.
        x = moveEndX_;
        y = moveEndY_;
        z = moveEndZ_;
        isMoving_ = false;
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

    void updateModelFacing(float deltaTime) {
        if (!modelFacingSet_) {
            modelFacing_ = orientation;
            modelFacingSet_ = true;
            facingVelocity_ = 0.0f;
            return;
        }
        if (facingChaseRate_ > 0.0f) {
            // 0x007160b0 on the facing and its speed, the target brought
            // within half a turn of where the model is.
            float target = orientation;
            if (modelFacing_ > target + kPi) target += 2.0f * kPi;
            else if (modelFacing_ < target - kPi) target -= 2.0f * kPi;
            wowee::math::chaseStep(modelFacing_, facingVelocity_, target, facingChaseRate_, deltaTime);
            modelFacing_ = wrapAngle(modelFacing_);
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
    // A monster move's spline set forward or back in them, which its end
    // takes off; see setMonsterMoveFlags.
    bool splineFlagsArrive_ = false;
    bool launched_ = false;
    // Asked for its locomotion again; see takeLocomotionRequest.
    bool locomotionRequested_ = false;
    bool baseAnimationAsked_ = false;
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
    std::function<std::optional<float>(const glm::vec3&)> arrivalFacing_;
    // The drawn facing; see getModelFacing.
    float modelFacing_ = 0;
    bool modelFacingSet_ = false;
    std::array<float, 4> facingDiffs_{};
    float facingChaseRate_ = 0.0f;
    float facingVelocity_ = 0.0f;
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
    /// The field, and the predicted health with it: a health field landing
    /// sets both (0x0073f330), as the unit's creation does (0x007237f0).
    void setHealth(uint32_t h) {
        health = h;
        predictedHealth = static_cast<int32_t>(h);
    }
    /// The health the client predicts (+0xfb0), which UnitHealth answers
    /// while the CVar predictedHealth is on (0x0071c2c0).
    /// When UNIT_FIELD_TARGET was last written (0x00716900 stamps +0x9e4),
    /// in app seconds; never, until then.
    [[nodiscard]] std::optional<double> targetChangedAt() const { return targetChangedAt_; }
    void noteTargetChanged(double now) { targetChangedAt_ = now; }
    [[nodiscard]] int32_t getPredictedHealth() const { return predictedHealth; }
    void setPredictedHealth(int32_t h) { predictedHealth = h; }

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

    /// Who loots this unit's corpse, as SMSG_LOOT_LIST says (0x0071bc00): the
    /// master looter (+0xc40) and the group member whose turn it is (+0xc48).
    /// The unit tooltip names them on a dead unit (0x00621070).
    struct Looters {
        uint64_t master = 0;
        uint64_t roundRobin = 0;
    };
    [[nodiscard]] const Looters& looters() const { return looters_; }
    void setLooters(const Looters& l) { looters_ = l; }

    /// The powers the client predicts (+0xfb4); see power_prediction.hpp.
    [[nodiscard]] power_prediction::Predicted& predictedPower() { return predicted; }
    [[nodiscard]] const power_prediction::Predicted& predictedPower() const { return predicted; }
    /// 0x0071c320: from the fields, as a unit is made.
    void resetPredictedPower() {
        std::array<uint32_t, 7> fields{};
        for (int t = 0; t < 7; ++t) fields[t] = powers[t];
        predicted.reset(fields);
    }

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
    int32_t predictedHealth = 0;
    std::optional<double> targetChangedAt_;
    uint32_t maxHealth = 0;
    uint32_t powers[7] = {};     // Indexed by power type (0=mana,1=rage,2=focus,3=energy,4=happiness,5=runes,6=runic)
    uint32_t maxPowers[7] = {};  // Max values per power type
    uint8_t powerType = 0;       // Active power type
    power_prediction::Predicted predicted;
    Looters looters_;
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

    /// Its rotation from the update block, on the wire's axes - what the
    /// client faces and draws it by (game_object_rotation). None where the
    /// block carried none.
    [[nodiscard]] const std::optional<glm::quat>& getRotation() const { return rotation; }
    void setRotation(const glm::quat& q) { rotation = q; }

protected:
    std::string name;
    uint32_t entry = 0;
    uint32_t displayId = 0;
    std::optional<glm::quat> rotation;
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
/// UnitIsCorpse's answer (0x0060c6f0): whether the object a unit token names
/// is a corpse object (TYPEMASK_CORPSE, 0x80) - a dead player's corpse, not a
/// dead creature. A creature that died stays a unit; asked by health, every
/// dead mob came out a corpse and TargetFrame_CheckLevel drew it a skull in
/// place of its level, which showed whenever the frame checked the level of
/// one already dead (logging in beside it, or targeting it again).
inline bool isCorpseObject(const Entity* e) {
    return e != nullptr && e->getType() == ObjectType::CORPSE;
}

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
