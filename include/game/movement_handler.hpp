#pragma once

#include "game/world_packets.hpp"
#include "game/entity.hpp"
#include "core/coordinates.hpp"
#include "game/opcode_table.hpp"
#include "game/vehicle_mover.hpp"
#include "network/packet.hpp"
#include <glm/glm.hpp>
#include <chrono>
#include <deque>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace game {

class GameHandler;

class MovementHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    explicit MovementHandler(GameHandler& owner);

    void registerOpcodes(DispatchTable& table);

    // --- Public API (delegated from GameHandler) ---

    void sendMovement(Opcode opcode);
    void setPosition(float x, float y, float z);
    void setOrientation(float orientation);
    void setMovementPitch(float radians) { movementInfo.pitch = radians; }
    void dismount();

    // Follow target (moved from GameHandler)
    void followTarget();
    void cancelFollow();
    // Per-frame movement toward the followed target's live position, called
    // from GameHandler::update() alongside the existing followRenderPos_
    // refresh. A no-op when no follow target is set. Recomputes the target
    // position fresh every call (from the live entity, not a stored point),
    // so it doesn't need the long-distance Z-interpolation guard the
    // headless client's single-shot /movement/goto needs - there's never a
    // large stale remaining distance to interpolate across.
    void updateFollowMovement(float deltaTime);

    // Area trigger detection
    void loadAreaTriggerDbc();
    void checkAreaTriggers();

    // Transport attachment
    void setTransportAttachment(uint64_t childGuid, ObjectType type, uint64_t transportGuid,
                                const glm::vec3& localOffset, bool hasLocalOrientation,
                                float localOrientation,
                                bool offsetNeedsTransportResolution = false);
    void clearTransportAttachment(uint64_t childGuid);
    void updateAttachedTransportChildren(float deltaTime);

    // --- The active mover (DAT_00ca1238) ---
    /// The unit the client moves and names in its movement packets: the
    /// player, a unit the server gave it control of - a vehicle it drives, a
    /// unit it possesses - or none (0). movementInfo is that unit's movement.
    [[nodiscard]] uint64_t activeMover() const;
    /// Whether a unit's move spline from its monster move still runs (+0xbc).
    [[nodiscard]] bool onMoveSpline(uint64_t guid);
    /// 0x00717c50 at world entry: the player again, and no other unit
    /// controlled.
    void resetActiveMover();
    /// 0x00729010: the old mover's movement stopped - the player's said with
    /// CMSG_MOVE_NOT_ACTIVE_MOVER - then CMSG_SET_ACTIVE_MOVER for the new
    /// one, whose movement movementInfo takes over.
    void setActiveMover(uint64_t guid);
    /// Whether the client controls a unit (its +0xa30 0x400, 0x0071c930).
    [[nodiscard]] bool unitHasClientControl(uint64_t guid) const;
    /// The player's PLAYER_FARSIGHT changed (0x006e4fd0 -> 0x006e2880).
    void onFarsightChanged(uint64_t farsight);
    /// A unit came into view: a control update that waited for it is
    /// applied (0x007402b0 -> 0x0072cca0), and the farsight object looked
    /// through once it is there.
    void onUnitCreated(uint64_t guid);
    /// The active mover's speeds: the player's, or the unit's own.
    [[nodiscard]] MovementSpeeds moverSpeeds() const;
    /// The mover's pitch asked to `pitch` (0x005fb3a0): at once and sent as
    /// MSG_MOVE_SET_PITCH, or - movement flags2 0x10 - over time at its
    /// pitch rate (0x0072d470).
    void requestMoverPitch(float pitch);
    /// The aim keys (VehicleAimUp/DownStart/Stop, 0x005fb1a0): the mover
    /// pitches up or down at its pitch rate while one is held.
    void setAimKey(bool up, bool held);
    /// The driven vehicle's own requests, its movement stopped first (move
    /// events 0x37 and 0x34 -> 0x006e9980): CMSG_CHANGE_SEATS_ON_CONTROLLED_
    /// VEHICLE and CMSG_DISMISS_CONTROLLED_VEHICLE.
    void sendChangeSeatsOnControlledVehicle(uint64_t vehicleGuid, uint8_t seat);
    void sendDismissControlledVehicle();
    /// What VehicleAimIncrement steps from (0x005fb4b0): the pitch a pitch
    /// over time is on its way to, else the mover's.
    [[nodiscard]] float aimBasePitch() const { return pitchingToTarget_ ? pitchTarget_ : movementInfo.pitch; }
    /// Each frame: the mover's pitch over time, and a full-speed pitcher's
    /// held start sent after 100 ms (0x00721b90). True where the pitch moved.
    bool updateMoverPitch(float deltaTime);

    // Movement info accessors
    [[nodiscard]] const MovementInfo& getMovementInfo() const { return movementInfo; }
    MovementInfo& getMovementInfoMut() { return movementInfo; }

    // Speed accessors
    [[nodiscard]] float getServerRunSpeed() const { return serverRunSpeed_; }
    [[nodiscard]] float getServerWalkSpeed() const { return serverWalkSpeed_; }
    [[nodiscard]] float getServerSwimSpeed() const { return serverSwimSpeed_; }
    [[nodiscard]] float getServerSwimBackSpeed() const { return serverSwimBackSpeed_; }
    [[nodiscard]] float getServerFlightSpeed() const { return serverFlightSpeed_; }
    [[nodiscard]] float getServerFlightBackSpeed() const { return serverFlightBackSpeed_; }
    [[nodiscard]] float getServerRunBackSpeed() const { return serverRunBackSpeed_; }
    [[nodiscard]] float getServerTurnRate() const { return serverTurnRate_; }

    // CREATE_OBJECT/LIVING carries the authoritative speed table on login.
    // Apply it to the same cache used by camera movement and later force-speed
    // packets so reconnecting while mounted retains the server-provided speed.
    void applyServerMovementSpeeds(float walk, float run, float runBack,
                                   float swim, float swimBack, float flight,
                                   float flightBack, float turn, float pitch);

    // Movement flag queries
    [[nodiscard]] bool isPlayerRooted() const { return movementInfo.isPlayerRooted(); }
    [[nodiscard]] bool isGravityDisabled() const { return movementInfo.isGravityDisabled(); }
    [[nodiscard]] bool isFeatherFalling() const { return movementInfo.isFeatherFalling(); }
    [[nodiscard]] bool isWaterWalking() const { return movementInfo.isWaterWalking(); }
    [[nodiscard]] bool isPlayerFlying() const { return movementInfo.isPlayerFlying(); }
    [[nodiscard]] bool isHovering() const { return movementInfo.isHovering(); }
    [[nodiscard]] bool isSwimming() const { return movementInfo.isSwimming(); }

    // Taxi / Flight Paths
    [[nodiscard]] bool isTaxiWindowOpen() const { return taxiWindowOpen_; }
    void closeTaxi();
    void activateTaxi(uint32_t destNodeId);
    [[nodiscard]] bool isOnTaxiFlight() const { return onTaxiFlight_; }

    /// True while a locally-initiated dismount is waiting on the server to
    /// agree. The player's mount field keeps its old value for a few frames
    /// after the request, and taking that at face value re-mounts them.
    [[nodiscard]] bool isDismountPending() const { return dismountGraceRemaining_ > 0.0f; }
    void clearDismountPending() { dismountGraceRemaining_ = 0.0f; }
    [[nodiscard]] bool isTaxiMountActive() const { return taxiMountActive_; }
    [[nodiscard]] bool isTaxiActivationPending() const { return taxiActivatePending_; }
    void forceClearTaxiAndMovementState();
    [[nodiscard]] const std::string& getTaxiDestName() const { return taxiDestName_; }
    [[nodiscard]] const ShowTaxiNodesData& getTaxiData() const { return currentTaxiData_; }
    [[nodiscard]] uint32_t getTaxiCurrentNode() const { return currentTaxiData_.nearestNode; }

    struct TaxiNode {
        uint32_t id = 0;
        uint32_t mapId = 0;
        float x = 0, y = 0, z = 0;
        std::string name;
        uint32_t mountDisplayIdAlliance = 0;
        uint32_t mountDisplayIdHorde = 0;
    };
    struct TaxiPathEdge {
        uint32_t pathId = 0;
        uint32_t fromNode = 0, toNode = 0;
        uint32_t cost = 0;
    };
    struct TaxiPathNode {
        uint32_t id = 0;
        uint32_t pathId = 0;
        uint32_t nodeIndex = 0;
        uint32_t mapId = 0;
        float x = 0, y = 0, z = 0;
    };

    [[nodiscard]] const std::unordered_map<uint32_t, TaxiNode>& getTaxiNodes() const { return taxiNodes_; }

    /// Drops what was read out of the taxi DBCs, so the next ask reloads them.
    ///
    /// Called when the active expansion changes. GameHandler::resetDbcCaches
    /// cleared copies of its own for this, and getTaxiNodes forwards here - so
    /// switching expansion left the previous one's flight points in place and
    /// nothing said so.
    void resetTaxiDbcCache() {
        taxiNodes_.clear();
        taxiPathEdges_.clear();
        taxiPathNodes_.clear();
        taxiDbcLoaded_ = false;
    }
    // WotLK 3.3.5a TaxiNodes.dbc has 384 entries; the known-taxi bitmask
    // is 12 × uint32 = 384 bits. Node IDs outside this range are invalid.
    static constexpr uint32_t kMaxTaxiNodeId = 384;
    [[nodiscard]] bool isKnownTaxiNode(uint32_t nodeId) const {
        if (nodeId == 0 || nodeId > kMaxTaxiNodeId) return false;
        uint32_t idx = nodeId - 1;
        return (knownTaxiMask_[idx / 32] & (1u << (idx % 32))) != 0;
    }
    [[nodiscard]] uint32_t getTaxiCostTo(uint32_t destNodeId) const;
    /// True when the taxi cost map has a route from the current node to dest.
    [[nodiscard]] bool hasTaxiRouteTo(uint32_t destNodeId) const;
    /// Node-id hop chain current → dest (inclusive); empty if unreachable.
    [[nodiscard]] std::vector<uint32_t> getTaxiRouteTo(uint32_t destNodeId) const;
    [[nodiscard]] bool taxiNpcHasRoutes(uint64_t guid) const {
        auto it = taxiNpcHasRoutes_.find(guid);
        return it != taxiNpcHasRoutes_.end() && it->second;
    }

    void updateClientTaxi(float deltaTime);
    // Ends the active client-simulated taxi flight. See the definition in
    // movement_handler.cpp for what snapToFinalWaypoint means - callers outside
    // MovementHandler (SMSG_DISMOUNT / UNIT_FIELD_MOUNTDISPLAYID handlers reacting
    // to an authoritative server completion signal) should pass false.
    void finishClientTaxiFlight(bool snapToFinalWaypoint);
    // Remember an early server-side dismount/flag clear. Some cores send the
    // completion signal well before our local spline endpoint and do not repeat
    // it, so updateClientTaxi() consumes it once the player reaches the landing
    // zone instead of waiting for the exact final waypoint.
    void deferServerTaxiCompletion();
    // Server cores can clear taxi flags/dismount before the client spline reaches
    // its final waypoint. Only treat those signals as authoritative near arrival.
    [[nodiscard]] bool isNearTaxiDestination(float maxDistance = 24.0f) const;
    uint32_t nextMovementTimestampMs();
    void sanitizeMovementForTaxi();

    // Heartbeat / movement timing (for GameHandler::update())
    float& timeSinceLastMoveHeartbeatRef() { return timeSinceLastMoveHeartbeat_; }
    [[nodiscard]] float getMoveHeartbeatInterval() const { return moveHeartbeatInterval_; }
    [[nodiscard]] bool isServerMovementAllowed() const { return serverMovementAllowed_; }
    /// Whether the player is in the air on a flying mount, as the movement
    /// code worked it out - which sets and clears FLYING, the flag the server
    /// reads flight from. Only while flight is allowed: a FLYING the server
    /// set by itself is left alone.
    void setFlightAirborne(bool airborne);
    /// Where the last movement packet that went out put the player, in
    /// canonical coordinates - which is where the server has them - and false
    /// before any has.
    [[nodiscard]] bool lastSentPosition(glm::vec3& out) const {
        if (!hasSentMovement_) return false;
        out = lastSentPos_;
        return true;
    }
    void setServerMovementAllowed(bool v) { serverMovementAllowed_ = v; }
    uint32_t& monsterMovePacketsThisTickRef() { return monsterMovePacketsThisTick_; }
    uint32_t& monsterMovePacketsDroppedThisTickRef() { return monsterMovePacketsDroppedThisTick_; }

    // Movement clock / fall state setters (formerly accessed via friend)
    void resetMovementClock() { movementClockStart_ = std::chrono::steady_clock::now(); lastMovementTimestampMs_ = 0; }
    void setFalling(bool falling) { isFalling_ = falling; }
    void setFallStartMs(uint32_t ms) { fallStartMs_ = ms; }

    // Taxi state references for GameHandler update/processing
    bool& onTaxiFlightRef() { return onTaxiFlight_; }
    bool& taxiMountActiveRef() { return taxiMountActive_; }
    bool& taxiActivatePendingRef() { return taxiActivatePending_; }
    float& taxiActivateTimerRef() { return taxiActivateTimer_; }
    bool& taxiClientActiveRef() { return taxiClientActive_; }
    float& taxiLandingCooldownRef() { return taxiLandingCooldown_; }
    float& taxiStartGraceRef() { return taxiStartGrace_; }
    bool& taxiRecoverPendingRef() { return taxiRecoverPending_; }
    uint32_t& taxiRecoverMapIdRef() { return taxiRecoverMapId_; }
    glm::vec3& taxiRecoverPosRef() { return taxiRecoverPos_; }
    std::unordered_map<uint64_t, bool>& taxiNpcHasRoutesRef() { return taxiNpcHasRoutes_; }
    uint32_t* knownTaxiMaskPtr() { return knownTaxiMask_; }
    bool& taxiMaskInitializedRef() { return taxiMaskInitialized_; }
    uint64_t& taxiNpcGuidRef() { return taxiNpcGuid_; }
    [[nodiscard]] uint64_t getTaxiNpcGuid() const { return taxiNpcGuid_; }

    // Other-player movement timing (for cleanup on despawn etc.)
    std::unordered_map<uint64_t, uint32_t>& otherPlayerMoveTimeMsRef() { return otherPlayerMoveTimeMs_; }
    std::unordered_map<uint64_t, float>& otherPlayerSmoothedIntervalMsRef() { return otherPlayerSmoothedIntervalMs_; }

    // Methods also called from GameHandler's registerOpcodeHandlers
    void handleCompressedMoves(network::Packet& packet);
    void handleForceMoveFlagChange(network::Packet& packet, const char* name, Opcode ackOpcode, uint32_t flag, bool set);
    void handleMoveSetCollisionHeight(network::Packet& packet);
    void applyTaxiMountForCurrentNode();
    /// Plays out a monster move held for a vehicle's animation (0x00747990).
    void releaseHeldVehicleMove(uint64_t mover);
    /// A move of the unit's is held for its vehicle's animation (its
    /// passenger component's +0x10 0x200, 0x00748070 to 0x00748230).
    [[nodiscard]] bool vehicleMoveHeld(uint64_t unit) const { return heldVehicleMoves_.count(unit) != 0; }

