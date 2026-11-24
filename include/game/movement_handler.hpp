#pragma once

#include "game/area_trigger_watch.hpp"
#include "game/world_packets.hpp"
#include "game/entity.hpp"
#include "core/coordinates.hpp"
#include "game/opcode_table.hpp"
#include "game/vehicle_mover.hpp"
#include "game/taxi_map.hpp"
#include "game/teleport_ack_gate.hpp"
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

    /// `trailer`: a uint32 written after the movement block, as
    /// CMSG_MOVE_SPLINE_DONE's spline id is (0x0071f210).
    void sendMovement(Opcode opcode, std::optional<uint32_t> trailer = std::nullopt);
    /// 0x006eae70 -> 0x0071f210: the active mover's spline run to its end
    /// sends CMSG_MOVE_SPLINE_DONE with its spline id, once.
    void updateMoverSplineDone();
    /// 0x006f1490 -> 0x00717d90: while the ground under the active mover has
    /// not streamed in (its movement held), each frame's time goes out as
    /// CMSG_MOVE_TIME_SKIPPED and is left out of its movement times.
    void updateMoverTimeSkip(float deltaTime);
    void setPosition(float x, float y, float z);
    void setOrientation(float orientation);
    void setMovementPitch(float radians) { movementInfo.pitch = radians; }
    void dismount();

    // Follow target (moved from GameHandler)
    void followTarget();
    // The same, for a unit that is not the current target - which is what a
    // typed "/follow Bob" resolves to when Bob is standing somewhere else.
    // 0x005224c0 hands the resolver's answer straight to the follow state;
    // there is no "make it the target first" step to borrow.
    void followGuid(uint64_t guid);
    void cancelFollow();

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
    /// The unit the camera looks through (the camera's +0x88): the farsight
    /// object in view, else the player.
    [[nodiscard]] uint64_t cameraTarget() const;
    /// Whether CMSG_FAR_SIGHT has said the player looks through its far
    /// sight object (+0x1858 0x1).
    [[nodiscard]] bool farSightAnnounced() const { return farSightAnnounced_; }
    /// Whether a unit's move spline from its monster move still runs (+0xbc).
    [[nodiscard]] bool onMoveSpline(uint64_t guid);
    /// The spline in a unit's create or movement block, or none: the one it
    /// was on goes either way (0x006f1520). `queuedMs`: how long ago the
    /// block was read.
    void takeBlockSpline(const UpdateBlock& block, const std::shared_ptr<Entity>& entity, uint32_t queuedMs);
    /// The flags of a unit's move spline while it still runs.
    [[nodiscard]] std::optional<uint32_t> moveSplineFlags(uint64_t guid);
    /// The flags of the unit's move spline as the client keeps it (+0xbc),
    /// with 0x400 once it has run out; none when it has no spline kept.
    [[nodiscard]] std::optional<uint32_t> keptMoveSplineFlags(uint64_t guid) const;
    /// 0x00717c50 at world entry: the player again, and no other unit
    /// controlled.
    void resetActiveMover();
    /// 0x00729010: the old mover's movement stopped - the player's said with
    /// CMSG_MOVE_NOT_ACTIVE_MOVER - then CMSG_SET_ACTIVE_MOVER for the new
    /// one, whose movement movementInfo takes over.
    void setActiveMover(uint64_t guid);
    /// Whether the active mover is a unit the player possesses rather than
    /// rides (vehicle::moverIsPossessed, decided as it became the mover):
    /// the player then stays where it stood.
    [[nodiscard]] bool moverPossessed() const;
    /// The player's own movement, wherever another unit moves in its stead.
    [[nodiscard]] const MovementInfo& playerMovement() const;
    /// Whether the client controls a unit (its +0xa30 0x400, 0x0071c930).
    [[nodiscard]] bool unitHasClientControl(uint64_t guid) const;
    /// The player's PLAYER_FARSIGHT changed (0x006e4fd0 -> 0x006e2880).
    void onFarsightChanged(uint64_t farsight);
    /// SMSG_CLEAR_FAR_SIGHT_IMMEDIATE (0x006e2e90 case 0x20d -> 0x006e2b00):
    /// while looking through far sight, PLAYER_FARSIGHT is gone through again.
    void clearFarSightImmediate();
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
    /// 0x0072ead0 -> 0x006ee460: the mover's pitch set at once, held in its
    /// vehicle row's limits (Flags 0x40), and sent (MSG_MOVE_SET_PITCH).
    void setMoverPitchNow(float pitch);
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

    /// The player's collision height as the server last gave it
    /// (SMSG_MOVE_SET_COLLISION_HGT: mounted, or off its mount again); none
    /// since the player's object was made, when it is its model's, or its
    /// mount's where it was made mounted (0x0073fcc0).
    [[nodiscard]] std::optional<float> collisionHeightGiven() const { return collisionHeightGiven_; }
    void forgetCollisionHeight() { collisionHeightGiven_.reset(); }

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
    [[nodiscard]] bool isTaxiMountActive() const { return taxiMountActive_ || onTaxiFlight_; }
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
        taxiBounds_.clear();
        taxiMapExpansion_.clear();
        worldMapTransforms_.clear();
        taxiMap_ = {};
        taxiRoutes_.clear();
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
    /// The flight map's nodes (0x005932e0), in the order the bindings count them.
    [[nodiscard]] const TaxiMap& getTaxiMap() const { return taxiMap_; }
    /// Where a node is on the flight map as the end of a leg.
    [[nodiscard]] std::pair<float, float> taxiLegPosition(uint32_t nodeId) const;
    /// True when there is a flight from the current node to dest.
    [[nodiscard]] bool hasTaxiRouteTo(uint32_t destNodeId) const;
    /// Node-id hop chain current → dest (inclusive); empty if unreachable.
    [[nodiscard]] std::vector<uint32_t> getTaxiRouteTo(uint32_t destNodeId) const;
    [[nodiscard]] bool taxiNpcHasRoutes(uint64_t guid) const {
        auto it = taxiNpcHasRoutes_.find(guid);
        return it != taxiNpcHasRoutes_.end() && it->second;
    }

    void updateClientTaxi(float deltaTime);
    /// Off any flight and off any route of one, as a login or another map
    /// finds the player; the server says again when it has them fly.
    void forgetTaxi();
    /// The splines of a world left: their clock and their units are gone.
    void forgetMoveSplines() { moveSplines_.clear(); }
    /// Where the server put the player, canonical: what the next packet with a
    /// place in it is measured by (noteOutgoingPlace).
    void noteServerPlace(const glm::vec3& canonical) { knownPlace_ = canonical; }
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
    /// The movement clock as it reads now, for what only looks at it: never
    /// moved on past the real time (nextMovementTimestampMs gives each packet
    /// its own millisecond, and asked every frame it ran ahead).
    [[nodiscard]] uint32_t movementClockMs() const;
    /// A fall that starts without a jump: the ground gone from under the
    /// player's feet (0x00762980 finds none -> 0x00988370(0)). FALLING is
    /// set, the fall time (+0x80) counts from \p sinceMs ago, and the jump
    /// block carries no upward speed (+0xb8 = 0), the direction and speed it
    /// was moving with. No packet of its own: the next one out - a heartbeat
    /// while it falls (0x006f09f0 sends them for flags & 0xc0100f), the
    /// landing - carries it. Nothing while a fall is already on.
    void startFall(uint32_t sinceMs);
    /// The fall gone far (0x007612b0: +0x44 0x2000), carried by the next
    /// packet.
    void setFallingFar();
    /// A fall's step landed the mover (0x007612b0 -> 0x00988490), having
    /// fallen \p droppedYards from its highest point (0x00986fb0). FALLING
    /// goes with it; MSG_MOVE_FALL_LAND goes out where the server was told of
    /// the fall or the drop was one (0x0073d4a0). Nothing without a fall on.
    void landFall(float droppedYards);
    /// The fall over with nothing said: too short to tell of, held back, or
    /// the mover carried off by something else.
    void endFallUnsaid();
    void sanitizeMovementForTaxi();

    /// The mover's heartbeat where one is due (0x006f09f0).
    void updateHeartbeat();
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
    void forgetFall() {
        isFalling_ = false;
        fallStartMs_ = 0;
        fallSaid_ = false;
    }

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

    /// A movement packet for the mover, in server coordinates.
    [[nodiscard]] MovementInfo moverWireInfo();

    /// MSG_MOVE_TELEPORT_ACK goes out once the place teleported to is in
    /// (TeleportAckGate): the world-entry callback holds it when it queues
    /// the area load, and the loader lets it go when the load is done.
    void holdTeleportAck() { teleportAck_.hold(); }
    /// Sends the held acknowledgement, if any. Returns whether one went.
    bool releaseTeleportAck();
    [[nodiscard]] bool hasPendingTeleportAck() const { return teleportAck_.pending(); }
    /// A new session's world: a teleport of the last one is not answered.
    void dropTeleportAck() {
        teleportAck_.drop();
        worldportAck_.drop();
    }
    /// MSG_MOVE_WORLDPORT_ACK goes out once the new world is in (0x00403b70 loads
    /// it, then sends 0xdc): held and let go as the teleport's.
    void holdWorldportAck() { worldportAck_.hold(); }
    bool releaseWorldportAck();

private:
    TeleportAckGate teleportAck_;
    TeleportAckGate worldportAck_;
    void sendWorldportAck();
    void sendTeleportAck(const TeleportAckGate::Ack& pending);

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
    /// An event of the player's own movement, handed to its animation
    /// (0x00741xxx -> 0x0073ed10).
    void moverEvent(Opcode op, uint32_t flagsBefore, bool launchedBefore);
    void handleTeleportAck(network::Packet& packet);
    void handleTransferPending(network::Packet& packet);
    void handleNewWorld(network::Packet& packet);
    void handleShowTaxiNodes(network::Packet& packet);
    void handleClientControlUpdate(network::Packet& packet);
    /// 0x0072cca0 for a unit in view.
    void applyControlUpdate(uint64_t guid, bool allow);
    [[nodiscard]] uint64_t playerFarsight() const;
    /// A unit's move spline from its monster move (+0xbc, 0x0098c770), and
    /// whether it still runs: not let go (+0x20 0x400) nor run out.
    /// How a transport's axes - the wire's - lie in the world now; none where
    /// it is not there.
    [[nodiscard]] std::optional<glm::mat4> transportFrame(uint64_t transportGuid) const;
    void noteMoveSpline(uint64_t guid, bool hasPath, uint32_t splineFlags, uint32_t durationMs,
                        uint32_t splineId = 0);
    /// The movement of `guid` as the client keeps it: movementInfo for the
    /// mover, the player's held one while another unit moves.
    [[nodiscard]] const MovementInfo& movementOf(uint64_t guid) const;
    /// A passenger's offset on `transportGuid` as the wire carries it - the
    /// inverse of how it came in (TransportManager::serverToTransportLocal).
    [[nodiscard]] glm::vec3 transportOffsetToWire(uint64_t transportGuid, const glm::vec3& localOffset) const;
    /// A passenger's facing on `transportGuid` as the wire carries it, for a
    /// canonical world yaw (transport_local::localFacing); `kept` where the
    /// transport is not one the manager runs, such as a vehicle.
    [[nodiscard]] float transportFacingToWire(uint64_t transportGuid, float canonicalYaw, float kept) const;
    void handleActivateTaxiReply(network::Packet& packet);
    void loadTaxiDbc();

    // --- Private helpers ---
    void buildTaxiMap();
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
    /// A 3.3.5 flight is the server's: UNIT_FLAG_TAXI_FLIGHT says the player
    /// is on one, and the splines of SMSG_MONSTER_MOVE carry them along it
    /// (0x0073c8e0), each reported with CMSG_MOVE_SPLINE_DONE as it ends. A
    /// flight to another map is two of them: the server moves the player over
    /// on the first one's report. The route in the tables is never flown.
    void updateServerTaxi(float deltaTime);
    void endServerTaxi();
    /// Whether the unit a forced change names is there to take it and answer
    /// it. 0x00732450, the handler of all of them, looks it up (0x004d4db0)
    /// and has nothing to do with one that is not: no answer goes out.
    [[nodiscard]] bool unitForForcedChange(uint64_t guid, const char* name);
    /// Says so in the log when a place about to be told the server is nowhere
    /// near the last one it or we named.
    void noteOutgoingPlace(uint16_t wireOp, const MovementInfo& canonical);
    [[nodiscard]] bool playerTaxiFlag() const;
    bool restoreWorldTransferFallbackIfNearOrigin(const char* context);

    GameHandler& owner_;

    // --- Movement state ---
    // Reference to GameHandler's movementInfo to avoid desync
    MovementInfo& movementInfo;
    std::chrono::steady_clock::time_point movementClockStart_ = std::chrono::steady_clock::now();
    float timeSkipCarryMs_ = 0.0f;  ///< a frame's part of a millisecond, carried
    uint32_t lastMovementTimestampMs_ = 0;
    bool serverMovementAllowed_ = true;
    uint32_t monsterMovePacketsThisTick_ = 0;
    uint32_t monsterMovePacketsDroppedThisTick_ = 0;

    // Fall/jump tracking
    /// The fall's timer stopped: how long it ran, none where it did not.
    std::optional<uint32_t> endFallTimer();
    /// The fall time from here, and the fall's jump block (0x00988370).
    void fallBegan(uint32_t startMs);
    bool isFalling_ = false;
    uint32_t fallStartMs_ = 0;
    /// The last packet that went out off a transport told of a fall
    /// (+0xa30 0x80, 0x0071ef80).
    bool fallSaid_ = false;
    std::optional<float> collisionHeightGiven_;

    // Heartbeat timing
    int heartbeatLogCount_ = 0;  // periodic position audit counter
    uint32_t lastAreaTriggerId_ = 0;
    AreaTriggerWatch areaTriggerWatch_;
    // The last SMSG_TRANSFER_PENDING named a transport: the player goes to
    // the next map on it, and SMSG_NEW_WORLD carries their place on its deck.
    bool transferPendingOnTransport_ = false;
    uint32_t transferPendingTransportEntry_ = 0;
    uint32_t transferPendingTransportMapId_ = 0;
    bool pendingAreaTriggerDestinationValid_ = false;
    uint32_t pendingAreaTriggerDestinationMapId_ = 0;
    glm::vec3 pendingAreaTriggerDestinationServerPos_{0.0f};
    float pendingAreaTriggerDestinationServerO_ = 0.0f;
    bool worldTransferFallbackValid_ = false;
    uint32_t worldTransferFallbackMapId_ = 0;
    uint32_t worldTransferFallbackTriggerId_ = 0;
    glm::vec3 worldTransferFallbackCanonicalPos_{0.0f};
    float worldTransferFallbackCanonicalO_ = 0.0f;
    // Seconds left in which a stale non-zero mount field is not to be believed.
    // Bounded so a dismount the server refuses recovers on its own rather than
    // leaving the player permanently unable to look mounted.
    float dismountGraceRemaining_ = 0.0f;
    uint32_t lastMoveSendMs_ = 0;  ///< the last movement packet of any kind (0x006e9b70)
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
    bool onTaxiFlight_ = false;
    /// The flight's UNIT_FLAG_TAXI_FLIGHT has been there (updateServerTaxi);
    /// until then the accepted request stands for it, for so long.
    bool taxiFlagSeen_ = false;
    std::optional<glm::vec3> knownPlace_;
    float taxiFlagWait_ = 0.0f;
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
    TaxiMap taxiMap_;          ///< what the open flight map lists
    TaxiRoutes taxiRoutes_;    ///< and the flights over it
    std::unordered_map<uint32_t, TaxiMapBounds> taxiBounds_;       ///< WorldMapContinent, by map
    std::unordered_map<uint32_t, uint32_t> taxiMapExpansion_;      ///< Map.dbc's ExpansionID
    std::vector<WorldMapTransform> worldMapTransforms_;

    // --- The active mover ---
    std::optional<uint64_t> activeMover_;  ///< DAT_00ca1238; none set: the player
    uint64_t moverInfoOwner_ = 0;          ///< whose movement movementInfo is; 0 the player's
    bool moverPossessed_ = false;          ///< the mover other than the player is possessed, not ridden
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
    // A full-speed turner's (flags2 0x8) held START_TURN (+0x950, +0x948)
    // and whether one went out since (+0xa30 0x4000000).
    std::optional<Opcode> heldTurnStart_;
    uint32_t heldTurnStartMs_ = 0;
    bool turnStartSent_ = false;
    bool flushingPitchStart_ = false;
    /// 0x00721c20: the held START_TURN and START_PITCH out, in that order.
    void flushHeldStarts();
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