private:
    /// A monster move held for a vehicle's enter or exit animation, kept as
    /// it came (0x00748170) and run again as it is let go.
    struct HeldVehicleMove {
        uint16_t opcode = 0;
        std::vector<uint8_t> data;
        bool transportForm = false;
    };
    std::unordered_map<uint64_t, HeldVehicleMove> heldVehicleMoves_;
    bool replayingVehicleMove_ = false;
    /// 0x0074c040: a move for a unit with one held lets that one go first;
    /// then this one is held where the vehicle's animation asks.
    bool holdForVehicle(const network::Packet& packet, uint64_t mover, uint64_t transport, uint8_t seat,
                        bool transportForm);
    // --- Packet handlers ---
    void handleMonsterMove(network::Packet& packet);
    void handleMonsterMoveTransport(network::Packet& packet);
    void handleOtherPlayerMovement(network::Packet& packet);
    void handleMoveSetSpeed(network::Packet& packet);
    void handleForceRunSpeedChange(network::Packet& packet);
    /// `guid` 0 is the player.
    network::Packet buildForceAck(Opcode ackOpcode, uint32_t counter, uint64_t guid = 0);
    void handleForceSpeedChange(network::Packet& packet, const char* name, Opcode ackOpcode, float* speedStorage,
                                float MovementSpeeds::* unitMember);
    /// The movement a forced change of `guid` lands on: the mover's, or the
    /// player's held one while another unit moves; none for another unit.
    [[nodiscard]] MovementInfo* movementFor(uint64_t guid);
    void handleForceMoveRootState(network::Packet& packet, bool rooted);
    void handleMoveKnockBack(network::Packet& packet);
    void handleTeleportAck(network::Packet& packet);
    void handleNewWorld(network::Packet& packet);
    void handleShowTaxiNodes(network::Packet& packet);
    void handleClientControlUpdate(network::Packet& packet);
    /// 0x0072cca0 for a unit in view.
    void applyControlUpdate(uint64_t guid, bool allow);
    /// The unit the camera looks through (the camera's +0x88): the farsight
    /// object in view, else the player.
    [[nodiscard]] uint64_t cameraTarget() const;
    [[nodiscard]] uint64_t playerFarsight() const;
    /// A unit's move spline from its monster move (+0xbc, 0x0098c770), and
    /// whether it still runs: not let go (+0x20 0x400) nor run out.
    void noteMoveSpline(uint64_t guid, bool hasPath, uint32_t splineFlags, uint32_t durationMs);
    /// The movement of `guid` as the client keeps it: movementInfo for the
    /// mover, the player's held one while another unit moves.
    [[nodiscard]] const MovementInfo& movementOf(uint64_t guid) const;
    /// A movement packet for the mover, in server coordinates.
    [[nodiscard]] MovementInfo moverWireInfo();
    /// A passenger's offset on `transportGuid` as the wire carries it - the
    /// inverse of how it came in (TransportManager::serverToTransportLocal).
    [[nodiscard]] glm::vec3 transportOffsetToWire(uint64_t transportGuid, const glm::vec3& localOffset) const;
    void handleActivateTaxiReply(network::Packet& packet);
    void loadTaxiDbc();

    // --- Private helpers ---
    void buildTaxiCostMap();
    void startClientTaxiPath(const std::vector<uint32_t>& pathNodes);
    // Evaluates the same uniform Catmull-Rom curve used to render/pace taxi
    // flight motion, shared between the per-segment arc-length precomputation
    // in startClientTaxiPath() and the per-frame position update.
    static glm::vec3 evalTaxiCatmullRom(const glm::vec3& p0, const glm::vec3& p1,
                                         const glm::vec3& p2, const glm::vec3& p3, float t);
    // Commits a taxi flight already built by startClientTaxiPath(): snaps the player
    // to the path start and sets taxiClientActive_. Only called once
    // SMSG_ACTIVATETAXIREPLY confirms success - see activateTaxi()'s comment.
    void beginTaxiFlightMotion();
    bool restoreWorldTransferFallbackIfNearOrigin(const char* context);

    GameHandler& owner_;

    // --- Movement state ---
    // Reference to GameHandler's movementInfo to avoid desync
    MovementInfo& movementInfo;
    std::chrono::steady_clock::time_point movementClockStart_ = std::chrono::steady_clock::now();
    uint32_t lastMovementTimestampMs_ = 0;
    bool serverMovementAllowed_ = true;
    uint32_t monsterMovePacketsThisTick_ = 0;
    uint32_t monsterMovePacketsDroppedThisTick_ = 0;

    // Fall/jump tracking
    bool isFalling_ = false;
    uint32_t fallStartMs_ = 0;

    // Heartbeat timing
    int heartbeatLogCount_ = 0;  // periodic position audit counter
    uint32_t lastAreaTriggerId_ = 0;
    uint32_t postTransferReturnAreaTriggerId_ = 0;
    // Previous checkAreaTriggers() sample for swept-path testing (canonical
    // coords). Mounted speed covers ~3 yd per 0.25s check, enough to straddle
    // small portal boxes when only the instantaneous position is tested.
    glm::vec3 lastAreaTriggerCheckPos_{0.0f};
    uint32_t lastAreaTriggerCheckMapId_ = 0xFFFFFFFFu;
    bool lastAreaTriggerCheckValid_ = false;
    bool postTransferReturnAreaTriggerSawNear_ = false;
    bool pendingAreaTriggerDestinationValid_ = false;
    uint32_t pendingAreaTriggerDestinationMapId_ = 0;
    glm::vec3 pendingAreaTriggerDestinationServerPos_{0.0f};
    float pendingAreaTriggerDestinationServerO_ = 0.0f;
    bool worldTransferFallbackValid_ = false;
    uint32_t worldTransferFallbackMapId_ = 0;
    uint32_t worldTransferFallbackTriggerId_ = 0;
    glm::vec3 worldTransferFallbackCanonicalPos_{0.0f};
    float worldTransferFallbackCanonicalO_ = 0.0f;
    float timeSinceLastMoveHeartbeat_ = 0.0f;
    // Seconds left in which a stale non-zero mount field is not to be believed.
    // Bounded so a dismount the server refuses recovers on its own rather than
    // leaving the player permanently unable to look mounted.
    float dismountGraceRemaining_ = 0.0f;
    float moveHeartbeatInterval_ = 0.5f;
    uint32_t lastHeartbeatSendTimeMs_ = 0;
    float lastHeartbeatX_ = 0.0f;
    float lastHeartbeatY_ = 0.0f;
    float lastHeartbeatZ_ = 0.0f;
    uint32_t lastHeartbeatFlags_ = 0;
    uint64_t lastHeartbeatTransportGuid_ = 0;
    uint32_t lastNonHeartbeatMoveSendTimeMs_ = 0;
    glm::vec3 lastSentPos_{0.0f};
    bool hasSentMovement_ = false;
    /// Why movement is being held back from the server, said once per stretch
    /// of it. See sendMovement.
    const char* movementHeldReason_ = nullptr;
    uint32_t lastFacingSendTimeMs_ = 0;
    float lastFacingSentOrientation_ = 0.0f;

    // Speed state
    float serverRunSpeed_ = 7.0f;
    float serverWalkSpeed_ = 2.5f;
    float serverRunBackSpeed_ = 4.5f;
    float serverSwimSpeed_ = 4.722f;
    float serverSwimBackSpeed_ = 2.5f;
    float serverFlightSpeed_ = 7.0f;
    float serverFlightBackSpeed_ = 4.5f;
    float serverTurnRate_ = core::coords::PI;
    float serverPitchRate_ = core::coords::PI;

    // Other-player movement smoothing
    std::unordered_map<uint64_t, uint32_t> otherPlayerMoveTimeMs_;
    std::unordered_map<uint64_t, float>    otherPlayerSmoothedIntervalMs_;

    // --- Taxi / Flight Path state ---
    std::unordered_map<uint64_t, bool> taxiNpcHasRoutes_;
    std::unordered_map<uint32_t, TaxiNode> taxiNodes_;
    std::vector<TaxiPathEdge> taxiPathEdges_;
    std::unordered_map<uint32_t, std::vector<TaxiPathNode>> taxiPathNodes_;
    bool taxiDbcLoaded_ = false;
    bool taxiWindowOpen_ = false;
    ShowTaxiNodesData currentTaxiData_;
    uint64_t taxiNpcGuid_ = 0;
    /// Triggers already reported as a near miss, so the log says it once.
    std::set<uint32_t> nearMissLogged_;
    bool onTaxiFlight_ = false;
    std::string taxiDestName_;
    // Set in activateTaxi(); used by finishClientTaxiFlight() to snap to the
    // destination TaxiNodes.dbc entry's own registered position rather than
    // taxiClientPath_'s last waypoint (from the separate TaxiPathNode.dbc
    // table) - live-confirmed the two can disagree by 100+ yards, since a
    // taxi path's own waypoint trace doesn't necessarily terminate exactly on
    // the node's registered coordinate the way GM teleports/.gps do.
    uint32_t taxiDestNodeId_ = 0;
    bool taxiMountActive_ = false;
    bool taxiActivatePending_ = false;
    float taxiActivateTimer_ = 0.0f;
    // How long to wait for SMSG_ACTIVATETAXIREPLY before giving up - see
    // updateClientTaxi()'s comment on why a dropped reply needs this now.
    static constexpr float kTaxiActivateReplyTimeoutSeconds = 8.0f;
    bool taxiClientActive_ = false;
    float taxiLandingCooldown_ = 0.0f;
    float taxiStartGrace_ = 0.0f;
    size_t taxiClientIndex_ = 0;
    std::vector<glm::vec3> taxiClientPath_;
    // Arc length of the smooth Catmull-Rom curve actually rendered through each
    // consecutive pair of taxiClientPath_ points - one entry per segment, same
    // indexing as taxiClientIndex_. Pacing (how long each segment takes to
    // traverse at taxiClientSpeed_) uses this instead of the straight-line
    // chord distance between waypoints: the server's own flight spline paces
    // by the curve's real length too, and a smooth curve through a series of
    // waypoints is essentially always shorter than the straight-line polyline
    // connecting the same points (the curve "cuts" corners at turns). Using
    // chord length there made our total flight duration systematically longer
    // than the server's identical-speed (32.0f) spline - unnoticeable on a
    // short hop but live-confirmed to drift ~90 yards short of the true
    // destination on a long multi-waypoint flight (Booty Bay -> Stormwind),
    // by the time the server's own flight ends and clears UNIT_FLAG_TAXI_FLIGHT.
    // Computed once per segment when the path is built (see startClientTaxiPath()).
    std::vector<float> taxiClientSegmentArcLengths_;
    float taxiClientSpeed_ = 32.0f;
    float taxiClientSegmentProgress_ = 0.0f;
    bool taxiServerCompletionPending_ = false;
    bool taxiRecoverPending_ = false;
    uint32_t taxiRecoverMapId_ = 0;
    glm::vec3 taxiRecoverPos_{0.0f};
    uint32_t knownTaxiMask_[12] = {};
    bool taxiMaskInitialized_ = false;
    std::unordered_map<uint32_t, uint32_t> taxiCostMap_;
    // BFS predecessor per node from buildTaxiCostMap; getTaxiRouteTo walks it.
    std::unordered_map<uint32_t, uint32_t> taxiPrevMap_;

    bool followMoveMoving_ = false;  // whether MSG_MOVE_START_FORWARD has been sent for the current follow

    // --- The active mover ---
    std::optional<uint64_t> activeMover_;  ///< DAT_00ca1238; none set: the player
    uint64_t moverInfoOwner_ = 0;          ///< whose movement movementInfo is; 0 the player's
    std::unordered_map<uint64_t, vehicle::MoveSpline> moveSplines_;
        MovementInfo heldPlayerMovement_;      ///< the player's while another unit moves
    std::unordered_map<uint64_t, bool> unitControl_;  ///< +0xa30 0x400 of units but the player
    /// A control update for a unit not in view, kept for its create
    /// (0x00716060: DAT_00ca1248..50).
    std::optional<std::pair<uint64_t, bool>> pendingControl_;
    bool farSightAnnounced_ = false;       ///< the player's +0x1858 0x1
    // The mover's pitch over time (event 0x36 -> 0x33) and the aim keys.
    bool pitchingToTarget_ = false;
    uint32_t pitchStopMs_ = 0;
    float pitchTarget_ = 0.0f;
    int aimKeys_ = 0;                      ///< 1 up (0x400), 2 down (0x800)
    bool aimKeyPitching_ = false;          ///< the input control's 0x80000
    // A full-speed pitcher's (flags2 0x10) held START_PITCH (+0x954, +0x94c)
    // and whether one went out since (+0xa30 0x8000000).
    std::optional<Opcode> heldPitchStart_;
    uint32_t heldPitchStartMs_ = 0;
    bool pitchStartSent_ = false;
    bool flushingPitchStart_ = false;
    float lastSentFacing_ = 0.0f;           ///< +0x294
    float lastSentPitch_ = 0.0f;            ///< +0x295
    [[nodiscard]] float moverPitchRate() const;
    void startMoverPitch(bool up);
    void stopMoverPitch();
    /// 0x006e9980: the mover's turn, pitch and strafe stopped and said, its
    /// run let go.
    void stopMoverForRequest();
};

} // namespace game
} // namespace wowee
