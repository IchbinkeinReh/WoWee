#include "game/movement_handler.hpp"
#include "game/packet_wait.hpp"
#include "game/passenger_place.hpp"
#include "game/mover_mode.hpp"
#include "game/move_heartbeat.hpp"
#include "game/transport_route_clock.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/inventory_handler.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera_controller.hpp"
#include "game/packet_parsers.hpp"
#include "game/spline_packet.hpp"
#include "game/vehicle.hpp"
#include "game/vehicle_mover.hpp"
#include "game/update_field_table.hpp"
#include <bit>
#include "game/transport_manager.hpp"
#include "game/transport_local.hpp"
#include "game/entity.hpp"
#include "game/combat_handler.hpp"
#include "game/monster_move_flags.hpp"
#include "network/world_socket.hpp"
#include "network/packet.hpp"
#include "network/wire_format.hpp"
#include "core/coordinates.hpp"
#include "core/application.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "core/logger.hpp"
#include "game/interface_text.hpp"
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <zlib.h>
#include <array>
#include <unordered_set>

namespace wowee {
namespace game {

namespace {

/// The bank of a unit on a flying spline (0x0098ca00, spline flag 0x2000):
/// twice the level angle from the way it goes to the place it is in a second,
/// at most a quarter turn, into the turn. In the renderer's axes, where a
/// positive roll tilts to the right.
float flightBank(const glm::vec3& tangent, const glm::vec3& ahead) {
    const glm::vec2 a(tangent.x, tangent.y), b(ahead.x, ahead.y);
    const float la = glm::length(a), lb = glm::length(b);
    if (la < 4.8828125e-4f || lb < 4.8828125e-4f) return 0.0f;
    const float angle = std::acos(std::clamp(glm::dot(a, b) / (la * lb), -1.0f, 1.0f));
    const float cross = a.x * b.y - a.y * b.x;
    return std::clamp(2.0f * (cross < 0.0f ? angle : -angle), -1.5707964f, 1.5707964f);
}

/// A taxi refusal as the original shows it: its ERR_TAXI* string, through
/// UI_ERROR_MESSAGE or UI_INFO_MESSAGE as the game error table at 0x00ac8af0
/// files that one (NOSUCHPATH, NOTENOUGHMONEY, TOOFARAWAY, NOVENDORNEARBY,
/// NOTVISITED, PLAYERMOVING and NOTSTANDING are yellow, the rest red).
struct TaxiError { const char* key; const char* enUS; bool info; };
constexpr TaxiError kTaxiNoSuchPath{"ERR_TAXINOSUCHPATH", "There is no direct path to that destination!", true};

void showTaxiError(GameHandler& owner, const TaxiError& e) {
    const std::string text = interfaceString(e.key, e.enUS);
    if (e.info) owner.addUIInfoMessage(text);
    else owner.addUIError(text);
    owner.playGameErrorSound(e.key);
}

/// SMSG_ACTIVATETAXIREPLY's result -> its error, as 0x006cee90 looks it up
/// in the table at 0x00ad9c94; nothing for 0 (success) or an unknown result.
const TaxiError* taxiReplyError(uint32_t result) {
    static constexpr TaxiError kErrors[] = {
        {"ERR_TAXIUNSPECIFIEDSERVERERROR", "Unspecified server taxi error.", false},
        kTaxiNoSuchPath,
        {"ERR_TAXINOTENOUGHMONEY", "You don't have enough money!", true},
        {"ERR_TAXITOOFARAWAY", "You are too far away from the taxi stand!", true},
        {"ERR_TAXINOVENDORNEARBY", "There is no taxi vendor nearby!", true},
        {"ERR_TAXINOTVISITED", "You haven't reached that taxi node on foot yet!", true},
        {"ERR_TAXIPLAYERBUSY", "You are busy and can't use the taxi service now.", false},
        {"ERR_TAXIPLAYERALREADYMOUNTED", "You are already mounted! Dismount first.", false},
        {"ERR_TAXIPLAYERSHAPESHIFTED", "You can't take a taxi while shapeshifted!", false},
        {"ERR_TAXIPLAYERMOVING", "You are moving.", true},
        {"ERR_TAXISAMENODE", "You are already there!", false},
        {"ERR_TAXINOTSTANDING", "You need to be standing to go anywhere.", true},
    };
    if (result == 0 || result > std::size(kErrors)) return nullptr;
    return &kErrors[result - 1];
}

struct KnownAreaTriggerDestination {
    uint32_t triggerId;
    uint32_t mapId;
    float serverX;
    float serverY;
    float serverZ;
    float serverO;
};

constexpr KnownAreaTriggerDestination kKnownAreaTriggerDestinations[] = {
    {.triggerId = 2166, .mapId = 0,   .serverX = -4838.95f, .serverY = -1318.46f, .serverZ = 501.868f, .serverO = 1.42372f}, // Deeprun Tram -> Ironforge
    {.triggerId = 2171, .mapId = 0,   .serverX = -8364.57f,   .serverY = 535.981f, .serverZ = 91.7969f, .serverO = 2.24619f}, // Deeprun Tram -> Stormwind
    {.triggerId = 2173, .mapId = 369,    .serverX = 68.3006f, .serverY = 2490.91f, .serverZ = -4.29647f, .serverO = 3.12192f}, // Stormwind -> Deeprun Tram
    {.triggerId = 2175, .mapId = 369,    .serverX = 69.2542f,   .serverY = 10.257f, .serverZ = -4.29664f, .serverO = 3.09832f}, // Ironforge -> Deeprun Tram
};

const KnownAreaTriggerDestination* findKnownAreaTriggerDestination(uint32_t triggerId, uint32_t mapId) {
    for (const auto& dest : kKnownAreaTriggerDestinations) {
        if (dest.triggerId == triggerId && dest.mapId == mapId) return &dest;
    }
    return nullptr;
}

const KnownAreaTriggerDestination* findKnownAreaTriggerDestinationByTrigger(uint32_t triggerId) {
    for (const auto& dest : kKnownAreaTriggerDestinations) {
        if (dest.triggerId == triggerId) return &dest;
    }
    return nullptr;
}

// What the client does with another unit's animation for the opcode its
// movement came in (FUN_0073ed10). Every start and stop, turn, swim and
// ascent asks for its locomotion at once (FUN_0073ac30), as does a change of
// walk or run mode while under way; a heartbeat, a facing or a landing asks
// for nothing more.
MoveOpcodeKind moveOpcodeKind(uint16_t wireOp) {
    if (wireOp == wireOpcode(Opcode::MSG_MOVE_JUMP)) return MoveOpcodeKind::Jump;
    if (wireOp == wireOpcode(Opcode::CMSG_MOVE_SET_FLY)) return MoveOpcodeKind::SetFly;
    if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_RUN_MODE) ||
        wireOp == wireOpcode(Opcode::MSG_MOVE_SET_WALK_MODE)) {
        return MoveOpcodeKind::LocomotionIfMoving;
    }
    for (const Opcode op : {Opcode::MSG_MOVE_START_FORWARD, Opcode::MSG_MOVE_START_BACKWARD,
                            Opcode::MSG_MOVE_STOP, Opcode::MSG_MOVE_START_STRAFE_LEFT,
                            Opcode::MSG_MOVE_START_STRAFE_RIGHT, Opcode::MSG_MOVE_STOP_STRAFE,
                            Opcode::MSG_MOVE_START_TURN_LEFT, Opcode::MSG_MOVE_START_TURN_RIGHT,
                            Opcode::MSG_MOVE_STOP_TURN, Opcode::MSG_MOVE_START_SWIM,
                            Opcode::MSG_MOVE_STOP_SWIM, Opcode::MSG_MOVE_START_SWIM_CHEAT,
                            Opcode::MSG_MOVE_STOP_SWIM_CHEAT, Opcode::MSG_MOVE_START_ASCEND,
                            Opcode::MSG_MOVE_STOP_ASCEND, Opcode::MSG_MOVE_START_DESCEND,
                            Opcode::MSG_MOVE_ROOT, Opcode::MSG_MOVE_UPDATE_CAN_FLY}) {
        if (wireOp == wireOpcode(op)) return MoveOpcodeKind::Locomotion;
    }
    return MoveOpcodeKind::None;
}

} // namespace

MovementHandler::MovementHandler(GameHandler& owner)
    : owner_(owner), movementInfo(owner.movementInfoRef()) {}

void MovementHandler::applyServerMovementSpeeds(float walk, float run, float runBack,
                                                 float swim, float swimBack, float flight,
                                                 float flightBack, float turn, float pitch) {
    auto apply = [](float value, float& destination) {
        if (std::isfinite(value) && value > 0.01f && value < 200.0f) {
            destination = value;
        }
    };
    apply(walk, serverWalkSpeed_);
    apply(run, serverRunSpeed_);
    apply(runBack, serverRunBackSpeed_);
    apply(swim, serverSwimSpeed_);
    apply(swimBack, serverSwimBackSpeed_);
    apply(flight, serverFlightSpeed_);
    apply(flightBack, serverFlightBackSpeed_);
    apply(turn, serverTurnRate_);
    apply(pitch, serverPitchRate_);
    LOG_INFO("Movement speeds restored from object update: walk=", serverWalkSpeed_,
             " run=", serverRunSpeed_, " runBack=", serverRunBackSpeed_,
             " swim=", serverSwimSpeed_, " flight=", serverFlightSpeed_);
}

void MovementHandler::registerOpcodes(DispatchTable& table) {
    // Creature movement
    table[Opcode::SMSG_MONSTER_MOVE] = [this](network::Packet& packet) { handleMonsterMove(packet); };
    table[Opcode::SMSG_COMPRESSED_MOVES] = [this](network::Packet& packet) { handleCompressedMoves(packet); };
    table[Opcode::SMSG_MONSTER_MOVE_TRANSPORT] = [this](network::Packet& packet) { handleMonsterMoveTransport(packet); };

    // A unit the server moves: one flag each (0x00740ba0), the packed guid and
    // nothing more. Several were read and dropped, so a creature or pet set to
    // hover stayed on the ground until it walked, and what the unit's
    // locomotion reads of its flags stayed what its create block had said.
    {
        // These are this client's own movement flags, not wire values: the
        // opcode carries no flag word. Naming them keeps them following
        // MovementFlags, which is what every reader compares against.
        constexpr auto asFlag = [](MovementFlags f) { return static_cast<uint32_t>(f); };
        constexpr uint32_t kWalkBit = asFlag(MovementFlags::WALKING);
        constexpr uint32_t kSwimBit = asFlag(MovementFlags::SWIMMING);
        constexpr uint32_t kFlyBits =
            asFlag(MovementFlags::CAN_FLY) | asFlag(MovementFlags::FLYING);
        constexpr uint32_t kHover = asFlag(MovementFlags::HOVER);
        constexpr uint32_t kSafeFall = asFlag(MovementFlags::FEATHER_FALL);
        constexpr uint32_t kWaterWalk = asFlag(MovementFlags::WATER_WALK);
        constexpr uint32_t kNoGravity = asFlag(MovementFlags::LEVITATING);
        constexpr uint32_t kRoot = asFlag(MovementFlags::ROOT);
        constexpr uint32_t kRootBits = kRoot | asFlag(MovementFlags::PENDING_ROOT);

        struct Change {
            Opcode opcode;
            uint32_t set, mask;
        };
        // Run mode says the unit is not walking. It says nothing about
        // swimming or flying, and clearing those took the flight animation off
        // anything that started moving. CAN_FLY because that is what
        // SET_FLYING means - AzerothCore sends it from Unit::SetCanFly for
        // units the client does not control - and FLYING because that is the
        // flag the animation reads.
        for (const Change& change : {Change{Opcode::SMSG_SPLINE_MOVE_SET_WALK_MODE, kWalkBit, kWalkBit},
                                     Change{Opcode::SMSG_SPLINE_MOVE_SET_RUN_MODE, 0u, kWalkBit},
                                     Change{Opcode::SMSG_SPLINE_MOVE_SET_FLYING, kFlyBits, kFlyBits},
                                     Change{Opcode::SMSG_SPLINE_MOVE_UNSET_FLYING, 0u, kFlyBits},
                                     Change{Opcode::SMSG_SPLINE_MOVE_START_SWIM, kSwimBit, kSwimBit},
                                     Change{Opcode::SMSG_SPLINE_MOVE_STOP_SWIM, 0u, kSwimBit},
                                     Change{Opcode::SMSG_SPLINE_MOVE_FEATHER_FALL, kSafeFall, kSafeFall},
                                     Change{Opcode::SMSG_SPLINE_MOVE_NORMAL_FALL, 0u, kSafeFall},
                                     Change{Opcode::SMSG_SPLINE_MOVE_WATER_WALK, kWaterWalk, kWaterWalk},
                                     Change{Opcode::SMSG_SPLINE_MOVE_LAND_WALK, 0u, kWaterWalk},
                                     Change{Opcode::SMSG_SPLINE_MOVE_GRAVITY_DISABLE, kNoGravity, kNoGravity},
                                     Change{Opcode::SMSG_SPLINE_MOVE_GRAVITY_ENABLE, 0u, kNoGravity},
                                     Change{Opcode::SMSG_SPLINE_MOVE_SET_HOVER, kHover, kHover},
                                     Change{Opcode::SMSG_SPLINE_MOVE_UNSET_HOVER, 0u, kHover},
                                     Change{Opcode::SMSG_SPLINE_MOVE_ROOT, kRoot, kRoot},
                                     Change{Opcode::SMSG_SPLINE_MOVE_UNROOT, 0u, kRootBits}}) {
            table[change.opcode] = [this, change](network::Packet& packet) {
                if (!packet.hasRemaining(1)) return;
                const uint64_t guid = packet.readPackedGuid();
                if (guid == 0 || guid == owner_.getPlayerGuid()) return;
                splineMoveFlags(guid, change.opcode, change.set, change.mask);
            };
        }
    }

    // Spline speed: all opcodes share the same PackedGuid+float format, differing
    // only in which member receives the value. Factory avoids 8 copy-pasted lambdas.
    // Another unit's speed goes on its entity (Entity::movementSpeeds).
    auto makeSplineSpeedHandler = [this](float MovementHandler::* member,
                                         float MovementSpeeds::* unitMember) {
        return [this, member, unitMember](network::Packet& packet) {
            if (!packet.hasRemaining(5)) return;
            uint64_t guid = packet.readPackedGuid();
            if (!packet.hasRemaining(4)) return;
            float speed = packet.readFloat();
            if (!std::isfinite(speed) || speed <= 0.01f || speed >= 200.0f) return;
            if (guid == owner_.getPlayerGuid()) {
                this->*member = speed;
            } else if (auto entity = owner_.getEntityManager().getEntity(guid)) {
                entity->movementSpeeds().*unitMember = speed;
            }
        };
    };
    table[Opcode::SMSG_SPLINE_SET_RUN_SPEED]      = makeSplineSpeedHandler(&MovementHandler::serverRunSpeed_, &MovementSpeeds::run);
    table[Opcode::SMSG_SPLINE_SET_RUN_BACK_SPEED]  = makeSplineSpeedHandler(&MovementHandler::serverRunBackSpeed_, &MovementSpeeds::runBack);
    table[Opcode::SMSG_SPLINE_SET_SWIM_SPEED]      = makeSplineSpeedHandler(&MovementHandler::serverSwimSpeed_, &MovementSpeeds::swim);

    // Force speed changes
    table[Opcode::SMSG_FORCE_RUN_SPEED_CHANGE] = [this](network::Packet& packet) { handleForceRunSpeedChange(packet); };
    table[Opcode::SMSG_FORCE_MOVE_ROOT] = [this](network::Packet& packet) { handleForceMoveRootState(packet, true); };
    table[Opcode::SMSG_FORCE_MOVE_UNROOT] = [this](network::Packet& packet) { handleForceMoveRootState(packet, false); };
    table[Opcode::SMSG_FORCE_WALK_SPEED_CHANGE] = [this](network::Packet& packet) {
        handleForceSpeedChange(packet, "WALK_SPEED", Opcode::CMSG_FORCE_WALK_SPEED_CHANGE_ACK, &serverWalkSpeed_,
                               &MovementSpeeds::walk);
    };
    table[Opcode::SMSG_FORCE_RUN_BACK_SPEED_CHANGE] = [this](network::Packet& packet) {
        handleForceSpeedChange(packet, "RUN_BACK_SPEED", Opcode::CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK, &serverRunBackSpeed_,
                               &MovementSpeeds::runBack);
    };
    table[Opcode::SMSG_FORCE_SWIM_SPEED_CHANGE] = [this](network::Packet& packet) {
        handleForceSpeedChange(packet, "SWIM_SPEED", Opcode::CMSG_FORCE_SWIM_SPEED_CHANGE_ACK, &serverSwimSpeed_,
                               &MovementSpeeds::swim);
    };
    table[Opcode::SMSG_FORCE_SWIM_BACK_SPEED_CHANGE] = [this](network::Packet& packet) {
        handleForceSpeedChange(packet, "SWIM_BACK_SPEED", Opcode::CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK, &serverSwimBackSpeed_,
                               &MovementSpeeds::swimBack);
    };
    table[Opcode::SMSG_FORCE_FLIGHT_SPEED_CHANGE] = [this](network::Packet& packet) {
        handleForceSpeedChange(packet, "FLIGHT_SPEED", Opcode::CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK, &serverFlightSpeed_,
                               &MovementSpeeds::flight);
    };
    table[Opcode::SMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE] = [this](network::Packet& packet) {
        handleForceSpeedChange(packet, "FLIGHT_BACK_SPEED", Opcode::CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK, &serverFlightBackSpeed_,
                               &MovementSpeeds::flightBack);
    };
    table[Opcode::SMSG_FORCE_TURN_RATE_CHANGE] = [this](network::Packet& packet) {
        handleForceSpeedChange(packet, "TURN_RATE", Opcode::CMSG_FORCE_TURN_RATE_CHANGE_ACK, &serverTurnRate_,
                               &MovementSpeeds::turnRate);
    };
    table[Opcode::SMSG_FORCE_PITCH_RATE_CHANGE] = [this](network::Packet& packet) {
        handleForceSpeedChange(packet, "PITCH_RATE", Opcode::CMSG_FORCE_PITCH_RATE_CHANGE_ACK, &serverPitchRate_,
                               &MovementSpeeds::pitchRate);
    };

    // Movement flag toggles
    table[Opcode::SMSG_MOVE_SET_CAN_FLY] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "SET_CAN_FLY", Opcode::CMSG_MOVE_SET_CAN_FLY_ACK,
            static_cast<uint32_t>(MovementFlags::CAN_FLY), true);
    };
    table[Opcode::SMSG_MOVE_UNSET_CAN_FLY] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "UNSET_CAN_FLY", Opcode::CMSG_MOVE_SET_CAN_FLY_ACK,
            static_cast<uint32_t>(MovementFlags::CAN_FLY), false);
    };
    table[Opcode::SMSG_MOVE_FEATHER_FALL] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "FEATHER_FALL", Opcode::CMSG_MOVE_FEATHER_FALL_ACK,
            static_cast<uint32_t>(MovementFlags::FEATHER_FALL), true);
    };
    table[Opcode::SMSG_MOVE_WATER_WALK] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "WATER_WALK", Opcode::CMSG_MOVE_WATER_WALK_ACK,
            static_cast<uint32_t>(MovementFlags::WATER_WALK), true);
    };
    table[Opcode::SMSG_MOVE_SET_HOVER] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "SET_HOVER", Opcode::CMSG_MOVE_HOVER_ACK,
            static_cast<uint32_t>(MovementFlags::HOVER), true);
    };
    table[Opcode::SMSG_MOVE_UNSET_HOVER] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "UNSET_HOVER", Opcode::CMSG_MOVE_HOVER_ACK,
            static_cast<uint32_t>(MovementFlags::HOVER), false);
    };
    table[Opcode::SMSG_MOVE_KNOCK_BACK] = [this](network::Packet& packet) { handleMoveKnockBack(packet); };

    // Teleport: the player's own, with a counter to be answered. Another unit's
    // (0xc5) is a movement packet like the others below.
    table[Opcode::MSG_MOVE_TELEPORT_ACK] = [this](network::Packet& packet) { handleTeleportAck(packet); };
    table[Opcode::SMSG_TRANSFER_PENDING] = [this](network::Packet& packet) { handleTransferPending(packet); };
    table[Opcode::SMSG_NEW_WORLD] = [this](network::Packet& packet) { handleNewWorld(packet); };

    // Taxi
    table[Opcode::SMSG_SHOWTAXINODES] = [this](network::Packet& packet) { handleShowTaxiNodes(packet); };
    table[Opcode::SMSG_ACTIVATETAXIREPLY] = [this](network::Packet& packet) { handleActivateTaxiReply(packet); };

    // MSG_MOVE_* relay (other player movement)
    for (auto op : { Opcode::MSG_MOVE_START_FORWARD, Opcode::MSG_MOVE_START_BACKWARD,
                     Opcode::MSG_MOVE_STOP, Opcode::MSG_MOVE_START_STRAFE_LEFT,
                     Opcode::MSG_MOVE_START_STRAFE_RIGHT, Opcode::MSG_MOVE_STOP_STRAFE,
                     Opcode::MSG_MOVE_JUMP, Opcode::MSG_MOVE_START_TURN_LEFT,
                     Opcode::MSG_MOVE_START_TURN_RIGHT, Opcode::MSG_MOVE_STOP_TURN,
                     Opcode::MSG_MOVE_SET_FACING, Opcode::MSG_MOVE_FALL_LAND,
                     Opcode::MSG_MOVE_HEARTBEAT, Opcode::MSG_MOVE_START_SWIM,
                     Opcode::MSG_MOVE_STOP_SWIM, Opcode::MSG_MOVE_SET_WALK_MODE,
                     Opcode::MSG_MOVE_SET_RUN_MODE, Opcode::MSG_MOVE_START_PITCH_UP,
                     Opcode::MSG_MOVE_START_PITCH_DOWN, Opcode::MSG_MOVE_STOP_PITCH,
                     Opcode::MSG_MOVE_START_ASCEND, Opcode::MSG_MOVE_STOP_ASCEND,
                     Opcode::MSG_MOVE_START_DESCEND, Opcode::MSG_MOVE_SET_PITCH,
                     Opcode::MSG_MOVE_GRAVITY_CHNG, Opcode::MSG_MOVE_UPDATE_CAN_FLY,
                     Opcode::MSG_MOVE_UPDATE_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY,
                     // The three that were left out of this list while their
                     // siblings were in it. AzerothCore answers a force-move
                     // ack by broadcasting the mover's movement info to
                     // everyone but the mover - MiscHandler's switch turns
                     // CMSG_MOVE_HOVER_ACK into MSG_MOVE_HOVER and so on - so
                     // these carry another player's position and flags at the
                     // moment they start hovering, feather-falling or walking
                     // on water, and dropping them left that player where they
                     // were last seen until their next heartbeat.
                     Opcode::MSG_MOVE_HOVER, Opcode::MSG_MOVE_FEATHER_FALL,
                     Opcode::MSG_MOVE_WATER_WALK,
                     Opcode::MSG_MOVE_ROOT, Opcode::MSG_MOVE_UNROOT,
                     // Taking off into flight and landing from it. The
                     // server relays a player's CMSG_MOVE_SET_FLY to the
                     // others under the same opcode, and the client plays
                     // the take-off and the landing on it (FUN_0073ed10,
                     // case 0x346).
                     Opcode::CMSG_MOVE_SET_FLY,
                     // Another player knocked back: the mover's movement info
                     // and then the knock's direction and speeds, which
                     // 0x007187f0 launches the unit with. The fall block of
                     // the movement info carries the same launch.
                     Opcode::MSG_MOVE_KNOCK_BACK,
                     // 0x00740d30 sends these into the same cases as
                     // MSG_MOVE_START_SWIM and MSG_MOVE_STOP_SWIM.
                     Opcode::MSG_MOVE_START_SWIM_CHEAT, Opcode::MSG_MOVE_STOP_SWIM_CHEAT,
                     // Another unit put somewhere else (0x00741b60 -> 0x00740d30
                     // case 0xc5 -> 0x007189a0): the movement info, no counter.
                     Opcode::MSG_MOVE_TELEPORT }) {
        table[op] = [this](network::Packet& packet) {
            if (owner_.getState() == WorldState::IN_WORLD) handleOtherPlayerMovement(packet);
        };
    }

    // Another unit's collision height (0x00741b00 -> 0x007406a0 case 0x518):
    // its movement as any move message carries it, then the height, which
    // the client keeps for that unit's own collision (0x006ee810) - nothing
    // this client draws or moves others by - so the movement is what lands.
    table[Opcode::MSG_MOVE_SET_COLLISION_HGT] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleOtherPlayerMovement(packet);
    };
    // Another unit's movement clock moved on (0x0071cab0 -> 0x006e9280): the
    // packed guid and the milliseconds it skipped, added to what its
    // timestamps are measured from, so the next move's time since the last
    // one leaves the skip out.
    table[Opcode::MSG_MOVE_TIME_SKIPPED] = [this](network::Packet& packet) {
        if (owner_.getState() != WorldState::IN_WORLD) return;
        const uint64_t guid = isPreWotlk() ? packet.readUInt64() : packet.readPackedGuid();
        if (!packet.hasRemaining(4)) return;
        const uint32_t skipped = packet.readUInt32();
        if (auto it = otherPlayerMoveTimeMs_.find(guid); it != otherPlayerMoveTimeMs_.end()) it->second += skipped;
    };

    // MSG_MOVE_SET_*_SPEED relay
    for (auto op : { Opcode::MSG_MOVE_SET_RUN_SPEED, Opcode::MSG_MOVE_SET_RUN_BACK_SPEED,
                     Opcode::MSG_MOVE_SET_WALK_SPEED, Opcode::MSG_MOVE_SET_SWIM_SPEED,
                     Opcode::MSG_MOVE_SET_SWIM_BACK_SPEED, Opcode::MSG_MOVE_SET_FLIGHT_SPEED,
                     Opcode::MSG_MOVE_SET_FLIGHT_BACK_SPEED, Opcode::MSG_MOVE_SET_TURN_RATE,
                     Opcode::MSG_MOVE_SET_PITCH_RATE }) {
        table[op] = [this](network::Packet& packet) {
            if (owner_.getState() == WorldState::IN_WORLD) handleMoveSetSpeed(packet);
        };
    }

    // ---- Client control & spline speed/flag changes ----

    // Client control update
    table[Opcode::SMSG_CLIENT_CONTROL_UPDATE] = [this](network::Packet& packet) {
        handleClientControlUpdate(packet);
    };

    // Remaining spline speed opcodes - same factory as above.
    table[Opcode::SMSG_SPLINE_SET_FLIGHT_SPEED]      = makeSplineSpeedHandler(&MovementHandler::serverFlightSpeed_, &MovementSpeeds::flight);
    table[Opcode::SMSG_SPLINE_SET_FLIGHT_BACK_SPEED]  = makeSplineSpeedHandler(&MovementHandler::serverFlightBackSpeed_, &MovementSpeeds::flightBack);
    table[Opcode::SMSG_SPLINE_SET_SWIM_BACK_SPEED]    = makeSplineSpeedHandler(&MovementHandler::serverSwimBackSpeed_, &MovementSpeeds::swimBack);
    table[Opcode::SMSG_SPLINE_SET_WALK_SPEED]          = makeSplineSpeedHandler(&MovementHandler::serverWalkSpeed_, &MovementSpeeds::walk);
    table[Opcode::SMSG_SPLINE_SET_TURN_RATE]           = makeSplineSpeedHandler(&MovementHandler::serverTurnRate_, &MovementSpeeds::turnRate);
    // The pitch rate the same way: 0x00741bc0 handles it with the others.
    table[Opcode::SMSG_SPLINE_SET_PITCH_RATE]          = makeSplineSpeedHandler(&MovementHandler::serverPitchRate_, &MovementSpeeds::pitchRate);

    // ---- Player movement flag changes (server-pushed) ----
    table[Opcode::SMSG_MOVE_GRAVITY_DISABLE] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "GRAVITY_DISABLE", Opcode::CMSG_MOVE_GRAVITY_DISABLE_ACK,
            static_cast<uint32_t>(MovementFlags::LEVITATING), true);
    };
    table[Opcode::SMSG_MOVE_GRAVITY_ENABLE] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "GRAVITY_ENABLE", Opcode::CMSG_MOVE_GRAVITY_ENABLE_ACK,
            static_cast<uint32_t>(MovementFlags::LEVITATING), false);
    };
    table[Opcode::SMSG_MOVE_LAND_WALK] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "LAND_WALK", Opcode::CMSG_MOVE_WATER_WALK_ACK,
            static_cast<uint32_t>(MovementFlags::WATER_WALK), false);
    };
    table[Opcode::SMSG_MOVE_NORMAL_FALL] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "NORMAL_FALL", Opcode::CMSG_MOVE_FEATHER_FALL_ACK,
            static_cast<uint32_t>(MovementFlags::FEATHER_FALL), false);
    };
    table[Opcode::SMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "SET_CAN_TRANSITION_SWIM_FLY",
            Opcode::CMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY_ACK, 0, true);
    };
    table[Opcode::SMSG_MOVE_UNSET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "UNSET_CAN_TRANSITION_SWIM_FLY",
            Opcode::CMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY_ACK, 0, false);
    };
    table[Opcode::SMSG_MOVE_SET_COLLISION_HGT] = [this](network::Packet& packet) {
        handleMoveSetCollisionHeight(packet);
    };
    table[Opcode::SMSG_MOVE_SET_FLIGHT] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "SET_FLIGHT", Opcode::CMSG_MOVE_FLIGHT_ACK,
            static_cast<uint32_t>(MovementFlags::FLYING), true);
    };
    table[Opcode::SMSG_MOVE_UNSET_FLIGHT] = [this](network::Packet& packet) {
        handleForceMoveFlagChange(packet, "UNSET_FLIGHT", Opcode::CMSG_MOVE_FLIGHT_ACK,
            static_cast<uint32_t>(MovementFlags::FLYING), false);
    };
}

// ============================================================
// handleClientControlUpdate
// ============================================================

void MovementHandler::handleClientControlUpdate(network::Packet& packet) {
    // PackedGuid + uint8 allowMovement (0x0072d0b0).
    //
    // Through readPackedGuid rather than the byte loop that used to be written
    // out here. It decoded correctly, but it was a second copy of the packed
    // guid rule, and the layout sweep - which reads the widths a handler asks
    // for - saw a handler taking a uint8 where the server writes a guid and
    // called it a misparse. A second copy of a wire rule costs that twice
    // over: it can drift, and it hides the handler from the check.
    if (!packet.hasFullPackedGuid()) {
        LOG_WARNING("SMSG_CLIENT_CONTROL_UPDATE malformed (truncated packed guid)");
        packet.skipAll();
        return;
    }
    uint64_t controlGuid = packet.readPackedGuid();
    if (!packet.hasRemaining(1)) {
        LOG_WARNING("SMSG_CLIENT_CONTROL_UPDATE too short: ", packet.getSize(), " bytes");
        return;
    }
    const bool allowMovement = (packet.readUInt8() != 0);
    if (controlGuid == 0) controlGuid = owner_.getPlayerGuid();
    if (controlGuid != owner_.getPlayerGuid() && !owner_.getEntityManager().getEntity(controlGuid)) {
        // 0x00716060: a unit not in view keeps it for its create - the
        // latest for it, or one that gives control over one that takes it.
        if (!pendingControl_ || pendingControl_->first == controlGuid || !pendingControl_->second || allowMovement)
            pendingControl_ = std::make_pair(controlGuid, allowMovement);
        return;
    }
    applyControlUpdate(controlGuid, allowMovement);
}

// ============================================================
// Movement Timestamp
// ============================================================

uint32_t MovementHandler::nextMovementTimestampMs() {
    auto now = core::steadyNow();
    uint64_t elapsed = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - movementClockStart_).count()) + 1ULL;
    if (elapsed > std::numeric_limits<uint32_t>::max()) {
        movementClockStart_ = now;
        elapsed = 1ULL;
    }

    uint32_t candidate = static_cast<uint32_t>(elapsed);
    if (candidate <= lastMovementTimestampMs_) {
        candidate = lastMovementTimestampMs_ + 1U;
        if (candidate == 0) {
            movementClockStart_ = now;
            candidate = 1U;
        }
    }

    lastMovementTimestampMs_ = candidate;
    return candidate;
}

uint32_t MovementHandler::movementClockMs() const {
    const uint64_t elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       core::steadyNow() - movementClockStart_)
                                                       .count()) +
                             1ULL;
    return std::max(static_cast<uint32_t>(std::min<uint64_t>(elapsed, std::numeric_limits<uint32_t>::max())),
                    lastMovementTimestampMs_);
}

void MovementHandler::startFall(uint32_t sinceMs) {
    if (activeMover() != owner_.getPlayerGuid()) return;
    if (!mayStartFall(movementInfo.flags)) return;
    const uint32_t now = movementClockMs();
    movementInfo.flags = fallFlags(movementInfo.flags, movementInfo.flags2);
    fallBegan(now > sinceMs ? now - sinceMs : 0u);
    static const bool kFallDebug = [] {
        const char* v = std::getenv("WOWEE_FLOOR_DEBUG");
        return v && v[0] && v[0] != '0';
    }();
    if (kFallDebug) {
        LOG_WARNING("FALL START: walk-off z=", movementInfo.z, " since=", sinceMs, "ms start=", fallStartMs_,
                    " now=", now, " flags=0x", std::hex, movementInfo.flags, std::dec);
    }
}

void MovementHandler::fallBegan(uint32_t startMs) {
    isFalling_ = true;
    fallStartMs_ = startMs;
    movementInfo.fallTime = 0;
    // Its jump block: +0xb8 the speed it was given, 0 here; the direction
    // the keys move it (0x009881d0 -> 0x00987ef0) and its speed (+0x8c) as
    // the walk had them - the jump's block (outgoingJumpBlock) without the
    // jump's upward speed.
    const OutgoingJump jump = ownJumpBlock(movementInfo, serverWalkSpeed_,
                                           serverRunSpeed_ > 0.0f ? serverRunSpeed_ : 7.0f, serverRunBackSpeed_);
    movementInfo.jumpVelocity = 0.0f;
    movementInfo.jumpSinAngle = jump.dirX;
    movementInfo.jumpCosAngle = jump.dirY;
    movementInfo.jumpXYSpeed = jump.horizontalSpeed;
}

void MovementHandler::setFallingFar() {
    if (movementInfo.hasFlag(MovementFlags::FALLING))
        movementInfo.flags |= static_cast<uint32_t>(MovementFlags::FALLINGFAR);
}

void MovementHandler::landFall(float droppedYards) {
    if (!landingSent(movementInfo.flags)) return;
    if (fallLandSent(fallSaid_, droppedYards)) sendMovement(Opcode::MSG_MOVE_FALL_LAND);
    // Landed all the same where no packet says so - the step was too short to
    // tell of, or the server takes no movement now: the step clears FALLING
    // (0x00988490) whatever becomes of the packet.
    if (landingSent(movementInfo.flags)) endFallUnsaid();
}

void MovementHandler::endFallUnsaid() {
    const uint32_t flagsBefore = movementInfo.flags;
    const bool launchedBefore = movementInfo.jumpVelocity != 0.0f;
    movementInfo.time = movementClockMs();
    endFall(movementInfo, endFallTimer());
    // Its animation goes with the landing (0x0073d3d0), not the packet.
    if (activeMover() == owner_.getPlayerGuid())
        moverEvent(Opcode::MSG_MOVE_FALL_LAND, flagsBefore, launchedBefore);
}

bool MovementHandler::restoreWorldTransferFallbackIfNearOrigin(const char* context) {
    if (!worldTransferFallbackValid_ || owner_.getCurrentMapId() != worldTransferFallbackMapId_) {
        return false;
    }
    if (!isCorruptOriginPosition(owner_.getCurrentMapId(),
                                movementInfo.x, movementInfo.y, movementInfo.z)) {
        return false;
    }

    LOG_WARNING(context,
                ": correcting near-origin map 0 position using area trigger ",
                worldTransferFallbackTriggerId_,
                " fallback canonical=(",
                worldTransferFallbackCanonicalPos_.x, ", ",
                worldTransferFallbackCanonicalPos_.y, ", ",
                worldTransferFallbackCanonicalPos_.z, ")");

    movementInfo.x = worldTransferFallbackCanonicalPos_.x;
    movementInfo.y = worldTransferFallbackCanonicalPos_.y;
    movementInfo.z = worldTransferFallbackCanonicalPos_.z;
    movementInfo.orientation = worldTransferFallbackCanonicalO_;
    movementInfo.transportGuid = 0;
    movementInfo.transportSeat = -1;
    movementInfo.flags &= ~static_cast<uint32_t>(MovementFlags::ONTRANSPORT);
    owner_.clearPlayerTransport();

    if (auto player = owner_.getEntityManager().getEntity(owner_.getPlayerGuid())) {
        player->setPosition(movementInfo.x, movementInfo.y, movementInfo.z, movementInfo.orientation);
    }

    return true;
}

// ============================================================
// sendMovement
// ============================================================

std::optional<uint32_t> MovementHandler::endFallTimer() {
    std::optional<uint32_t> fellMs;
    if (isFalling_ && wrapMsDiff(movementInfo.time, fallStartMs_) >= 0)
        fellMs = static_cast<uint32_t>(wrapMsDiff(movementInfo.time, fallStartMs_));
    isFalling_ = false;
    fallStartMs_ = 0;
    return fellMs;
}

namespace {
/// 0x009904e0, of what is sent here: swimming begun and left, flying set. (The
/// others of its eight are vehicle requests and cheats.)
bool splineLetsThrough(Opcode opcode) {
    return opcode == Opcode::MSG_MOVE_START_SWIM || opcode == Opcode::MSG_MOVE_STOP_SWIM ||
           opcode == Opcode::CMSG_MOVE_SET_FLY || opcode == Opcode::CMSG_MOVE_SPLINE_DONE;
}
}  // namespace

void MovementHandler::setFlightAirborne(bool airborne) {
    const uint32_t canFly = static_cast<uint32_t>(MovementFlags::CAN_FLY);
    const uint32_t flying = static_cast<uint32_t>(MovementFlags::FLYING);
    if (!(movementInfo.flags & canFly)) return;
    const bool now = (movementInfo.flags & flying) != 0;
    if (airborne == now) return;
    // Taking off and landing are events of their own (0x006ef230, 0x2d and
    // 0x2e of 0x006ef860): the state first, then CMSG_MOVE_SET_FLY with it.
    // The server hears of neither otherwise. Landed, the mover falls from
    // where it is (0x0098c240), and the fall's next step lands it.
    if (airborne) {
        startFlying(movementInfo, endFallTimer());
    } else if (stopFlying(movementInfo)) {
        fallBegan(movementClockMs());
    }
    sendMovement(Opcode::CMSG_MOVE_SET_FLY);
}

void MovementHandler::sendMovement(Opcode opcode, std::optional<uint32_t> trailer) {
    if (owner_.getState() != WorldState::IN_WORLD) {
        LOG_WARNING("Cannot send movement in state: ", (int)owner_.getState());
        return;
    }

    // The keys that move and turn end being away (0x005fae70, 0x005fafb0, 0x005fb0b0,
    // 0x005face0; a vehicle's, 0x0056b5c0).
    if (!owner_.clickToMoveUnderWay() &&
        (opcode == Opcode::MSG_MOVE_START_FORWARD || opcode == Opcode::MSG_MOVE_START_BACKWARD ||
         opcode == Opcode::MSG_MOVE_START_STRAFE_LEFT || opcode == Opcode::MSG_MOVE_START_STRAFE_RIGHT ||
         opcode == Opcode::MSG_MOVE_START_TURN_LEFT || opcode == Opcode::MSG_MOVE_START_TURN_RIGHT ||
         opcode == Opcode::MSG_MOVE_START_ASCEND || opcode == Opcode::MSG_MOVE_START_DESCEND))
        owner_.clearAfkMoving();

    // Into the air before rising in it: the take-off's CMSG_MOVE_SET_FLY, and
    // the controls looked at again after it (0x007413f0 -> 0x005fbbc0). Not
    // from the water: a swimmer swims up (0x009898e0 keeps SWIMMING).
    if (opcode == Opcode::MSG_MOVE_START_ASCEND && movementInfo.hasFlag(MovementFlags::CAN_FLY) &&
        !movementInfo.hasFlag(MovementFlags::SWIMMING) && !movementInfo.hasFlag(MovementFlags::FLYING)) {
        setFlightAirborne(true);
    }

    // A landing that ends no fall is no landing (landingSent): put on a
    // lift's car by a teleport, or arriving on a transport, the mover never
    // started a fall, and the landing went out anyway with the last fall's
    // time - which only a fall's start zeroes (+0x80, 0x00988370).
    if (opcode == Opcode::MSG_MOVE_FALL_LAND && !landingSent(movementInfo.flags)) {
        static const bool kFallDebug = [] {
            const char* v = std::getenv("WOWEE_FLOOR_DEBUG");
            return v && v[0] && v[0] != '0';
        }();
        if (kFallDebug) {
            LOG_WARNING("FALL LAND: none sent - no fall started z=", movementInfo.z,
                        " fallTime=", movementInfo.fallTime, " flags=0x", std::hex, movementInfo.flags,
                        std::dec);
        }
        return;
    }

    // Block manual movement while taxi is active/mounted, but always allow
    // stop/heartbeat opcodes so stuck states can be recovered.
    bool taxiAllowed =
        (opcode == Opcode::MSG_MOVE_HEARTBEAT) ||
        (opcode == Opcode::MSG_MOVE_STOP) ||
        (opcode == Opcode::MSG_MOVE_STOP_STRAFE) ||
        (opcode == Opcode::MSG_MOVE_STOP_TURN) ||
        (opcode == Opcode::MSG_MOVE_STOP_SWIM);
    // Held back, and said: a packet not sent leaves the server with the last
    // position it was told, while the player walks on here - so every NPC the
    // player then walks up to is out of the server's reach, and refuses in
    // silence. Once per stretch of it, not per packet.
    const char* heldReason = nullptr;
    // The packet names the active mover and carries its movement (0x0071ef80):
    // the player, or the unit the client controls in its stead.
    const uint64_t mover = activeMover();
    const bool moverIsPlayer = mover == owner_.getPlayerGuid();
    if (mover == 0) {
        heldReason = "no unit is the active mover";
    } else if (!isPreWotlk() && !(moverIsPlayer && onTaxiFlight_) && onMoveSpline(mover) &&
               !splineLetsThrough(opcode)) {
        // 0x0071ef20, for any spline the server moves the mover along - a
        // charge, a pull, a leap into a seat: what is over is none, as the
        // client marks it (flag 0x400, 0x006eae70 and 0x006eaf50) and then
        // lets everything by. A flight is held below, its end included.
        heldReason = "a spline carries the mover";
    } else if (!moverIsPlayer) {
        if (!unitHasClientControl(mover)) heldReason = "the server has not given control of the unit";
    } else if (onTaxiFlight_ && !isPreWotlk()) {
        // 0x0071ef20: while a spline carries the mover nothing of its own
        // movement goes out (0x009904e0 lets a swim's start and stop and
        // CMSG_MOVE_SET_FLY by, which a flight has no use for), and what ends
        // it, CMSG_MOVE_SPLINE_DONE, is asked after the spline is marked over
        // (0x006eae70). Held back with the rest, the server never heard that
        // the player had come to the border of a map and left them there.
        if (opcode != Opcode::CMSG_MOVE_SPLINE_DONE) heldReason = "a taxi flight is in progress";
    } else if (!taxiAllowed && opcode != Opcode::CMSG_MOVE_SPLINE_DONE) {
        if (!serverMovementAllowed_) heldReason = "the server has not given control of the character";
        else if (onTaxiFlight_) heldReason = "a taxi flight is in progress";
        else if (taxiMountActive_) heldReason = "a taxi mount is on";
        else if (owner_.resurrectPendingRef()) heldReason = "a resurrection is pending";
    }
    if (heldReason) {
        if (heldReason != movementHeldReason_) {
            movementHeldReason_ = heldReason;
            LOG_WARNING("Movement: not sent while ", heldReason,
                        " - the server keeps the last position it was told");
        }
        // The mover starts and stops to swim all the same: its state changes
        // first and the packet is asked after (0x006ef860, 0x0071ef80).
        if (moverIsPlayer && opcode == Opcode::MSG_MOVE_START_SWIM) {
            startSwimming(movementInfo, endFallTimer());
        } else if (moverIsPlayer && opcode == Opcode::MSG_MOVE_STOP_SWIM && stopSwimming(movementInfo)) {
            fallBegan(movementClockMs());
        }
        return;
    }

    // Always send a strictly increasing non-zero client movement clock value.
    const uint32_t movementTime = nextMovementTimestampMs();
    movementInfo.time = movementTime;

    if (opcode == Opcode::MSG_MOVE_SET_FACING &&
        isPreWotlk()) {
        const float facingDelta = core::coords::normalizeAngleRad(
            movementInfo.orientation - lastFacingSentOrientation_);
        const uint32_t sinceLastFacingMs =
            lastFacingSendTimeMs_ != 0 && wrapMsDiff(movementTime, lastFacingSendTimeMs_) >= 0
                ? static_cast<uint32_t>(wrapMsDiff(movementTime, lastFacingSendTimeMs_))
                : std::numeric_limits<uint32_t>::max();
        if (std::abs(facingDelta) < 0.02f && sinceLastFacingMs < 200U) {
            return;
        }
    }

    // Starting to move or turn lets go of the loot (0x0072e5d0, 0x0072e680,
    // 0x0072e730, 0x0072e7e0, 0x0072e900, 0x0072e9b0, 0x0072ead0).
    if (moverIsPlayer && releasesLoot(opcode)) owner_.closeLootByMoving();

    // The state before this message. There is no event for the player starting
    // or stopping: PLAYER_STARTED_MOVING and PLAYER_STOPPED_MOVING are names from
    // a much later client, and this one's event-id table holds no movement name
    // at all - who moves is asked of the unit, not announced.
    const uint32_t flagsBefore = movementInfo.flags;
    const bool launchedBefore = movementInfo.jumpVelocity != 0.0f;

    // Starting to move (0x005faa40 -> 0x00806620) walks the pending casts and
    // cancels, with CMSG_CANCEL_CAST, only those whose spell's InterruptFlags
    // (Spell.dbc word 31) has 0x1; a cast without it keeps going while the
    // player moves. Food/water restoration still ends when the player gets up.
    if (owner_.getSpellHandler()) {
        const bool isPositionalMove =
            opcode == Opcode::MSG_MOVE_START_FORWARD  ||
            opcode == Opcode::MSG_MOVE_START_BACKWARD ||
            opcode == Opcode::MSG_MOVE_START_STRAFE_LEFT  ||
            opcode == Opcode::MSG_MOVE_START_STRAFE_RIGHT ||
            opcode == Opcode::MSG_MOVE_JUMP;
        if (isPositionalMove) {
            owner_.getSpellHandler()->cancelCastsOnMove();  // 0x00806620
        }
    }
    // A move key, autorun, both mouse buttons or a jump (0x005fbe10,
    // 0x005fbf10, 0x005fc0f0 -> 0x005faa40) ends the player's own channel
    // when its spell's ChannelInterruptFlags has 0x8: CMSG_CANCEL_CHANNELLING.
    if (moverIsPlayer && owner_.getSpellHandler() &&
        (opcode == Opcode::MSG_MOVE_START_FORWARD || opcode == Opcode::MSG_MOVE_START_BACKWARD ||
         opcode == Opcode::MSG_MOVE_START_STRAFE_LEFT || opcode == Opcode::MSG_MOVE_START_STRAFE_RIGHT ||
         opcode == Opcode::MSG_MOVE_JUMP)) {
        owner_.getSpellHandler()->cancelChannelOnMove();
    }
    // A turn key starting a turn (0x005fb0b0 -> 0x0072e7e0) ends the player's
    // channel when turning breaks it: ChannelInterruptFlags 0x10 together with
    // AttributesEx 0x4000. A mouse turn is a facing, not a turn start, and
    // leaves it alone, as the client's does.
    if (moverIsPlayer && owner_.getSpellHandler() &&
        (opcode == Opcode::MSG_MOVE_START_TURN_LEFT || opcode == Opcode::MSG_MOVE_START_TURN_RIGHT)) {
        owner_.getSpellHandler()->cancelChannelOnTurn();
    }

    // The pitch's packets go out only for a unit that can pitch: swimming,
    // flying, or movement flags2 0x20 (0x007413f0 for 0xbf..0xc1 and 0xdb;
    // 0x00989220 sets no pitch flag otherwise).
    const bool pitchOpcode = opcode == Opcode::MSG_MOVE_START_PITCH_UP ||
                             opcode == Opcode::MSG_MOVE_START_PITCH_DOWN ||
                             opcode == Opcode::MSG_MOVE_STOP_PITCH || opcode == Opcode::MSG_MOVE_SET_PITCH;
    const bool canPitch = movementInfo.hasFlag(MovementFlags::SWIMMING) ||
                          movementInfo.hasFlag(MovementFlags::FLYING) || (movementInfo.flags2 & 0x0020) != 0;
    if (pitchOpcode && !canPitch) return;
    // 0x007413f0: with flags2 0x8 the turn's packets go through 0x007219f0,
    // with 0x10 the pitch's through 0x00721ac0; any other packet takes the
    // starts they hold out after it (0x00721c20).
    const bool turnOpcode = opcode == Opcode::MSG_MOVE_START_TURN_LEFT ||
                            opcode == Opcode::MSG_MOVE_START_TURN_RIGHT || opcode == Opcode::MSG_MOVE_STOP_TURN;
    const bool heldRoute = (turnOpcode && (movementInfo.flags2 & 0x0008) != 0) ||
                           (pitchOpcode && (movementInfo.flags2 & 0x0010) != 0);

    // Update movement flags based on opcode
    switch (opcode) {
        case Opcode::MSG_MOVE_START_FORWARD:
            movementInfo.flags |= static_cast<uint32_t>(MovementFlags::FORWARD);
            break;
        case Opcode::MSG_MOVE_START_BACKWARD:
            movementInfo.flags |= static_cast<uint32_t>(MovementFlags::BACKWARD);
            break;
        case Opcode::MSG_MOVE_STOP:
            movementInfo.flags &= ~(static_cast<uint32_t>(MovementFlags::FORWARD) |
                                    static_cast<uint32_t>(MovementFlags::BACKWARD));
            break;
        case Opcode::MSG_MOVE_START_STRAFE_LEFT:
            movementInfo.flags |= static_cast<uint32_t>(MovementFlags::STRAFE_LEFT);
            break;
        case Opcode::MSG_MOVE_START_STRAFE_RIGHT:
            movementInfo.flags |= static_cast<uint32_t>(MovementFlags::STRAFE_RIGHT);
            break;
        case Opcode::MSG_MOVE_STOP_STRAFE:
            movementInfo.flags &= ~(static_cast<uint32_t>(MovementFlags::STRAFE_LEFT) |
                                    static_cast<uint32_t>(MovementFlags::STRAFE_RIGHT));
            break;
        case Opcode::MSG_MOVE_JUMP:
            movementInfo.flags |= static_cast<uint32_t>(MovementFlags::FALLING);
            isFalling_ = true;
            fallStartMs_ = movementInfo.time;
            movementInfo.fallTime = 0;
            {
                // The client's jump block (see outgoingJumpBlock): the impulse
                // is downward-positive, -7.955547; the direction is the keys'
                // and goes out x first; the speed is the current one. This sent
                // +7.96, sin before cos and the facing alone, so every other
                // client drew our jump falling straight down and heading off
                // mirrored across the facing.
                const OutgoingJump jump = ownJumpBlock(movementInfo, serverWalkSpeed_,
                                                       serverRunSpeed_ > 0.0f ? serverRunSpeed_ : 7.0f,
                                                       serverRunBackSpeed_);
                movementInfo.jumpVelocity = jump.verticalSpeed;
                movementInfo.jumpSinAngle = jump.dirX;   // wire order: x, then y
                movementInfo.jumpCosAngle = jump.dirY;
                movementInfo.jumpXYSpeed = jump.horizontalSpeed;
            }
            // Falling now, and out of the water and the air with it: the fall's
            // start (0x00988370) clears swimming, ascending, descending and
            // flying (flags & 0xf91fffff) - a jump out of the water goes up
            // without SWIMMING, and no STOP_SWIM follows it.
            movementInfo.flags &= 0xf91fffffu;
            break;
        case Opcode::MSG_MOVE_START_TURN_LEFT:
            movementInfo.flags |= static_cast<uint32_t>(MovementFlags::TURN_LEFT);
            break;
        case Opcode::MSG_MOVE_START_TURN_RIGHT:
            movementInfo.flags |= static_cast<uint32_t>(MovementFlags::TURN_RIGHT);
            break;
        case Opcode::MSG_MOVE_STOP_TURN:
            movementInfo.flags &= ~(static_cast<uint32_t>(MovementFlags::TURN_LEFT) |
                                    static_cast<uint32_t>(MovementFlags::TURN_RIGHT));
            break;
        case Opcode::MSG_MOVE_FALL_LAND:
            // The landing carries how long the fall lasted: the client's land
            // (0x00988490) clears FALLING and FALLING_FAR and leaves the fall
            // time (+0x80) as the fall step counted it (0x007618b0,
            // 0x007612b0); only a fall's start zeroes it (0x00988370,
            // 0x00987410). It went out as 0.
            endFall(movementInfo, endFallTimer());
            break;
        case Opcode::MSG_MOVE_START_ASCEND:
            movementInfo.flags |= static_cast<uint32_t>(MovementFlags::ASCENDING);
            movementInfo.flags &= ~static_cast<uint32_t>(MovementFlags::DESCENDING);
            break;
        case Opcode::MSG_MOVE_STOP_ASCEND:
            movementInfo.flags &= ~static_cast<uint32_t>(MovementFlags::ASCENDING);
            movementInfo.flags &= ~static_cast<uint32_t>(MovementFlags::DESCENDING);
            break;
        case Opcode::MSG_MOVE_START_SWIM:
            // The camera controller has sent this pair on every water entry
            // and exit all along; nothing here listened, so no movement packet
            // this client sends has ever carried SWIMMING. The server reads
            // pitch only when the flag is set, which is why a swimming
            // character looked flat to everyone else, and isSwimming() - which
            // reads this same word - answered no while the player swam.
            // 0x00721210: the active mover starting to swim triggers
            // tutorial 0x1b.
            owner_.triggerTutorial(0x1b);
            // Entering water ends a fall, and nothing else would: FALL_LAND is
            // sent on touching ground and a swimmer never touches it, so a
            // jump into a lake left FALLING set with fallTime still climbing.
            startSwimming(movementInfo, endFallTimer());
            break;
        case Opcode::MSG_MOVE_STOP_SWIM:
            // 0x0098bff0: swimming, rising and sinking end together, and the
            // mover falls from there, before the packet goes out. Out of the
            // water level again, so the next packet that does carry a pitch -
            // a flying mount - starts from horizontal rather than from however
            // the swimmer was last angled.
            if (stopSwimming(movementInfo)) fallBegan(movementInfo.time);
            break;
        case Opcode::MSG_MOVE_START_PITCH_UP:
            movementInfo.flags = (movementInfo.flags & ~static_cast<uint32_t>(MovementFlags::PITCH_DOWN)) |
                                 static_cast<uint32_t>(MovementFlags::PITCH_UP);
            break;
        case Opcode::MSG_MOVE_START_PITCH_DOWN:
            movementInfo.flags = (movementInfo.flags & ~static_cast<uint32_t>(MovementFlags::PITCH_UP)) |
                                 static_cast<uint32_t>(MovementFlags::PITCH_DOWN);
            break;
        case Opcode::MSG_MOVE_STOP_PITCH:
            movementInfo.flags &= ~(static_cast<uint32_t>(MovementFlags::PITCH_UP) |
                                    static_cast<uint32_t>(MovementFlags::PITCH_DOWN));
            break;
        // The walk and run modes ToggleRun asks for (0x006ef860 events
        // 0x12/0x11 -> 0x0098b0e0).
        case Opcode::MSG_MOVE_SET_WALK_MODE:
            movementInfo.flags |= static_cast<uint32_t>(MovementFlags::WALKING);
            break;
        case Opcode::MSG_MOVE_SET_RUN_MODE:
            movementInfo.flags &= ~static_cast<uint32_t>(MovementFlags::WALKING);
            break;
        case Opcode::MSG_MOVE_START_DESCEND:
            // Must set DESCENDING so outgoing movement packets carry the correct
            // flag during flight descent. Only clearing ASCENDING left the flag
            // field ambiguous (neither ascending nor descending).
            movementInfo.flags &= ~static_cast<uint32_t>(MovementFlags::ASCENDING);
            movementInfo.flags |= static_cast<uint32_t>(MovementFlags::DESCENDING);
            break;
        default:
            break;
    }
    if (moverIsPlayer) moverEvent(opcode, flagsBefore, launchedBefore);

    // A full-speed turner (flags2 0x8) holds its START_TURN for 100 ms; a
    // stop before it went out is one MSG_MOVE_SET_FACING, or nothing where
    // the facing and pitch are where they were sent (0x007219f0, 0x0071ae80).
    if ((movementInfo.flags2 & 0x0008) != 0 && !flushingPitchStart_) {
        if (opcode == Opcode::MSG_MOVE_START_TURN_LEFT || opcode == Opcode::MSG_MOVE_START_TURN_RIGHT) {
            heldTurnStart_ = opcode;
            heldTurnStartMs_ = movementTime;
            return;
        }
        if (opcode == Opcode::MSG_MOVE_STOP_TURN) {
            const bool startHeld = heldTurnStart_.has_value() && !turnStartSent_;
            heldTurnStart_.reset();
            turnStartSent_ = false;
            if (startHeld) {
                if (vehicle::pitchUnchangedSinceSent(movementInfo.orientation, lastSentFacing_, movementInfo.pitch,
                                                     lastSentPitch_, canPitch))
                    return;
                opcode = Opcode::MSG_MOVE_SET_FACING;
            }
        }
    }

    // A full-speed pitcher (flags2 0x10) holds its START_PITCH for 100 ms; a
    // stop before it went out is one MSG_MOVE_SET_PITCH, or nothing where the
    // facing and pitch are where they were sent (0x00721ac0, 0x0071ae80).
    if ((movementInfo.flags2 & 0x0010) != 0 && !flushingPitchStart_) {
        if (opcode == Opcode::MSG_MOVE_START_PITCH_UP || opcode == Opcode::MSG_MOVE_START_PITCH_DOWN) {
            heldPitchStart_ = opcode;
            heldPitchStartMs_ = movementTime;
            return;
        }
        if (opcode == Opcode::MSG_MOVE_STOP_PITCH) {
            const bool startHeld = heldPitchStart_.has_value() && !pitchStartSent_;
            heldPitchStart_.reset();
            pitchStartSent_ = false;
            if (startHeld) {
                if (vehicle::pitchUnchangedSinceSent(movementInfo.orientation, lastSentFacing_, movementInfo.pitch,
                                                     lastSentPitch_, canPitch))
                    return;
                opcode = Opcode::MSG_MOVE_SET_PITCH;
            }
        }
    }

    if (opcode == Opcode::MSG_MOVE_SET_FACING) {
        lastFacingSendTimeMs_ = movementInfo.time;
        lastFacingSentOrientation_ = movementInfo.orientation;
    }

    // Keep fallTime current
    if (isFalling_ && movementInfo.hasFlag(MovementFlags::FALLING)) {
        // Across the movement clock's 32-bit wrap as well.
        const int32_t sinceFallStart = wrapMsDiff(movementInfo.time, fallStartMs_);
        uint32_t elapsed = sinceFallStart >= 0 ? static_cast<uint32_t>(sinceFallStart) : 0u;
        movementInfo.fallTime = elapsed;
    } else if (!movementInfo.hasFlag(MovementFlags::FALLING)) {
        if (isFalling_) {
            isFalling_ = false;
            fallStartMs_ = 0;
        }
        // Not falling, the last fall's time stays as the landing left it
        // (0x00988490 does not clear +0x80), and every packet after it
        // carries it until the next fall starts.
    }

    if (onTaxiFlight_ || taxiMountActive_ || taxiActivatePending_ || taxiClientActive_) {
        sanitizeMovementForTaxi();
    }

    // The player's transport; a unit moved in its stead carries its own.
    // An elevator or a tram is one as a ship is: the client attaches its
    // mover to any game object flagged a transport it stands on (0x007612b0
    // -> 0x006ec7b0 -> 0x006ec400; 0x0074b3f0 asks vtable +0xec, a game
    // object's GAMEOBJECT_FLAGS & GO_FLAG_TRANSPORT, 0x00712f20), M2 or WMO,
    // and writes the block whenever it has one (0x00987140).
    bool includeTransportInWire = moverIsPlayer && owner_.isOnTransport();
    // Carried to another map on a transport, the player is on it before the
    // transport is known there: the movement keeps its transport (the
    // client's mover keeps the guid and offset it was sent, 0x00987140
    // writes the block whenever it has one). A packet without it took the
    // player off the transport on the server, which then left them behind at
    // the next map change - the ride back never came.
    const bool transferOnTransport = moverIsPlayer && owner_.hasPendingPlayerTransportWorldTransfer() &&
                                     owner_.pendingPlayerTransportGuid() != 0;

    // Add transport data if player is on a server-recognized transport
    if (includeTransportInWire) {
        bool transportResolved = false;
        if (owner_.getTransportManager()) {
            auto* tr = owner_.getTransportManager()->getTransport(owner_.playerTransportGuidRef());
            if (tr) {
                transportResolved = true;
                glm::vec3 composed = owner_.getTransportManager()->getPlayerWorldPosition(owner_.playerTransportGuidRef(), owner_.playerTransportOffsetRef());
                movementInfo.x = composed.x;
                movementInfo.y = composed.y;
                movementInfo.z = composed.z;
            }
        }
        if (!transportResolved && transferOnTransport) {
            // Kept, with the offset the transfer gave (below).
        } else if (!transportResolved) {
            // Transport not tracked - don't send ONTRANSPORT to the server.
            // Sending stale transport GUID + local offset causes the server to
            // compute a bad world position and teleport us to map origin.
            LOG_WARNING("sendMovement: transport 0x", std::hex, owner_.playerTransportGuidRef(),
                        std::dec, " not found - clearing transport state");
            includeTransportInWire = false;
            owner_.clearPlayerTransport();
        }
    }
    const bool pendingTransportWire =
        transferOnTransport && (!includeTransportInWire || !owner_.getTransportManager() ||
                                !owner_.getTransportManager()->getTransport(owner_.playerTransportGuidRef()));
    if (pendingTransportWire) {
        includeTransportInWire = true;
        movementInfo.flags |= static_cast<uint32_t>(MovementFlags::ONTRANSPORT);
        movementInfo.transportGuid = owner_.pendingPlayerTransportGuid();
        // Put back on the wire's axes below, as any other's.
        const glm::vec3 offset =
            owner_.getTransportManager()
                ? owner_.getTransportManager()->serverToTransportLocal(owner_.pendingPlayerTransportGuid(),
                                                                       owner_.pendingPlayerTransportWireOffset())
                : core::coords::serverToCanonical(owner_.pendingPlayerTransportWireOffset());
        movementInfo.transportX = offset.x;
        movementInfo.transportY = offset.y;
        movementInfo.transportZ = offset.z;
        movementInfo.transportTime = movementInfo.time;
        movementInfo.transportSeat = -1;
        movementInfo.transportTime2 = movementInfo.time;
    } else if (includeTransportInWire) {
        movementInfo.flags |= static_cast<uint32_t>(MovementFlags::ONTRANSPORT);
        movementInfo.transportGuid = owner_.playerTransportGuidRef();
        movementInfo.transportX = owner_.playerTransportOffsetRef().x;
        movementInfo.transportY = owner_.playerTransportOffsetRef().y;
        movementInfo.transportZ = owner_.playerTransportOffsetRef().z;
        movementInfo.transportTime = movementInfo.time;
        movementInfo.transportSeat = -1;
        movementInfo.transportTime2 = movementInfo.time;

        // The facing on it, as the wire has it (0x00987140).
        movementInfo.transportO = transportFacingToWire(
            movementInfo.transportGuid, movementInfo.orientation, movementInfo.transportO);
    } else {
        movementInfo.flags &= ~static_cast<uint32_t>(MovementFlags::ONTRANSPORT);
        movementInfo.transportGuid = 0;
        movementInfo.transportSeat = -1;
    }
    {
        // WOWEE_MOVE_TRACE: every movement packet as it goes out - its
        // opcode, flags and transport - for following what the server is told
        // across a transport's map change.
        static const bool kMoveTrace = [] {
            const char* v = std::getenv("WOWEE_MOVE_TRACE");
            return v && v[0] && v[0] != '0';
        }();
        if (kMoveTrace) {
            LOG_WARNING("MOVE SENT: op=0x", std::hex, wireOpcode(opcode), " flags=0x", movementInfo.flags,
                        " transport=0x", (includeTransportInWire ? movementInfo.transportGuid : 0ull), std::dec,
                        " pos=(", movementInfo.x, ", ", movementInfo.y, ", ", movementInfo.z, ") local=(",
                        movementInfo.transportX, ", ", movementInfo.transportY, ", ", movementInfo.transportZ,
                        ") time=", movementInfo.time);
        }
        // WOWEE_FLOOR_DEBUG: what a jump and a landing tell the server, which
        // reckons fall damage from the world height between them.
        static const bool kFallDebug = [] {
            const char* v = std::getenv("WOWEE_FLOOR_DEBUG");
            return v && v[0] && v[0] != '0';
        }();
        if (kFallDebug && moverIsPlayer &&
            (opcode == Opcode::MSG_MOVE_JUMP || opcode == Opcode::MSG_MOVE_FALL_LAND)) {
            LOG_WARNING("FALL SENT: ", opcode == Opcode::MSG_MOVE_JUMP ? "JUMP" : "FALL_LAND", " z=", movementInfo.z,
                        " fallTime=", movementInfo.fallTime, " transport=", includeTransportInWire ? 1 : 0,
                        " tz=", movementInfo.transportZ, " time=", movementInfo.time);
        } else if (kFallDebug && moverIsPlayer && movementInfo.hasFlag(MovementFlags::FALLING)) {
            LOG_WARNING("FALL SENT: op=0x", std::hex, wireOpcode(opcode), std::dec, " falling z=", movementInfo.z,
                        " fallTime=", movementInfo.fallTime, " vz=", movementInfo.jumpVelocity,
                        " time=", movementInfo.time);
        }
    }

    LOG_DEBUG("Sending movement packet: opcode=0x", std::hex,
              wireOpcode(opcode), std::dec,
              (includeTransportInWire ? " ONTRANSPORT" : ""));

    // Block heartbeats from a near-origin position on Eastern Kingdoms (map 0).
    // These positions are almost certainly bugs (area trigger misfire, corrupted
    // save). Sending them tells the server the player is there, which persists
    // the bad position across sessions and creates a teleport loop.
    if (isCorruptOriginPosition(owner_.getCurrentMapId(),
                               movementInfo.x, movementInfo.y, movementInfo.z)) {
        if (!restoreWorldTransferFallbackIfNearOrigin("sendMovement")) {
            LOG_WARNING("sendMovement: BLOCKED near-origin heartbeat canonical=(",
                        movementInfo.x, ", ", movementInfo.y, ", ", movementInfo.z,
                        ") onTransport=", owner_.isOnTransport(),
                        " transportGuid=0x", std::hex, owner_.playerTransportGuidRef(), std::dec,
                        " flags=0x", std::hex, movementInfo.flags, std::dec);
            return;
        }
    }

    // Where the spline that lets this by has the mover (movementNow).
    if (const MovementInfo now = movementNow(mover); !isPreWotlk() && !taxiClientActive_) {
        movementInfo.x = now.x;
        movementInfo.y = now.y;
        movementInfo.z = now.z;
    }
    if (moverIsPlayer) noteOutgoingPlace(wireOpcode(opcode), movementInfo);

    // Convert canonical → server coordinates for the wire
    MovementInfo wireInfo = movementInfo;
    glm::vec3 serverPos = core::coords::canonicalToServer(glm::vec3(wireInfo.x, wireInfo.y, wireInfo.z));
    wireInfo.x = serverPos.x;
    wireInfo.y = serverPos.y;
    wireInfo.z = serverPos.z;

    // Periodic position audit - log every ~60 heartbeats (~30s) to trace position drift.
    if (opcode == Opcode::MSG_MOVE_HEARTBEAT && ++heartbeatLogCount_ % 60 == 0) {
        LOG_DEBUG("HEARTBEAT #", heartbeatLogCount_, " canonical=(",
                    movementInfo.x, ",", movementInfo.y, ",", movementInfo.z,
                    ") server=(", wireInfo.x, ",", wireInfo.y, ",", wireInfo.z,
                    ") flags=0x", std::hex, movementInfo.flags, std::dec);
    }

    wireInfo.orientation = core::coords::canonicalToServerYaw(wireInfo.orientation);

    if (includeTransportInWire) {
        // Back to the wire's axes the way it came in (serverToTransportLocal):
        // a WMO transport's offset is the wire's already, and swapping it put
        // the player across the deck for the server.
        glm::vec3 serverTransportPos = transportOffsetToWire(
            wireInfo.transportGuid, glm::vec3(wireInfo.transportX, wireInfo.transportY, wireInfo.transportZ));
        wireInfo.transportX = serverTransportPos.x;
        wireInfo.transportY = serverTransportPos.y;
        wireInfo.transportZ = serverTransportPos.z;
    }

    // Build and send movement packet (expansion-specific format)
    auto packet = owner_.getPacketParsers()
        ? owner_.getPacketParsers()->buildMovementPacket(opcode, wireInfo, mover)
        : MovementPacket::build(opcode, wireInfo, mover);
    if (trailer) packet.writeUInt32(*trailer);
    owner_.getSocket()->send(packet);
    // What went out (0x0071f0c0): the facing, the pitch where it can pitch,
    // and whether a turn (+0xa30 0x4000000) or a pitch (0x8000000) is
    // under way since.
    lastSentFacing_ = movementInfo.orientation;
    if (canPitch) lastSentPitch_ = movementInfo.pitch;
    if ((movementInfo.flags & (static_cast<uint32_t>(MovementFlags::TURN_LEFT) |
                               static_cast<uint32_t>(MovementFlags::TURN_RIGHT))) != 0)
        turnStartSent_ = true;
    if ((movementInfo.flags & (static_cast<uint32_t>(MovementFlags::PITCH_UP) |
                               static_cast<uint32_t>(MovementFlags::PITCH_DOWN))) != 0)
        pitchStartSent_ = true;
    // Whether the server was told of the fall (+0xa30 0x80, 0x0071ef80).
    if (!includeTransportInWire) fallSaid_ = movementInfo.hasFlag(MovementFlags::FALLING);
    lastSentPos_ = glm::vec3(movementInfo.x, movementInfo.y, movementInfo.z);
    hasSentMovement_ = true;
    if (movementHeldReason_) {
        LOG_WARNING("Movement: sending again (was held while ", movementHeldReason_, ")");
        movementHeldReason_ = nullptr;
    }

    lastMoveSendMs_ = movementInfo.time;
    // Any other packet takes a held START_TURN, then a held START_PITCH,
    // out after it (0x00721c20).
    if (!heldRoute && !flushingPitchStart_) flushHeldStarts();
}

void MovementHandler::flushHeldStarts() {
    flushingPitchStart_ = true;
    if (heldTurnStart_) {
        const Opcode held = *heldTurnStart_;
        heldTurnStart_.reset();
        sendMovement(held);
    }
    if (heldPitchStart_) {
        const Opcode held = *heldPitchStart_;
        heldPitchStart_.reset();
        sendMovement(held);
    }
    flushingPitchStart_ = false;
}

// ============================================================
// sanitizeMovementForTaxi
// ============================================================

void MovementHandler::sanitizeMovementForTaxi() {
    constexpr uint32_t kClearTaxiFlags =
        static_cast<uint32_t>(MovementFlags::FORWARD) |
        static_cast<uint32_t>(MovementFlags::BACKWARD) |
        static_cast<uint32_t>(MovementFlags::STRAFE_LEFT) |
        static_cast<uint32_t>(MovementFlags::STRAFE_RIGHT) |
        static_cast<uint32_t>(MovementFlags::TURN_LEFT) |
        static_cast<uint32_t>(MovementFlags::TURN_RIGHT) |
        static_cast<uint32_t>(MovementFlags::PITCH_UP) |
        static_cast<uint32_t>(MovementFlags::PITCH_DOWN) |
        static_cast<uint32_t>(MovementFlags::FALLING) |
        static_cast<uint32_t>(MovementFlags::FALLINGFAR) |
        static_cast<uint32_t>(MovementFlags::SWIMMING);

    movementInfo.flags &= ~kClearTaxiFlags;
    movementInfo.fallTime = 0;
    movementInfo.jumpVelocity = 0.0f;
    movementInfo.jumpSinAngle = 0.0f;
    movementInfo.jumpCosAngle = 0.0f;
    movementInfo.jumpXYSpeed = 0.0f;
    movementInfo.pitch = 0.0f;
}

// ============================================================
// forceClearTaxiAndMovementState
// ============================================================

void MovementHandler::forceClearTaxiAndMovementState() {
    taxiActivatePending_ = false;
    taxiActivateTimer_ = 0.0f;
    taxiClientActive_ = false;
    taxiClientPath_.clear();
    taxiServerCompletionPending_ = false;
    taxiRecoverPending_ = false;
    taxiStartGrace_ = 0.0f;
    onTaxiFlight_ = false;

    const bool clearingTaxiMount = taxiMountActive_;
    if (clearingTaxiMount && owner_.mountCallbackRef()) {
        owner_.mountCallbackRef()(0);
    }
    taxiMountActive_ = false;
    // A normal mount is owned by its aura/update-field path. Do not silently
    // zero it here without firing the mount callback: that leaves the rendered
    // mount behind and reconciliation spawns a duplicate. Unstuck explicitly
    // dismounts normal mounts before calling this movement reset.
    if (clearingTaxiMount) {
        owner_.currentMountDisplayIdRef() = 0;
    }
    owner_.vehicleIdRef() = 0;
    // Death/resurrect state is intentionally NOT cleared here.
    // Previously this method reset 10 death-related fields despite being named
    // "forceClearTaxiAndMovementState", which could cancel pending resurrections
    // or clear death state on taxi dismount. Death state is managed by
    // entity_controller (markPlayerDead) and the resurrect packet handlers.

    movementInfo.flags = 0;
    movementInfo.flags2 = 0;
    movementInfo.transportGuid = 0;
    owner_.clearPlayerTransport();

    if (owner_.getSocket() && owner_.getState() == WorldState::IN_WORLD) {
        sendMovement(Opcode::MSG_MOVE_STOP);
        sendMovement(Opcode::MSG_MOVE_STOP_STRAFE);
        sendMovement(Opcode::MSG_MOVE_STOP_TURN);
        sendMovement(Opcode::MSG_MOVE_STOP_SWIM);
        sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    }

    LOG_INFO("Force-cleared taxi/movement state");
}

// ============================================================
// setPosition / setOrientation
// ============================================================

void MovementHandler::setPosition(float x, float y, float z) {
    movementInfo.x = x;
    movementInfo.y = y;
    movementInfo.z = z;
    // The player's own entity goes with it. The server never echoes the
    // player's movement back, so nothing else moved it: it stayed where the
    // player logged in, landed or was teleported, and every distance measured
    // from it - range on the action bar, IsSpellInRange, interact distance,
    // tab targeting - was measured from there.
    //
    // Not during a client taxi flight, where the flight simulation moves the
    // entity and the character is placed from it.
    if (taxiClientActive_ || onTaxiFlight_) return;
    // The unit the client moves goes with it - a driven vehicle, whose
    // passenger the player rides along on it.
    const uint64_t mover = activeMover();
    if (mover != 0 && mover != owner_.getPlayerGuid()) {
        if (auto unit = owner_.getEntityManager().getEntity(mover)) unit->setPosition(x, y, z, movementInfo.orientation);
    }
    // A possessed unit goes without the player, who stays where it stood
    // (the Eye of Kilrogg flies off, the warlock keeps channelling).
    if (moverPossessed()) return;
    if (auto player = owner_.getEntityManager().getEntity(owner_.getPlayerGuid())) {
        // Driving, it is on its seat and not where the vehicle is.
        const glm::vec3 at = seatPlace(owner_.getPlayerGuid()).value_or(glm::vec3(x, y, z));
        player->setPosition(at.x, at.y, at.z, movementInfo.orientation);
    }
}

void MovementHandler::setOrientation(float orientation) {
    movementInfo.orientation = orientation;
}

// ============================================================
// dismount
// ============================================================

void MovementHandler::dismount() {
    if (!owner_.getSocket()) return;
    // The player's mount field goes on reading its old value for a few frames
    // after the request goes out. Believing it re-mounts the player behind their
    // back, and that stale value then makes the server's own SMSG_DISMOUNT look
    // transient and get thrown away - the mount blinked off, back on, and off
    // again over about two hundred milliseconds, with the character caught
    // holding the seated rider pose in between.
    constexpr float kDismountGraceSeconds = 1.5f;
    dismountGraceRemaining_ = kDismountGraceSeconds;
    uint32_t savedMountAura = owner_.mountAuraSpellIdRef();
    if (owner_.currentMountDisplayIdRef() != 0 || taxiMountActive_) {
        if (owner_.mountCallbackRef()) {
            owner_.mountCallbackRef()(0);
        }
        owner_.currentMountDisplayIdRef() = 0;
        taxiMountActive_ = false;
        owner_.mountAuraSpellIdRef() = 0;
        LOG_INFO("Dismount: cleared local mount state, onTaxiFlight=", onTaxiFlight_);
    }
    uint16_t cancelMountWire = wireOpcode(Opcode::CMSG_CANCEL_MOUNT_AURA);
    if (cancelMountWire != 0xFFFF) {
        network::Packet pkt(cancelMountWire);
        owner_.getSocket()->send(pkt);
        LOG_INFO("Sent CMSG_CANCEL_MOUNT_AURA");
    } else if (savedMountAura != 0) {
        auto pkt = CancelAuraPacket::build(savedMountAura);
        owner_.getSocket()->send(pkt);
        LOG_INFO("Sent CMSG_CANCEL_AURA (mount spell ", savedMountAura, ") - Classic fallback");
    } else {
        for (const auto& a : owner_.getPlayerAuras()) {
            if (!a.isEmpty() && a.maxDurationMs < 0 && a.casterGuid == owner_.getPlayerGuid()) {
                auto pkt = CancelAuraPacket::build(a.spellId);
                owner_.getSocket()->send(pkt);
                LOG_INFO("Sent CMSG_CANCEL_AURA (spell ", a.spellId, ") - brute force dismount");
            }
        }
    }
}

// ============================================================
// Force Speed / Root / Flag Change Handlers
// ============================================================

// Shared force-ACK packet builder. All server-forced movement changes (speed,
// root, flag, collision-height, knockback) require the same ACK structure:
// GUID + counter + movement payload with server-space coordinates. Centralised
// here so transport coordinate conversion can't diverge between handlers.
bool MovementHandler::unitForForcedChange(uint64_t guid, const char* name) {
    if (guid != 0 && owner_.getEntityManager().getEntity(guid)) return true;
    // Answered all the same, the answer had the movement of no unit at all:
    // before the first world of a client that is the origin, facing
    // 1.5707964 on the wire, and the server may take it at its word - with
    // the player out of sight of everything until they moved.
    static core::LogBudget budget(20, "Forced movement change");
    if (budget.take()) {
        LOG_WARNING("Forced movement change ", name, " for 0x", std::hex, guid, std::dec, guid == owner_.getPlayerGuid() ? " (the player)" : "",
                    " not answered: the unit is not there, state=", static_cast<int>(owner_.getState()));
    }
    return false;
}

void MovementHandler::noteOutgoingPlace(uint16_t wireOp, const MovementInfo& canonical) {
    const glm::vec3 at(canonical.x, canonical.y, canonical.z);
    constexpr float kFar = 300.0f;
    if (wireOp != wireOpcode(Opcode::CMSG_MOVE_SPLINE_DONE) && knownPlace_ && glm::length(at - *knownPlace_) > kFar) {
        static core::LogBudget budget(20, "Outgoing place");
        if (budget.take()) {
            LOG_WARNING("Outgoing packet 0x", std::hex, wireOp, std::dec, " places the player ",
                        glm::length(at - *knownPlace_), " yd from where the server had them: canonical ", at.x, ", ",
                        at.y, ", ", at.z, " facing ", canonical.orientation, " for ", knownPlace_->x, ", ",
                        knownPlace_->y, ", ", knownPlace_->z);
        }
    }
    knownPlace_ = at;
}

network::Packet MovementHandler::buildForceAck(Opcode ackOpcode, uint32_t counter, uint64_t guid) {
    // The unit the change was for and its movement (0x0071ef80): the
    // player's, or the unit the client moves in its stead.
    if (guid == 0) guid = owner_.getPlayerGuid();
    network::Packet ack(wireOpcode(ackOpcode));
    const bool legacyGuid =
        isActiveExpansion("classic") || isActiveExpansion("tbc") || isActiveExpansion("turtle");
    if (legacyGuid) {
        ack.writeUInt64(guid);
    } else {
        ack.writePackedGuid(guid);
    }
    ack.writeUInt32(counter);

    MovementInfo wire = movementNow(guid);
    wire.time = nextMovementTimestampMs();
    if (guid == owner_.getPlayerGuid()) noteOutgoingPlace(wireOpcode(ackOpcode), wire);
    if (wire.hasFlag(MovementFlags::ONTRANSPORT)) {
        wire.transportTime = wire.time;
        wire.transportTime2 = wire.time;
        // The facing on it as it stands now, as the heartbeat sends it.
        wire.transportO = transportFacingToWire(wire.transportGuid, wire.orientation, wire.transportO);
    }
    glm::vec3 serverPos = core::coords::canonicalToServer(glm::vec3(wire.x, wire.y, wire.z));
    wire.x = serverPos.x;
    wire.y = serverPos.y;
    wire.z = serverPos.z;
    // The facing too, as every other movement the client sends has it: the
    // canonical one went out, a quarter turn off.
    wire.orientation = core::coords::canonicalToServerYaw(wire.orientation);
    if (wire.hasFlag(MovementFlags::ONTRANSPORT)) {
        glm::vec3 serverTransport =
            transportOffsetToWire(wire.transportGuid, glm::vec3(wire.transportX, wire.transportY, wire.transportZ));
        wire.transportX = serverTransport.x;
        wire.transportY = serverTransport.y;
        wire.transportZ = serverTransport.z;
    }
    if (owner_.getPacketParsers()) {
        owner_.getPacketParsers()->writeMovementPayload(ack, wire);
    } else {
        MovementPacket::writeMovementPayload(ack, wire);
    }
    return ack;
}

void MovementHandler::handleForceSpeedChange(network::Packet& packet, const char* name,
                                              Opcode ackOpcode, float* speedStorage,
                                              float MovementSpeeds::* unitMember) {
    const bool fscTbcLike = isPreWotlk();
    uint64_t guid = fscTbcLike
        ? packet.readUInt64() : packet.readPackedGuid();
    uint32_t counter = packet.readUInt32();

    size_t remaining = packet.getRemainingSize();
    if (remaining >= 8) {
        packet.readUInt32();
    } else if (remaining >= 5) {
        packet.readUInt8();
    }
    float newSpeed = packet.readFloat();

    LOG_INFO("SMSG_FORCE_", name, "_CHANGE: guid=0x", std::hex, guid, std::dec,
             " counter=", counter, " speed=", newSpeed);

    if (!unitForForcedChange(guid, name)) return;

    // The player's, or the unit the client moves in its stead (a driven
    // vehicle's): any unit's change lands on its movement (0x00732450 ->
    // 0x007307a0), and the server asks it of the one it moves.
    const bool forPlayer = guid == owner_.getPlayerGuid();
    std::shared_ptr<Entity> unit;
    if (!forPlayer) {
        if (guid != activeMover() || guid == 0) return;
        unit = owner_.getEntityManager().getEntity(guid);
        if (!unit) return;
    }

    // Validate BEFORE sending ACK - if we echo a bad speed back to the server
    // but don't apply it locally, the client and server desync on movement speed.
    if (std::isnan(newSpeed) || newSpeed < 0.1f || newSpeed > 100.0f) {
        LOG_WARNING("Ignoring invalid ", name, " speed: ", newSpeed);
        return;
    }

    if (owner_.getSocket()) {
        auto ack = buildForceAck(ackOpcode, counter, guid);
        ack.writeFloat(newSpeed);
        owner_.getSocket()->send(ack);
    }

    if (forPlayer) {
        if (speedStorage) *speedStorage = newSpeed;
        moverEvent(ackOpcode, movementInfo.flags, movementInfo.jumpVelocity != 0.0f);
    } else if (unitMember) {
        unit->movementSpeeds().*unitMember = newSpeed;
    }
}

void MovementHandler::handleForceRunSpeedChange(network::Packet& packet) {
    // A speed change is not a dismount signal. Mount ownership is updated by
    // SMSG_DISMOUNT / UNIT_FIELD_MOUNTDISPLAYID instead.
    handleForceSpeedChange(packet, "RUN_SPEED", Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK, &serverRunSpeed_,
                               &MovementSpeeds::run);
}

void MovementHandler::handleForceMoveRootState(network::Packet& packet, bool rooted) {
    const bool rootTbc = isPreWotlk();
    if (packet.getRemainingSize() < (rootTbc ? 8u : 2u)) return;
    uint64_t guid = rootTbc
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(4)) return;
    uint32_t counter = packet.readUInt32();

    LOG_INFO(rooted ? "SMSG_FORCE_MOVE_ROOT" : "SMSG_FORCE_MOVE_UNROOT",
             ": guid=0x", std::hex, guid, std::dec, " counter=", counter);

    if (!unitForForcedChange(guid, rooted ? "MOVE_ROOT" : "MOVE_UNROOT")) return;
    MovementInfo* info = movementFor(guid);
    if (!info) return;

    if (rooted) {
        rootMover(*info);
    } else {
        unrootMover(*info);
    }

    if (!owner_.getSocket()) return;
    Opcode ackOp = rooted ? Opcode::CMSG_FORCE_MOVE_ROOT_ACK : Opcode::CMSG_FORCE_MOVE_UNROOT_ACK;
    if (wireOpcode(ackOp) == 0xFFFF) return;
    owner_.getSocket()->send(buildForceAck(ackOp, counter, guid));
    if (guid == owner_.getPlayerGuid()) moverEvent(ackOp, info->flags, info->jumpVelocity != 0.0f);
}

void MovementHandler::handleForceMoveFlagChange(network::Packet& packet, const char* name,
                                                  Opcode ackOpcode, uint32_t flag, bool set) {
    const bool fmfTbcLike = isPreWotlk();
    if (packet.getRemainingSize() < (fmfTbcLike ? 8u : 2u)) return;
    uint64_t guid = fmfTbcLike
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(4)) return;
    uint32_t counter = packet.readUInt32();

    LOG_INFO("SMSG_FORCE_", name, ": guid=0x", std::hex, guid, std::dec, " counter=", counter);

    if (!unitForForcedChange(guid, name)) return;
    MovementInfo* info = movementFor(guid);
    if (!info) return;

    if (flag != 0) {
        if (set) {
            info->flags |= flag;
        } else {
            info->flags &= ~flag;
        }
    }
    // Flying taken away while up in the air ends the flight there and then
    // (0x006ef0d0 calls 0x0098c240 when FLYING is set): FLYING, ascending,
    // descending and the pitch keys all go, and the mover falls. Left behind,
    // FLYING kept the player in the swim/fly pose after .gm fly off - on the
    // way down, on the ground and walking on - as the animation reads it as
    // swimming or flying, and the landing that would have cleared it is only
    // said while CAN_FLY is still there. Cleared before the ack, which
    // carries the flags.
    const uint32_t flying = static_cast<uint32_t>(MovementFlags::FLYING);
    if (!set && flag == static_cast<uint32_t>(MovementFlags::CAN_FLY) && (info->flags & flying)) {
        if (stopFlying(*info) && info == &movementInfo) fallBegan(movementClockMs());
    }

    if (!owner_.getSocket()) return;
    if (wireOpcode(ackOpcode) == 0xFFFF) return;
    owner_.getSocket()->send(buildForceAck(ackOpcode, counter, guid));
}

void MovementHandler::handleMoveSetCollisionHeight(network::Packet& packet) {
    const bool legacyGuid = isPreWotlk();
    if (packet.getRemainingSize() < (legacyGuid ? 8u : 2u)) return;
    uint64_t guid = legacyGuid ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(8)) return;
    uint32_t counter = packet.readUInt32();
    float height = packet.readFloat();

    LOG_INFO("SMSG_MOVE_SET_COLLISION_HGT: guid=0x", std::hex, guid, std::dec,
             " counter=", counter, " height=", height);

    if (guid != owner_.getPlayerGuid() || !unitForForcedChange(guid, "COLLISION_HGT")) return;
    // 0x006ef860 event 0x3a -> 0x006e9600: the body's height (+0xcc) from
    // here on. A mount's comes this way (0x0073d5d0 sets none for a player
    // that mounts in the world: 0x006e9570 with its last argument 0), and a
    // player's own again as it gets off.
    if (height > 0.0f) collisionHeightGiven_ = height;
    if (!owner_.getSocket()) return;

    if (wireOpcode(Opcode::CMSG_MOVE_SET_COLLISION_HGT_ACK) == 0xFFFF) return;
    // buildForceAck now handles transport coordinate conversion, fixing the
    // previous omission that caused desync when riding boats/zeppelins.
    auto ack = buildForceAck(Opcode::CMSG_MOVE_SET_COLLISION_HGT_ACK, counter);
    ack.writeFloat(height);
    owner_.getSocket()->send(ack);
}

void MovementHandler::moverEvent(Opcode op, uint32_t flagsBefore, bool launchedBefore) {
    if (owner_.moverEventCallbackRef()) owner_.moverEventCallbackRef()(op, flagsBefore, launchedBefore);
    // 0x007413f0: the player's stop, a strafe's stop or a root (or unroot)
    // acknowledged, with the player at rest (none of 0xc010ff), takes what a
    // click-to-move left for its arrival (0x0071b1e0).
    if ((op == Opcode::MSG_MOVE_STOP || op == Opcode::MSG_MOVE_STOP_STRAFE ||
         op == Opcode::CMSG_FORCE_MOVE_ROOT_ACK || op == Opcode::CMSG_FORCE_MOVE_UNROOT_ACK) &&
        (movementInfo.flags & 0xc010ffu) == 0)
        owner_.clickToMoveMoverStopped();
}

void MovementHandler::handleMoveKnockBack(network::Packet& packet) {
    const bool mkbTbc = isPreWotlk();
    if (packet.getRemainingSize() < (mkbTbc ? 8u : 2u)) return;
    uint64_t guid = mkbTbc
        ? packet.readUInt64() : packet.readPackedGuid();
    if (!packet.hasRemaining(20)) return;
    uint32_t counter = packet.readUInt32();
    float vcos    = packet.readFloat();
    float vsin    = packet.readFloat();
    float hspeed  = packet.readFloat();
    float vspeed  = packet.readFloat();

    LOG_INFO("SMSG_MOVE_KNOCK_BACK: guid=0x", std::hex, guid, std::dec,
             " counter=", counter, " vcos=", vcos, " vsin=", vsin,
             " hspeed=", hspeed, " vspeed=", vspeed);

    if (guid != owner_.getPlayerGuid() || !unitForForcedChange(guid, "KNOCK_BACK")) return;

    if (owner_.knockBackCallbackRef()) {
        owner_.knockBackCallbackRef()(vcos, vsin, hspeed, vspeed);
    }
    // 0x006e9ff0, before the acknowledgement that carries it and no jump with
    // it: the mover falls with the packet's own jump block (+0xb8 and the
    // direction as they came, the speed +0x8c).
    if (MovementInfo* info = movementFor(guid)) {
        knockBackMover(*info, vcos, vsin, hspeed, vspeed);
        if (info == &movementInfo) {
            isFalling_ = true;
            fallStartMs_ = movementClockMs();
            fallSaid_ = true;
        }
    }

    if (!owner_.getSocket()) return;
    if (wireOpcode(Opcode::CMSG_MOVE_KNOCK_BACK_ACK) == 0xFFFF) return;
    owner_.getSocket()->send(buildForceAck(Opcode::CMSG_MOVE_KNOCK_BACK_ACK, counter));
    moverEvent(Opcode::CMSG_MOVE_KNOCK_BACK_ACK, movementInfo.flags, movementInfo.jumpVelocity != 0.0f);
}

// ============================================================
// Other Player / Creature Movement Handlers
// ============================================================

void MovementHandler::handleMoveSetSpeed(network::Packet& packet) {
    const bool useFull = isPreWotlk();
    uint64_t moverGuid = useFull
        ? packet.readUInt64() : packet.readPackedGuid();

    const size_t remaining = packet.getRemainingSize();
    if (remaining < 4) return;
    if (remaining > 4) {
        packet.setReadPos(packet.getSize() - 4);
    }

    float speed = packet.readFloat();
    if (!std::isfinite(speed) || speed <= 0.01f || speed > 200.0f) return;

    const uint16_t wireOp = packet.getOpcode();
    if (moverGuid != owner_.getPlayerGuid()) {
        // Another player's: kept on its entity for moving it on by its flags
        // (Entity::startMoveByFlags), as the client keeps every unit's speeds
        // in its CMovement.
        auto entity = owner_.getEntityManager().getEntity(moverGuid);
        if (!entity) return;
        MovementSpeeds& speeds = entity->movementSpeeds();
        if      (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_RUN_SPEED))         speeds.run        = speed;
        else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_RUN_BACK_SPEED))    speeds.runBack    = speed;
        else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_WALK_SPEED))        speeds.walk       = speed;
        else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_SWIM_SPEED))        speeds.swim       = speed;
        else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_SWIM_BACK_SPEED))   speeds.swimBack   = speed;
        else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_FLIGHT_SPEED))      speeds.flight     = speed;
        else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_FLIGHT_BACK_SPEED)) speeds.flightBack = speed;
        else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_TURN_RATE))         speeds.turnRate   = speed;
        else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_PITCH_RATE))        speeds.pitchRate  = speed;
        return;
    }
    if      (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_RUN_SPEED))        serverRunSpeed_      = speed;
    else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_RUN_BACK_SPEED))   serverRunBackSpeed_  = speed;
    else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_WALK_SPEED))       serverWalkSpeed_     = speed;
    else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_SWIM_SPEED))       serverSwimSpeed_     = speed;
    else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_SWIM_BACK_SPEED))  serverSwimBackSpeed_ = speed;
    else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_FLIGHT_SPEED))     serverFlightSpeed_   = speed;
    else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_FLIGHT_BACK_SPEED))serverFlightBackSpeed_= speed;
    else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_TURN_RATE))        serverTurnRate_      = speed;
    else if (wireOp == wireOpcode(Opcode::MSG_MOVE_SET_PITCH_RATE))       serverPitchRate_     = speed;
}

void MovementHandler::handleOtherPlayerMovement(network::Packet& packet) {
    // Same packed-guid mismatch found and fixed for MSG_MOVE_TELEPORT_ACK:
    // CMaNGOS sends a packed guid here on TBC too, not a raw UInt64. Reading
    // it as raw corrupted moverGuid, so getEntity(moverGuid) below always
    // missed - the entity still existed (created fine), but its position
    // never updated from these packets. Live-observed as another player's
    // tracked position staying frozen at wherever they were first spotted,
    // even while they kept walking.
    uint64_t moverGuid = packet.readPackedGuid();
    if (moverGuid == owner_.getPlayerGuid() || moverGuid == 0) {
        return;
    }
    // A movement packet lets the unit's move spline go (0x0098ba90).
    moveSplines_.erase(moverGuid);

    MovementInfo info = {};
    info.flags = packet.readUInt32();
    uint8_t flags2Size = owner_.getPacketParsers() ? owner_.getPacketParsers()->movementFlags2Size() : 2;
    if (flags2Size == 2) info.flags2 = packet.readUInt16();
    else if (flags2Size == 1) info.flags2 = packet.readUInt8();
    info.time = packet.readUInt32();
    info.x = packet.readFloat();
    info.y = packet.readFloat();
    info.z = packet.readFloat();
    info.orientation = packet.readFloat();

    const uint32_t wireTransportFlag = owner_.getPacketParsers() ? owner_.getPacketParsers()->wireOnTransportFlag() : 0x00000200;
    const bool onTransport = (info.flags & wireTransportFlag) != 0;
    uint64_t transportGuid = 0;
    float tLocalX = 0, tLocalY = 0, tLocalZ = 0, tLocalO = 0;
    int8_t transportSeat = -1;
    if (onTransport) {
        transportGuid = packet.readPackedGuid();
        tLocalX = packet.readFloat();
        tLocalY = packet.readFloat();
        tLocalZ = packet.readFloat();
        tLocalO = packet.readFloat();
        if (flags2Size >= 1) {
            /*uint32_t transportTime =*/ packet.readUInt32();
        }
        if (flags2Size >= 2) {
            transportSeat = static_cast<int8_t>(packet.readUInt8());
            // MOVEMENTFLAG2_INTERPOLATED_MOVEMENT, as FUN_004f4d40 tests it.
            if (info.flags2 & 0x0400) {
                /*uint32_t transportTime2 =*/ packet.readUInt32();
            }
        }
    }

    // The pitch, the fall time and the jump block after it (FUN_004f4d40):
    // how the unit is falling, and whether it was launched. Read on WotLK
    // only. TBC and Classic gate the jump block on JUMPING (0x2000) and the
    // pitch on other bits, and everything below reads the unit's flags as
    // WotLK's.
    const bool fallRead = !isPreWotlk() && MovementPacket::readPitchAndFall(packet, info);
    const bool fallingNow = (info.flags & static_cast<uint32_t>(MovementFlags::FALLING)) != 0;
    std::optional<float> verticalSpeed;
    if (fallRead) verticalSpeed = fallingNow ? info.jumpVelocity : 0.0f;

    auto entity = owner_.getEntityManager().getEntity(moverGuid);
    if (!entity) {
        return;
    }

    glm::vec3 canonical = core::coords::serverToCanonical(glm::vec3(info.x, info.y, info.z));
    float canYaw = core::coords::serverToCanonicalYaw(info.orientation);

    // The mover's seat, where its transport is a vehicle (0x007489c0): moved
    // through when 0xED brings it (0x00740d30 -> 0x007188f0 sets +0xa30
    // 0x20000000), at once otherwise. Its flags2 say whether it leaves of
    // its own will (0x0074bb60).
    owner_.vehicleRoster().setMoveFlags2(moverGuid, info.flags2);
    owner_.vehicleRoster().setPitch(moverGuid, info.pitch);
    owner_.vehicleRoster().setTransport(moverGuid, onTransport ? transportGuid : 0, transportSeat, tLocalO,
                                        !isPreWotlk() && packet.getOpcode() == wireOpcode(Opcode::MSG_MOVE_UNROOT));

    // A vehicle is a unit, not a transport the manager runs: the mover's
    // position is the world one the packet carries, and composing its seat
    // offset onto an unknown transport put it at the offset itself.
    if (onTransport && transportGuid != 0 && owner_.getTransportManager() &&
        owner_.getTransportManager()->getTransport(transportGuid)) {
        glm::vec3 localCanonical = owner_.getTransportManager()->serverToTransportLocal(
            transportGuid, glm::vec3(tLocalX, tLocalY, tLocalZ));
        // Its facing on it as it came (0x009872c0), turned with it.
        owner_.setTransportAttachment(moverGuid, entity->getType(), transportGuid, localCanonical, true,
                               tLocalO);
        glm::vec3 worldPos = owner_.getTransportManager()->getPlayerWorldPosition(transportGuid, localCanonical);
        canonical = worldPos;
    } else if (!onTransport) {
        owner_.clearTransportAttachment(moverGuid);
    }

    const uint16_t wireOp = packet.getOpcode();
    // Put there, not moved.
    const bool teleported = wireOp == wireOpcode(Opcode::MSG_MOVE_TELEPORT);
    uint32_t durationMs = teleported ? 0 : 120;
    // The mover's own speed: the distance from its last reported position
    // over the time between the two packets' movement timestamps, which the
    // mover stamped and network jitter leaves alone. -1 when unknown.
    float measuredSpeed = -1.0f;
    auto itPrev = otherPlayerMoveTimeMs_.find(moverGuid);
    if (itPrev != otherPlayerMoveTimeMs_.end() && !teleported) {
        uint32_t rawDt = info.time - itPrev->second;
        if (rawDt >= 20 && rawDt <= 2000) {
            const glm::vec3 lastReported(entity->getLatestX(), entity->getLatestY(),
                                         entity->getLatestZ());
            measuredSpeed = glm::length(canonical - lastReported) /
                            (static_cast<float>(rawDt) / 1000.0f);
            float fDt = static_cast<float>(rawDt);
            auto& smoothed = otherPlayerSmoothedIntervalMs_[moverGuid];
            if (smoothed < 1.0f) smoothed = fDt;
            smoothed = 0.7f * smoothed + 0.3f * fDt;
            float clamped = std::max(60.0f, std::min(500.0f, smoothed));
            durationMs = static_cast<uint32_t>(clamped);
        }
    }
    otherPlayerMoveTimeMs_[moverGuid] = info.time;

    const bool isStopOpcode =
        teleported || (wireOp == wireOpcode(Opcode::MSG_MOVE_STOP)) ||
        (wireOp == wireOpcode(Opcode::MSG_MOVE_STOP_STRAFE)) ||
        (wireOp == wireOpcode(Opcode::MSG_MOVE_STOP_TURN)) ||
        (wireOp == wireOpcode(Opcode::MSG_MOVE_STOP_SWIM)) ||
        (wireOp == wireOpcode(Opcode::MSG_MOVE_STOP_SWIM_CHEAT)) ||
        (wireOp == wireOpcode(Opcode::MSG_MOVE_FALL_LAND));

    const float entityDuration = isStopOpcode ? 0.0f : (durationMs / 1000.0f);
    if (teleported) entity->setPosition(canonical.x, canonical.y, canonical.z, canYaw);
    // Still under way or turning: the client moves another player on by the
    // flags of its last packet until the next one changes them - the start
    // packet included, which has no displacement to measure a speed from -
    // at the speeds it holds for that player (CMovement::GetCurrentSpeed,
    // FUN_00987570, and the turn rate FUN_00987770 reads), rather than
    // standing it at each packet's position until the next arrives. The
    // flags decide, not the opcode: a STOP_TURN or a landing with FORWARD
    // still set leaves the player running.
    constexpr uint32_t kUnderWayFlags =
        static_cast<uint32_t>(MovementFlags::FORWARD) |
        static_cast<uint32_t>(MovementFlags::BACKWARD) |
        static_cast<uint32_t>(MovementFlags::STRAFE_LEFT) |
        static_cast<uint32_t>(MovementFlags::STRAFE_RIGHT) |
        static_cast<uint32_t>(MovementFlags::TURN_LEFT) |
        static_cast<uint32_t>(MovementFlags::TURN_RIGHT) |
        static_cast<uint32_t>(MovementFlags::ASCENDING) |
        static_cast<uint32_t>(MovementFlags::DESCENDING);
    const bool underWay = (info.flags & kUnderWayFlags) != 0;
    // Falling, it drops by gravity from the fall the packet carries, up and
    // over on a jump (FUN_00988990, FUN_007618b0) - on a transport WoWee
    // carries it with the transport instead.
    if (fallRead && fallingNow && !onTransport) {
        FallMotion fall;
        fall.fallTimeSec = static_cast<float>(info.fallTime) / 1000.0f;
        fall.verticalSpeed = info.jumpVelocity;
        const glm::vec3 dir = core::coords::serverToCanonical(
            glm::vec3(info.jumpSinAngle, info.jumpCosAngle, 0.0f));
        fall.dirX = dir.x;
        fall.dirY = dir.y;
        fall.horizontalSpeed = info.jumpXYSpeed;
        entity->startMoveByFlags(canonical.x, canonical.y, canonical.z, canYaw,
                                 info.flags, info.flags2, durationMs / 1000.0f, fall);
    } else if (underWay) {
        entity->startMoveByFlags(canonical.x, canonical.y, canonical.z, canYaw,
                                 info.flags, info.flags2, durationMs / 1000.0f);
    } else {
        entity->startMoveTo(canonical.x, canonical.y, canonical.z, canYaw, entityDuration);
        if (measuredSpeed >= 0.0f) entity->setMoveSpeed(measuredSpeed);
    }
    entity->moveBeganAfter(owner_.sinceUnitsAdvanced());

    if (owner_.creatureMoveCallbackRef()) {
        const uint32_t notifyDuration = teleported || (isStopOpcode && !underWay) ? 0u : durationMs;
        owner_.creatureMoveCallbackRef()(moverGuid, canonical.x, canonical.y, canonical.z, notifyDuration);
    }

    if (owner_.unitMoveFlagsCallbackRef()) {
        owner_.unitMoveFlagsCallbackRef()(moverGuid, info.flags, ~0u);
    }

    // The jump this packet starts or ends: FUN_0073ed10 plays JumpStart on
    // MSG_MOVE_JUMP, and the packet that clears FALLING - MSG_MOVE_FALL_LAND,
    // or any other - lands the unit (FUN_0073d2b0).
    const ReportedJump jump =
        entity->reportMoveFlags(info.flags, moveOpcodeKind(wireOp), verticalSpeed);
    if (jump != ReportedJump::None && owner_.unitJumpCallbackRef()) {
        owner_.unitJumpCallbackRef()(moverGuid, jump);
    }
}

void MovementHandler::splineMoveFlags(uint64_t guid, Opcode opcode, uint32_t set, uint32_t mask) {
    if (const auto entity = owner_.getEntityManager().getEntity(guid)) {
        constexpr uint32_t kHover = static_cast<uint32_t>(MovementFlags::HOVER);
        const bool hoveredBefore = (entity->getReportedMoveFlags() & kHover) != 0;
        if (opcode == Opcode::SMSG_SPLINE_MOVE_ROOT) entity->setRooted(onMoveSpline(guid));
        else entity->setMoveFlags(set, mask);
        // 0x006eb060: up as a jump goes, or a fall - before the flag. The
        // ground of a unit that stands is where it stands, of one that hovers
        // the hover height under it (UNIT_FIELD_HOVERHEIGHT, the one after
        // MAXHEALTHMODIFIER).
        const bool hovers = (set & kHover) != 0;
        if ((mask & kHover) != 0 && hovers != hoveredBefore && !entity->isActivelyMoving() && !onMoveSpline(guid)) {
            const uint16_t before = fieldIndex(UF::UNIT_FIELD_MAXHEALTHMODIFIER);
            const float height =
                before == 0xFFFF ? 1.0f : std::bit_cast<float>(entity->getField(static_cast<uint16_t>(before + 1)));
            // 0x009883f0, 0x00988370: not a flying, falling or rooted one, one on a
            // transport, nor one without
            // gravity; from the water faster.
            const uint32_t flags = entity->getReportedMoveFlags();
            const bool held = hovers ? (flags & 0x2001e00u) != 0 : (flags & 0x2201e00u) != 0;
            if (!held && std::isfinite(height) && height > 0.0f && height < 100.0f) {
                if (hovers) entity->fallInPlace(entity->getZ() + height, (flags & 0x200000u) != 0 ? -9.096f : -7.955547f);
                else entity->fallInPlace(entity->getZ() - height, 0.0f);
            }
        }
    }
    if (owner_.unitMoveFlagsCallbackRef()) owner_.unitMoveFlagsCallbackRef()(guid, set, mask);
}

void MovementHandler::handleCompressedMoves(network::Packet& packet) {
    std::vector<uint8_t> decompressedStorage;
    const std::vector<uint8_t>* dataPtr = &packet.getData();

    namespace wire = network::wire;
    const auto& rawData = packet.getData();
    // uint32 inflated size, then a zlib stream (wire_format.hpp).
    const bool hasCompressedWrapper = wire::isZlibStreamAt(rawData, wire::kInflatedSizeBytes);
    if (hasCompressedWrapper) {
        uint32_t decompressedSize = wire::loadLE32(rawData, 0);
        if (decompressedSize == 0 || decompressedSize > 65536) {
            LOG_WARNING("SMSG_COMPRESSED_MOVES: bad decompressedSize=", decompressedSize);
            return;
        }

        const auto zlibStream = std::span<const uint8_t>(rawData).subspan(wire::kInflatedSizeBytes);
        decompressedStorage.resize(decompressedSize);
        uLongf destLen = decompressedSize;
        int ret = uncompress(decompressedStorage.data(), &destLen,
                             zlibStream.data(), static_cast<uLong>(zlibStream.size()));
        if (ret != Z_OK) {
            LOG_WARNING("SMSG_COMPRESSED_MOVES: zlib error ", ret);
            return;
        }

        decompressedStorage.resize(destLen);
        dataPtr = &decompressedStorage;
    }

    const auto& data = *dataPtr;
    const size_t dataLen = data.size();

    uint16_t monsterMoveWire          = wireOpcode(Opcode::SMSG_MONSTER_MOVE);
    uint16_t monsterMoveTransportWire = wireOpcode(Opcode::SMSG_MONSTER_MOVE_TRANSPORT);

    const std::array<uint16_t, 29> kMoveOpcodes = {
        wireOpcode(Opcode::MSG_MOVE_START_FORWARD),
        wireOpcode(Opcode::MSG_MOVE_START_BACKWARD),
        wireOpcode(Opcode::MSG_MOVE_STOP),
        wireOpcode(Opcode::MSG_MOVE_START_STRAFE_LEFT),
        wireOpcode(Opcode::MSG_MOVE_START_STRAFE_RIGHT),
        wireOpcode(Opcode::MSG_MOVE_STOP_STRAFE),
        wireOpcode(Opcode::MSG_MOVE_JUMP),
        wireOpcode(Opcode::MSG_MOVE_START_TURN_LEFT),
        wireOpcode(Opcode::MSG_MOVE_START_TURN_RIGHT),
        wireOpcode(Opcode::MSG_MOVE_STOP_TURN),
        wireOpcode(Opcode::MSG_MOVE_SET_FACING),
        wireOpcode(Opcode::MSG_MOVE_FALL_LAND),
        wireOpcode(Opcode::MSG_MOVE_HEARTBEAT),
        wireOpcode(Opcode::MSG_MOVE_START_SWIM),
        wireOpcode(Opcode::MSG_MOVE_STOP_SWIM),
        wireOpcode(Opcode::MSG_MOVE_SET_WALK_MODE),
        wireOpcode(Opcode::MSG_MOVE_SET_RUN_MODE),
        wireOpcode(Opcode::MSG_MOVE_START_PITCH_UP),
        wireOpcode(Opcode::MSG_MOVE_START_PITCH_DOWN),
        wireOpcode(Opcode::MSG_MOVE_STOP_PITCH),
        wireOpcode(Opcode::MSG_MOVE_START_ASCEND),
        wireOpcode(Opcode::MSG_MOVE_STOP_ASCEND),
        wireOpcode(Opcode::MSG_MOVE_START_DESCEND),
        wireOpcode(Opcode::MSG_MOVE_SET_PITCH),
        wireOpcode(Opcode::MSG_MOVE_GRAVITY_CHNG),
        wireOpcode(Opcode::MSG_MOVE_UPDATE_CAN_FLY),
        wireOpcode(Opcode::MSG_MOVE_UPDATE_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY),
        wireOpcode(Opcode::MSG_MOVE_ROOT),
        wireOpcode(Opcode::MSG_MOVE_UNROOT),
    };

    struct CompressedMoveSubPacket {
        uint16_t opcode = 0;
        std::vector<uint8_t> payload;
    };
    struct DecodeResult {
        bool ok = false;
        bool overrun = false;
        bool usedPayloadOnlySize = false;
        size_t endPos = 0;
        size_t recognizedCount = 0;
        size_t subPacketCount = 0;
        std::vector<CompressedMoveSubPacket> packets;
    };

    auto isRecognizedSubOpcode = [&](uint16_t subOpcode) {
        return subOpcode == monsterMoveWire ||
               subOpcode == monsterMoveTransportWire ||
               std::find(kMoveOpcodes.begin(), kMoveOpcodes.end(), subOpcode) != kMoveOpcodes.end();
    };

    auto decodeSubPackets = [&](bool payloadOnlySize) -> DecodeResult {
        DecodeResult result;
        result.usedPayloadOnlySize = payloadOnlySize;
        size_t pos = 0;
        while (pos < dataLen) {
            if (pos + wire::kMoveSubSizeBytes > dataLen) break;
            uint8_t subSize = data[pos];
            if (subSize == 0) {
                result.ok = true;
                result.endPos = pos + wire::kMoveSubSizeBytes;
                return result;
            }

            const size_t payloadLen = payloadOnlySize
                ? static_cast<size_t>(subSize)
                : (subSize >= wire::kMoveSubOpcodeBytes
                       ? static_cast<size_t>(subSize) - wire::kMoveSubOpcodeBytes : 0);
            if (!payloadOnlySize && subSize < wire::kMoveSubOpcodeBytes) {
                result.endPos = pos;
                return result;
            }

            const size_t packetLen = wire::kMoveSubHeaderBytes + payloadLen;
            if (pos + packetLen > dataLen) {
                result.overrun = true;
                result.endPos = pos;
                return result;
            }

            const uint16_t subOpcode = wire::moveSubOpcodeAt(data, pos);
            const auto payload =
                std::span<const uint8_t>(data).subspan(pos + wire::kMoveSubHeaderBytes, payloadLen);

            CompressedMoveSubPacket subPacket;
            subPacket.opcode = subOpcode;
            subPacket.payload.assign(payload.begin(), payload.end());
            result.packets.push_back(std::move(subPacket));
            ++result.subPacketCount;
            if (isRecognizedSubOpcode(subOpcode)) {
                ++result.recognizedCount;
            }

            pos += packetLen;
        }
        result.ok = (result.endPos == 0 || result.endPos == dataLen);
        result.endPos = dataLen;
        return result;
    };

    DecodeResult decoded = decodeSubPackets(false);
    if (!decoded.ok || decoded.overrun) {
        DecodeResult payloadOnlyDecoded = decodeSubPackets(true);
        const bool preferPayloadOnly =
            payloadOnlyDecoded.ok &&
            (!decoded.ok || decoded.overrun || payloadOnlyDecoded.recognizedCount > decoded.recognizedCount);
        if (preferPayloadOnly) {
            decoded = std::move(payloadOnlyDecoded);
            static uint32_t payloadOnlyFallbackCount = 0;
            ++payloadOnlyFallbackCount;
            if (payloadOnlyFallbackCount <= 10 || (payloadOnlyFallbackCount % 100) == 0) {
                LOG_WARNING("SMSG_COMPRESSED_MOVES decoded via payload-only size fallback",
                            " (occurrence=", payloadOnlyFallbackCount, ")");
            }
        }
    }

    if (!decoded.ok || decoded.overrun) {
        LOG_WARNING("SMSG_COMPRESSED_MOVES: sub-packet overruns buffer at pos=", decoded.endPos);
        return;
    }

    std::unordered_set<uint16_t> unhandledSeen;

    for (const auto& entry : decoded.packets) {
        network::Packet subPacket(entry.opcode, entry.payload);

        if (entry.opcode == monsterMoveWire) {
            handleMonsterMove(subPacket);
        } else if (entry.opcode == monsterMoveTransportWire) {
            handleMonsterMoveTransport(subPacket);
        } else if (owner_.getState() == WorldState::IN_WORLD &&
                   std::find(kMoveOpcodes.begin(), kMoveOpcodes.end(), entry.opcode) != kMoveOpcodes.end()) {
            handleOtherPlayerMovement(subPacket);
        } else {
            if (unhandledSeen.insert(entry.opcode).second) {
                LOG_INFO("SMSG_COMPRESSED_MOVES: unhandled sub-opcode 0x",
                         std::hex, entry.opcode, std::dec, " payloadLen=", entry.payload.size());
            }
        }
    }
}

// ============================================================
// Monster Move Handlers
// ============================================================

void MovementHandler::handleMonsterMove(network::Packet& packet) {
    if (isActiveExpansion("classic") || isActiveExpansion("turtle")) {
        constexpr uint32_t kMaxMonsterMovesPerTick = 256;
        ++monsterMovePacketsThisTick_;
        if (monsterMovePacketsThisTick_ > kMaxMonsterMovesPerTick) {
            ++monsterMovePacketsDroppedThisTick_;
            if (monsterMovePacketsDroppedThisTick_ <= 3 ||
                (monsterMovePacketsDroppedThisTick_ % 100) == 0) {
                LOG_WARNING("SMSG_MONSTER_MOVE: per-tick cap exceeded, dropping packet",
                            " (processed=", monsterMovePacketsThisTick_,
                            " dropped=", monsterMovePacketsDroppedThisTick_, ")");
            }
            return;
        }
    }

    MonsterMoveData data;
    auto logMonsterMoveParseFailure = [&](const std::string& msg) {
        static uint32_t failCount = 0;
        ++failCount;
        if (failCount <= 10 || (failCount % 100) == 0) {
            LOG_WARNING(msg, " (occurrence=", failCount, ")");
        }
    };
    auto logWrappedUncompressedFallbackUsed = [&]() {
        static uint32_t wrappedUncompressedFallbackCount = 0;
        ++wrappedUncompressedFallbackCount;
        if (wrappedUncompressedFallbackCount <= 10 || (wrappedUncompressedFallbackCount % 100) == 0) {
            LOG_WARNING("SMSG_MONSTER_MOVE parsed via uncompressed wrapped-subpacket fallback",
                        " (occurrence=", wrappedUncompressedFallbackCount, ")");
        }
    };
    namespace wire = network::wire;
    // One compressed-moves sub-packet that fills the buffer exactly: its
    // header dropped, the payload kept.
    auto stripWrappedSubpacket = [&](const std::vector<uint8_t>& bytes, std::vector<uint8_t>& stripped) -> bool {
        if (bytes.size() < wire::kMoveSubHeaderBytes) return false;
        uint8_t subSize = bytes[0];
        if (subSize < wire::kMoveSubOpcodeBytes) return false;
        size_t wrappedLen = static_cast<size_t>(subSize) + wire::kMoveSubSizeBytes;
        if (wrappedLen != bytes.size()) return false;
        size_t payloadLen = static_cast<size_t>(subSize) - wire::kMoveSubOpcodeBytes;
        if (wire::kMoveSubHeaderBytes + payloadLen > bytes.size()) return false;
        const auto payload =
            std::span<const uint8_t>(bytes).subspan(wire::kMoveSubHeaderBytes, payloadLen);
        stripped.assign(payload.begin(), payload.end());
        return true;
    };

    const auto& rawData = packet.getData();
    const bool allowTurtleMoveCompression = isActiveExpansion("turtle");
    // uint32 inflated size, then a zlib stream (wire_format.hpp).
    bool isCompressed = allowTurtleMoveCompression &&
                        wire::isZlibStreamAt(rawData, wire::kInflatedSizeBytes);
    if (isCompressed) {
        uint32_t decompSize = wire::loadLE32(rawData, 0);
        if (decompSize == 0 || decompSize > 65536) {
            LOG_WARNING("SMSG_MONSTER_MOVE: bad decompSize=", decompSize);
            return;
        }
        const auto zlibStream = std::span<const uint8_t>(rawData).subspan(wire::kInflatedSizeBytes);
        std::vector<uint8_t> decompressed(decompSize);
        uLongf destLen = decompSize;
        int ret = uncompress(decompressed.data(), &destLen,
                             zlibStream.data(), static_cast<uLong>(zlibStream.size()));
        if (ret != Z_OK) {
            LOG_WARNING("SMSG_MONSTER_MOVE: zlib error ", ret);
            return;
        }
        decompressed.resize(destLen);
        std::vector<uint8_t> stripped;
        bool hasWrappedForm = stripWrappedSubpacket(decompressed, stripped);

        bool parsed = false;
        if (hasWrappedForm) {
            network::Packet wrappedPacket(packet.getOpcode(), stripped);
            if (owner_.getPacketParsers()->parseMonsterMove(wrappedPacket, data)) {
                parsed = true;
            }
        }
        if (!parsed) {
            network::Packet decompPacket(packet.getOpcode(), decompressed);
            if (owner_.getPacketParsers()->parseMonsterMove(decompPacket, data)) {
                parsed = true;
            }
        }

        if (!parsed) {
            if (hasWrappedForm) {
                logMonsterMoveParseFailure("Failed to parse SMSG_MONSTER_MOVE (decompressed " +
                                           std::to_string(destLen) + " bytes, wrapped payload " +
                                           std::to_string(stripped.size()) + " bytes)");
            } else {
                logMonsterMoveParseFailure("Failed to parse SMSG_MONSTER_MOVE (decompressed " +
                                           std::to_string(destLen) + " bytes)");
            }
            return;
        }
    } else if (!owner_.getPacketParsers()->parseMonsterMove(packet, data)) {
        std::vector<uint8_t> stripped;
        if (stripWrappedSubpacket(rawData, stripped)) {
            network::Packet wrappedPacket(packet.getOpcode(), stripped);
            if (owner_.getPacketParsers()->parseMonsterMove(wrappedPacket, data)) {
                logWrappedUncompressedFallbackUsed();
            } else {
                logMonsterMoveParseFailure("Failed to parse SMSG_MONSTER_MOVE");
                return;
            }
        } else {
            logMonsterMoveParseFailure("Failed to parse SMSG_MONSTER_MOVE");
            return;
        }
    }

    // Held while its vehicle plays the seat's exit animation (0x0074c040).
    if (!isPreWotlk() && holdForVehicle(packet, data.guid, 0, 0xff, false)) return;

    // A monster move with no transport takes the unit off any it was on,
    // moved through (0x0073f590 -> 0x0073c8e0 -> 0x006f0c70 with none).
    if (!isPreWotlk() && owner_.vehicleRoster().passenger(data.guid)) {
        owner_.vehicleRoster().setTransport(data.guid, 0, -1, 0.0f, true);
    }

    auto entity = owner_.getEntityManager().getEntity(data.guid);
    if (!entity) {
        return;
    }
    // Its movement flags, and whether a spline still carries it, as the
    // handler finds them (0x0073c8e0); the new move sets them below.
    const bool wasOnSpline = onMoveSpline(data.guid);
    const uint32_t moveFlagsBefore = entity->getReportedMoveFlags();
    const uint32_t moveSplineFlags = isPreWotlk() ? 0u : data.splineFlags;
    // Its move spline (+0xbc, 0x0098c770): a path to run, or a stop.
    noteMoveSpline(data.guid, data.hasDest, moveSplineFlags, data.duration, data.splineId);

    // Classic/TBC creature patrols encode ground locomotion in the spline:
    // RUNMODE set means Run, while a clear bit means Walk. Publish it before
    // the render callback. WotLK uses the separate spline mode opcodes already
    // registered above, and 0x100 means DONE there, so do not reinterpret it.
    const bool usesPreWotlkSplineFlags = isActiveExpansion("classic") ||
                                          isActiveExpansion("turtle") ||
                                          isActiveExpansion("tbc");
    if (usesPreWotlkSplineFlags && entity->getType() == ObjectType::UNIT &&
        owner_.unitMoveFlagsCallbackRef()) {
        const uint32_t locomotionFlags =
            isPreWotlkSplineWalking(data.splineFlags)
                ? static_cast<uint32_t>(MovementFlags::WALKING)
                : 0u;
        owner_.unitMoveFlagsCallbackRef()(data.guid, locomotionFlags, ~0u);
    }

    if (data.hasDest) {
        glm::vec3 destCanonical = core::coords::serverToCanonical(
            glm::vec3(data.destX, data.destY, data.destZ));

        // The facing the move ends on, which the client gives the unit on
        // arriving (FUN_006eb0b0) - FINAL_ANGLE, FINAL_TARGET or
        // FINAL_POINT - and on its way faces it along the path. A target is
        // faced from where the move ends, as it stands now.
        std::optional<float> finalFacing;
        auto facingFromDest = [&](float toX, float toY) -> std::optional<float> {
            const float dx = toX - destCanonical.x;
            const float dy = toY - destCanonical.y;
            if (std::abs(dx) <= 0.01f && std::abs(dy) <= 0.01f) return std::nullopt;
            return std::atan2(-dy, dx);
        };
        if (data.moveType == 4) {
            finalFacing = core::coords::serverToCanonicalYaw(data.facingAngle);
        } else if (data.moveType == 3) {
            if (auto target = owner_.getEntityManager().getEntity(data.facingTarget)) {
                finalFacing = facingFromDest(target->getLatestX(), target->getLatestY());
            }
        } else if (data.moveType == 2) {
            const glm::vec3 spot = core::coords::serverToCanonical(
                glm::vec3(data.facingSpotX, data.facingSpotY, 0.0f));
            finalFacing = facingFromDest(spot.x, spot.y);
        }
        // FUN_0098ca00 leaves the facing alone on a falling or
        // orientation-fixed spline (0x4200) and faces the other way along an
        // inverted one; the pre-WotLK flags are not read for either.
        const bool wotlkSplineFlags = !usesPreWotlkSplineFlags;
        const bool keepsFacing = wotlkSplineFlags &&
            (data.splineFlags & (SplineFlagWotlk::FALLING | SplineFlagWotlk::ORIENT_FIXED)) != 0;
        const bool inverted = wotlkSplineFlags &&
            (data.splineFlags & SplineFlagWotlk::ORIENT_INVERSED) != 0;
        const float facingBefore = entity->getOrientation();
        // What a move too short to launch a spline faces at once.
        const float orientation = finalFacing.value_or(facingBefore);
        // The path's length from where the unit is drawn, through the
        // packet's start, waypoints and destination: one too short starts
        // no spline (0x0073c8e0).
        float pathLength = 0.0f;
        {
            glm::vec3 at(entity->getX(), entity->getY(), entity->getZ());
            auto through = [&](const glm::vec3& next) {
                pathLength += glm::length(next - at);
                at = next;
            };
            const glm::vec3 startCanonical = core::coords::serverToCanonical(glm::vec3(data.x, data.y, data.z));
            if (glm::dot(startCanonical - at, startCanonical - at) >= 0.00077160494f) through(startCanonical);
            for (const auto& wp : data.waypoints)
                through(core::coords::serverToCanonical(glm::vec3(wp.x, wp.y, wp.z)));
            through(destCanonical);
        }

        // Curved only for a FLYING or CATMULLROM spline (FUN_0098c770 sets its
        // mode from 0x42000); a walking unit's runs straight from point to
        // point. The pre-WotLK flags keep the curve.
        const bool smooth = !wotlkSplineFlags ||
            (data.splineFlags & (SplineFlagWotlk::FLYING | SplineFlagWotlk::CATMULLROM)) != 0;
        // A WotLK spline is the client's - a curve by startCurvedPath, a
        // straight one from the packet's start - with or without points
        // between: FUN_0073c8e0 builds it the same for one point.
        const bool clientRoute = wotlkSplineFlags;
        // Build full path: start → waypoints → destination (all in canonical coords)
        if (!data.waypoints.empty() || clientRoute) {
            glm::vec3 startCanonical = core::coords::serverToCanonical(
                glm::vec3(data.x, data.y, data.z));
            std::vector<std::array<float, 3>> path;
            // Path = start + waypoints + dest. Reserve once to avoid the
            // 2–3 reallocations push_back would trigger as it doubles.
            path.reserve(data.waypoints.size() + 2);
            path.push_back({startCanonical.x, startCanonical.y, startCanonical.z});
            for (const auto& wp : data.waypoints) {
                glm::vec3 wpCanonical = core::coords::serverToCanonical(
                    glm::vec3(wp.x, wp.y, wp.z));
                path.push_back({wpCanonical.x, wpCanonical.y, wpCanonical.z});
            }
            path.push_back({destCanonical.x, destCanonical.y, destCanonical.z});
            const std::optional<float> fallTop = entity->reportedFallTop();
            entity->startMoveAlongPath(path, orientation, data.duration / 1000.0f, smooth,
                                       /*clientCurve=*/wotlkSplineFlags,
                                       (moveSplineFlags & SplineFlagWotlk::CYCLIC) != 0,
                                       (moveSplineFlags & SplineFlagWotlk::ENTER_CYCLE) != 0);
            // 0x006e96c0: its arc, over the time the spline takes here.
            if ((moveSplineFlags & SplineFlagWotlk::PARABOLIC) != 0) {
                if (const auto ms = entity->pathDurationMs())
                    entity->setSplineArc(data.verticalAcceleration, data.parabolicStartTime, *ms);
            } else if ((moveSplineFlags & SplineFlagWotlk::FALLING) != 0) {
                entity->setSplineFall(fallTop);
            }
        } else {
            entity->startMoveTo(destCanonical.x, destCanonical.y, destCanonical.z,
                                orientation, data.duration / 1000.0f);
        }
        // From when it came (0x0098c770: +0x128, the clock of the frame it is
        // handled in, 0x006f1490), not from the frame before and not from when
        // a load let it be handled.
        const uint32_t waited = packetWaitedMs(packet);
        entity->moveBeganAfter(owner_.sinceUnitsAdvanced() - static_cast<float>(waited) * 0.001f);
        if (const auto it = moveSplines_.find(data.guid); it != moveSplines_.end()) it->second.startMs -= waited;
        // The spline takes what the client's own does, at its capped speed
        // (0x0073c8e0 hands 0x006eb680 that time, not the packet's); one too
        // short to start is over at once (0x006f11b0).
        if (clientRoute) {
            if (const auto it = moveSplines_.find(data.guid); it != moveSplines_.end()) {
                if (const auto ms = entity->pathDurationMs()) it->second.durationMs = *ms;
                else if (pathLength <= monster_move::kMinSplineLength) it->second.durationMs = 0;
            }
        }
        entity->faceAlongMove(finalFacing,
                              keepsFacing ? std::optional<float>(facingBefore) : std::nullopt,
                              inverted);
        // The target as it stands where the move ends (0x006eb0b0).
        if (data.moveType == 3 && wotlkSplineFlags) {
            entity->faceOnArrival([this, guid = data.facingTarget](const glm::vec3& at) -> std::optional<float> {
                const auto target = owner_.getEntityManager().getEntity(guid);
                if (!target) return std::nullopt;
                const float dx = target->getX() - at.x;
                const float dy = target->getY() - at.y;
                if (std::abs(dx) <= 0.01f && std::abs(dy) <= 0.01f) return std::nullopt;
                return std::atan2(-dy, dx);
            });
        }
        // Forward along it - backward on an inverted one - until it ends,
        // unless it is cyclic (0x006eb680, 0x006eb0b0).
        entity->setMonsterMoveFlags(
            monster_move::afterMonsterMove(moveFlagsBefore, wasOnSpline,
                                           pathLength > monster_move::kMinSplineLength, moveSplineFlags),
            (moveSplineFlags & monster_move::kSplineCyclic) == 0);

        if (owner_.creatureMoveCallbackRef()) {
            owner_.creatureMoveCallbackRef()(data.guid,
                destCanonical.x, destCanonical.y, destCanonical.z,
                data.duration);
        }
    } else if (data.moveType == 1) {
        glm::vec3 posCanonical = core::coords::serverToCanonical(
            glm::vec3(data.x, data.y, data.z));
        entity->setPosition(posCanonical.x, posCanonical.y, posCanonical.z,
                            entity->getOrientation());

        if (owner_.creatureMoveCallbackRef()) {
            owner_.creatureMoveCallbackRef()(data.guid,
                posCanonical.x, posCanonical.y, posCanonical.z, 0);
        }
    } else if (data.moveType == 4) {
        float orientation = core::coords::serverToCanonicalYaw(data.facingAngle);
        glm::vec3 posCanonical = core::coords::serverToCanonical(
            glm::vec3(data.x, data.y, data.z));
        entity->setPosition(posCanonical.x, posCanonical.y, posCanonical.z, orientation);
        if (owner_.creatureMoveCallbackRef()) {
            owner_.creatureMoveCallbackRef()(data.guid,
                posCanonical.x, posCanonical.y, posCanonical.z, 0);
        }
    } else if (data.moveType == 3 && data.facingTarget != 0) {
        auto target = owner_.getEntityManager().getEntity(data.facingTarget);
        if (target) {
            float dx = target->getX() - entity->getX();
            float dy = target->getY() - entity->getY();
            if (std::abs(dx) > 0.01f || std::abs(dy) > 0.01f) {
                float orientation = std::atan2(-dy, dx);
                entity->setOrientation(orientation);
            }
        }
    }
    // The mover's stop is reported as its spline's end is, with the stop's
    // own id (0x006f11b0 -> 0x006eae70 -> 0x0071f210) - a flight the server
    // breaks off among them - and the player is where it says.
    if (!data.hasDest && !isPreWotlk() && data.guid == activeMover()) {
        moveSplines_[data.guid] =
            vehicle::MoveSpline{.startMs = movementClockMs(), .durationMs = 0, .splineId = data.splineId};
        if (data.moveType == 1 && data.guid == owner_.getPlayerGuid()) {
            const glm::vec3 at(entity->getX(), entity->getY(), entity->getZ());
            const glm::vec3 here(movementInfo.x, movementInfo.y, movementInfo.z);
            movementInfo.x = at.x;
            movementInfo.y = at.y;
            movementInfo.z = at.z;
            if (glm::length(at - here) > 0.1f && owner_.playerPositionCorrectionCallbackRef())
                owner_.playerPositionCorrectionCallbackRef()(at.x, at.y, at.z);
        }
    }
    // A stop, or no path: every way it moved stops (0x006f11b0 ->
    // 0x006e9980).
    if (!data.hasDest)
        entity->setMonsterMoveFlags(monster_move::afterMonsterMove(moveFlagsBefore, wasOnSpline, false, 0), false);
}

bool MovementHandler::holdForVehicle(const network::Packet& packet, uint64_t mover, uint64_t transport, uint8_t seat,
                                     bool transportForm) {
    if (replayingVehicleMove_ || mover == 0) return false;
    // 0x0074c040 -> 0x00748230: the one held goes first.
    if (heldVehicleMoves_.count(mover)) releaseHeldVehicleMove(mover);
    const auto& hold = owner_.vehicleMoveHoldCallback();
    if (!hold || !hold(mover, transport, seat)) return false;
    heldVehicleMoves_[mover] = HeldVehicleMove{.opcode = packet.getOpcode(), .data = packet.getData(),
                                                .transportForm = transportForm};
    return true;
}

void MovementHandler::releaseHeldVehicleMove(uint64_t mover) {
    const auto it = heldVehicleMoves_.find(mover);
    if (it == heldVehicleMoves_.end()) return;
    HeldVehicleMove held = std::move(it->second);
    heldVehicleMoves_.erase(it);
    network::Packet packet(held.opcode, std::move(held.data));
    replayingVehicleMove_ = true;
    if (held.transportForm) handleMonsterMoveTransport(packet);
    else handleMonsterMove(packet);
    replayingVehicleMove_ = false;
}

void MovementHandler::handleMonsterMoveTransport(network::Packet& packet) {
    uint64_t moverGuid = 0;
    uint64_t transportGuid = 0;
    if (!isPreWotlk()) {
        // 0x0073f590: the mover's packed guid, the transport's, the seat
        // byte, then the monster move's own toggle byte.
        if (packet.getRemainingSize() < 2 + 2 + 12) return;
        moverGuid = packet.readPackedGuid();
        transportGuid = packet.readPackedGuid();
        if (packet.getRemainingSize() < 2 + 12) return;
        const auto seat = static_cast<int8_t>(packet.readUInt8());
        /*uint8_t toggle =*/ packet.readUInt8();
        // Held while the vehicle plays the seat's enter or exit animation
        // (0x0074c040).
        if (holdForVehicle(packet, moverGuid, transportGuid, static_cast<uint8_t>(seat), true)) {
            packet.skipAll();
            return;
        }
        // Its seat, where the transport is a vehicle (0x0074c040), moved
        // through (0x0073c8e0 sets +0xa30 0x20000000 around it).
        owner_.vehicleRoster().setTransport(moverGuid, transportGuid, seat,
                                            owner_.vehicleRoster().passenger(moverGuid)
                                                ? owner_.vehicleRoster().passenger(moverGuid)->facing
                                                : 0.0f,
                                            true);
    } else {
        if (packet.getRemainingSize() < 8 + 1 + 8 + 12) return;
        moverGuid = packet.readUInt64();
        /*uint8_t unk =*/ packet.readUInt8();
        transportGuid = packet.readUInt64();
    }

    float localX = packet.readFloat();
    float localY = packet.readFloat();
    float localZ = packet.readFloat();

    auto entity = owner_.getEntityManager().getEntity(moverGuid);
    if (!entity) return;

    // A vehicle is a unit and no transport the manager runs: the passenger
    // is where its vehicle is, and drawn in its seat. Composing the seat
    // offset onto an unknown transport put the passenger at the offset.
    if (!owner_.getTransportManager() || !owner_.getTransportManager()->getTransport(transportGuid)) {
        if (auto vehicle = owner_.getEntityManager().getEntity(transportGuid)) {
            entity->setPosition(vehicle->getX(), vehicle->getY(), vehicle->getZ(), entity->getOrientation());
        }
        // Where the move ends in it: the seat's offset.
        endFallForServerMove(moverGuid, true);
        SeatOffset& seat = seatOffsets_[moverGuid];
        seat = {.vehicle = transportGuid, .offset = glm::vec3(localX, localY, localZ)};
        if (packet.hasRemaining(5)) {
            packet.readUInt32();  // the spline's id
            const uint8_t facing = packet.readUInt8();
            const size_t facingBytes = facing == 2 ? 12u : facing == 3 ? 8u : facing == 4 ? 4u : 0u;
            if (facing != 1 && packet.hasRemaining(facingBytes + 4)) {
                packet.setReadPos(packet.getReadPos() + facingBytes);
                const uint32_t flags = packet.readUInt32();
                SplineBlockData spline;
                if (parseMonsterMoveSplineBody(packet, spline, flags, seat.offset, /*useTbcUncompressedMask=*/false,
                                               isPreWotlk() ? SplineFlagSet::PreWotlk : SplineFlagSet::Wotlk) &&
                    spline.hasDest) {
                    seat.offset = glm::vec3(spline.destination.x, spline.destination.y, spline.destination.z);
                }
            }
        }
        packet.skipAll();
        return;
    }

    if (packet.getReadPos() + 5 > packet.getSize()) {
        if (owner_.getTransportManager()) {
            glm::vec3 localCanonical = owner_.getTransportManager()->serverToTransportLocal(
                transportGuid, glm::vec3(localX, localY, localZ));
            owner_.setTransportAttachment(moverGuid, entity->getType(), transportGuid, localCanonical, false, 0.0f);
            glm::vec3 worldPos = owner_.getTransportManager()->getPlayerWorldPosition(transportGuid, localCanonical);
            entity->setPosition(worldPos.x, worldPos.y, worldPos.z, entity->getOrientation());
            if (entity->getType() == ObjectType::UNIT && owner_.creatureMoveCallbackRef())
                owner_.creatureMoveCallbackRef()(moverGuid, worldPos.x, worldPos.y, worldPos.z, 0);
        }
        return;
    }

    const uint32_t transportSplineId = packet.readUInt32();
    uint8_t moveType = packet.readUInt8();

    if (moveType == 1) {
        if (owner_.getTransportManager()) {
            glm::vec3 localCanonical = owner_.getTransportManager()->serverToTransportLocal(
                transportGuid, glm::vec3(localX, localY, localZ));
            owner_.setTransportAttachment(moverGuid, entity->getType(), transportGuid, localCanonical, false, 0.0f);
            glm::vec3 worldPos = owner_.getTransportManager()->getPlayerWorldPosition(transportGuid, localCanonical);
            entity->setPosition(worldPos.x, worldPos.y, worldPos.z, entity->getOrientation());
            if (entity->getType() == ObjectType::UNIT && owner_.creatureMoveCallbackRef())
                owner_.creatureMoveCallbackRef()(moverGuid, worldPos.x, worldPos.y, worldPos.z, 0);
        }
        return;
    }

    float facingAngle = entity->getOrientation();
    std::optional<float> fixedLocalFacing;
    // FINAL_POINT: a place in the transport's frame, as the move's points
    // are. Faced in the world once the move's own end is (below); the wire's
    // x and y went into the canonical facing as they came.
    std::optional<glm::vec3> facingSpotWorld;
    if (moveType == 2) {
        if (packet.getReadPos() + 12 > packet.getSize()) return;
        const float sx = packet.readFloat(), sy = packet.readFloat(), sz = packet.readFloat();
        auto* transports = owner_.getTransportManager();
        facingSpotWorld = transports->getPlayerWorldPosition(
            transportGuid, transports->serverToTransportLocal(transportGuid, glm::vec3(sx, sy, sz)));
    } else if (moveType == 3) {
        if (packet.getReadPos() + 8 > packet.getSize()) return;
        uint64_t tgtGuid = packet.readUInt64();
        if (auto tgt = owner_.getEntityManager().getEntity(tgtGuid)) {
            float dx = tgt->getX() - entity->getX();
            float dy = tgt->getY() - entity->getY();
            if (std::abs(dx) > 0.01f || std::abs(dy) > 0.01f)
                facingAngle = std::atan2(-dy, dx);
        }
    } else if (moveType == 4) {
        // FINAL_ANGLE is a facing on the transport, as every passenger's is
        // (0x006eb0b0 sets the movement's +0x20 to it): the transport's own
        // is added in the world (0x004f42a0). It was taken as a world facing.
        if (packet.getReadPos() + 4 > packet.getSize()) return;
        fixedLocalFacing = packet.readFloat();
        facingAngle = transport_local::worldYaw(
            owner_.getTransportManager()->transportFacing(transportGuid).value_or(0.0f), *fixedLocalFacing);
    }

    if (packet.getReadPos() + 4 > packet.getSize()) return;
    uint32_t splineFlags = packet.readUInt32();

    // Consolidated spline body parser
    SplineBlockData spline;
    if (!parseMonsterMoveSplineBody(packet, spline, splineFlags,
                                    glm::vec3(localX, localY, localZ),
                                    /*useTbcUncompressedMask=*/false,
                                    isPreWotlk() ? SplineFlagSet::PreWotlk
                                                 : SplineFlagSet::Wotlk)) {
        return;
    }
    uint32_t duration = spline.duration;
    float destLocalX = spline.hasDest ? spline.destination.x : localX;
    float destLocalY = spline.hasDest ? spline.destination.y : localY;
    float destLocalZ = spline.hasDest ? spline.destination.z : localZ;
    bool hasDest = spline.hasDest;

    const bool usesPreWotlkSplineFlags = isActiveExpansion("classic") ||
                                          isActiveExpansion("turtle") ||
                                          isActiveExpansion("tbc");
    if (usesPreWotlkSplineFlags && entity->getType() == ObjectType::UNIT &&
        owner_.unitMoveFlagsCallbackRef()) {
        const uint32_t locomotionFlags =
            isPreWotlkSplineWalking(splineFlags)
                ? static_cast<uint32_t>(MovementFlags::WALKING)
                : 0u;
        owner_.unitMoveFlagsCallbackRef()(moverGuid, locomotionFlags, ~0u);
    }

    if (!owner_.getTransportManager()) {
        LOG_WARNING("SMSG_MONSTER_MOVE_TRANSPORT: TransportManager not available for mover 0x",
                    std::hex, moverGuid, std::dec);
        return;
    }

    glm::vec3 startLocalCanonical = owner_.getTransportManager()->serverToTransportLocal(
        transportGuid, glm::vec3(localX, localY, localZ));
    // A facing from `from` towards `to`, both in the world (canonical).
    const auto facingTowards = [&facingAngle](const glm::vec3& from, const glm::vec3& to) {
        const float dx = to.x - from.x;
        const float dy = to.y - from.y;
        if (std::abs(dx) > 0.01f || std::abs(dy) > 0.01f) facingAngle = std::atan2(-dy, dx);
    };
    // Its movement flags as the handler finds them (0x0073c8e0); the move
    // sets them below.
    const bool wasOnSpline = onMoveSpline(moverGuid);
    const uint32_t moveFlagsBefore = entity->getReportedMoveFlags();
    const uint32_t moveSplineFlags = isPreWotlk() ? 0u : splineFlags;
    noteMoveSpline(moverGuid, hasDest && duration > 0, moveSplineFlags, duration, transportSplineId);

    if (hasDest && duration > 0) {
        glm::vec3 destLocalCanonical = owner_.getTransportManager()->serverToTransportLocal(
            transportGuid, glm::vec3(destLocalX, destLocalY, destLocalZ));
        glm::vec3 destWorld  = owner_.getTransportManager()->getPlayerWorldPosition(transportGuid, destLocalCanonical);

        // In the world: a WMO transport's local points are on the wire's
        // axes, which the canonical facing does not take.
        if (moveType == 0) {
            facingTowards(owner_.getTransportManager()->getPlayerWorldPosition(transportGuid, startLocalCanonical),
                          destWorld);
        } else if (facingSpotWorld) {
            facingTowards(destWorld, *facingSpotWorld);
        }

        // A fixed facing is one on the transport at the end of the move as
        // much as at a stop (0x006eb0b0 sets the movement's +0x20 to it on
        // arriving), so the transport's turn meanwhile is in it.
        owner_.setTransportAttachment(moverGuid, entity->getType(), transportGuid, destLocalCanonical,
                                      fixedLocalFacing.has_value(), fixedLocalFacing.value_or(0.0f));
        const float pathLength =
            glm::length(destWorld - glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
        entity->startMoveTo(destWorld.x, destWorld.y, destWorld.z, facingAngle, duration / 1000.0f);
        const uint32_t waited = packetWaitedMs(packet);
        entity->moveBeganAfter(owner_.sinceUnitsAdvanced() - static_cast<float>(waited) * 0.001f);
        if (const auto it = moveSplines_.find(moverGuid); it != moveSplines_.end()) it->second.startMs -= waited;
        // Run on the transport, from where the mover is on it now.
        if (entity->isActivelyMoving()) {
            if (const auto from = owner_.getTransportManager()->worldToTransportLocal(
                    transportGuid, glm::vec3(entity->getX(), entity->getY(), entity->getZ()))) {
                owner_.transportAttachmentsRef()[moverGuid].moveFromLocal = *from;
            }
        }
        // Along the path, then the facing asked for on arriving - as
        // handleMonsterMove does.
        entity->faceAlongMove(moveType >= 2 ? std::optional<float>(facingAngle) : std::nullopt,
                              std::nullopt, false);
        // Forward along it until it ends, as handleMonsterMove leaves them.
        entity->setMonsterMoveFlags(
            monster_move::afterMonsterMove(moveFlagsBefore, wasOnSpline,
                                           pathLength > monster_move::kMinSplineLength, moveSplineFlags),
            (moveSplineFlags & monster_move::kSplineCyclic) == 0);

        if (entity->getType() == ObjectType::UNIT && owner_.creatureMoveCallbackRef())
            owner_.creatureMoveCallbackRef()(moverGuid, destWorld.x, destWorld.y, destWorld.z, duration);

        LOG_DEBUG("SMSG_MONSTER_MOVE_TRANSPORT: mover=0x", std::hex, moverGuid,
                  " transport=0x", transportGuid, std::dec,
                  " dur=", duration, "ms dest=(", destWorld.x, ",", destWorld.y, ",", destWorld.z, ")");
    } else {
        glm::vec3 startWorld = owner_.getTransportManager()->getPlayerWorldPosition(transportGuid, startLocalCanonical);
        if (facingSpotWorld) facingTowards(startWorld, *facingSpotWorld);
        // A fixed facing stays on the transport, turning with it.
        owner_.setTransportAttachment(moverGuid, entity->getType(), transportGuid, startLocalCanonical,
                                      fixedLocalFacing.has_value(), fixedLocalFacing.value_or(0.0f));
        entity->setPosition(startWorld.x, startWorld.y, startWorld.z, facingAngle);
        // A stop: every way it moved stops (0x006f11b0 -> 0x006e9980).
        entity->setMonsterMoveFlags(monster_move::afterMonsterMove(moveFlagsBefore, wasOnSpline, false, 0), false);
        if (entity->getType() == ObjectType::UNIT && owner_.creatureMoveCallbackRef())
            owner_.creatureMoveCallbackRef()(moverGuid, startWorld.x, startWorld.y, startWorld.z, 0);
    }
}

// ============================================================
// Teleport Handlers
// ============================================================

void MovementHandler::handleTeleportAck(network::Packet& packet) {
    if (packet.getRemainingSize() < 1u) {
        LOG_WARNING("MSG_MOVE_TELEPORT_ACK too short");
        return;
    }

    // CMaNGOS sends a packed guid here on every expansion, including TBC -
    // confirmed by live packet capture: reading a raw UInt64 instead
    // misaligned every field after it, so the guid never matched the local
    // player and the position update was silently dropped as "remote entity".
    uint64_t guid = packet.readPackedGuid();
    if (!packet.hasRemaining(4)) return;
    uint32_t counter = packet.readUInt32();

    // Three layouts, not two.
    //
    // WotLK writes moveFlags2 as a uint16. TBC writes it as a single byte -
    // that is the "unknown byte, constant 0x04" a live capture found here, and
    // reading it is what made a ground-truth teleport (.go xyz to
    // ironforge-city) decode to the expected x/y/z. Vanilla has no moveFlags2
    // field at all.
    //
    // isPreWotlk() is true for vanilla and TBC alike, so gating the byte on it
    // read one byte too many on vanilla and shifted x, y, z and orientation by
    // a byte each. The coordinates that came out were garbage derived from the
    // same misalignment every time, which is why .tele to three different
    // places all arrived somewhere in Stonetalon. Reported as issue 113.
    const bool taNoFlags2 = isPreWotlk();          // no uint16 moveFlags2
    const bool taByteFlags2 = isActiveExpansion("tbc");  // ...but TBC has a uint8
    const size_t minMoveSz = taNoFlags2
        ? (4 + (taByteFlags2 ? 1u : 0u) + 4 + 4 * 4)
        : (4 + 2 + 4 + 4 * 4);
    if (packet.getRemainingSize() < minMoveSz) {
        LOG_WARNING("MSG_MOVE_TELEPORT_ACK: not enough data for movement info");
        return;
    }

    const uint32_t teleportMoveFlags = packet.readUInt32();
    if (!taNoFlags2)
        packet.readUInt16();  // moveFlags2 (WotLK only)
    uint32_t moveTime = packet.readUInt32();
    if (taByteFlags2)
        packet.readUInt8();  // TBC's uint8 moveFlags2 (see comment above)
    float serverX = packet.readFloat();
    float serverY = packet.readFloat();
    float serverZ = packet.readFloat();
    float orientation = packet.readFloat();
    // The movement it carries says whether the place is on a transport: the
    // transport's guid and the offset on it follow the position when it is.
    const bool teleportOnTransport =
        (teleportMoveFlags & static_cast<uint32_t>(MovementFlags::ONTRANSPORT)) != 0;
    uint64_t teleportTransportGuid = 0;
    glm::vec3 teleportTransportOffset(0.0f);
    if (teleportOnTransport && !taNoFlags2 && packet.hasRemaining(1)) {
        teleportTransportGuid = packet.readPackedGuid();
        if (packet.hasRemaining(16)) {
            teleportTransportOffset.x = packet.readFloat();
            teleportTransportOffset.y = packet.readFloat();
            teleportTransportOffset.z = packet.readFloat();
            packet.readFloat();  // its facing on the transport
        } else {
            teleportTransportGuid = 0;
        }
    }

    LOG_WARNING("MSG_MOVE_TELEPORT_ACK: guid=0x", std::hex, guid, std::dec,
                " counter=", counter,
                " pos=(", serverX, ", ", serverY, ", ", serverZ, ")",
                " currentPos=(", movementInfo.x, ", ", movementInfo.y, ", ", movementInfo.z, ")");

    // The player's, while it moves another unit: the place is the player's and
    // not that unit's, which movementInfo is the movement of. The server ends
    // what it moves the unit by, so the player moves itself again here already.
    if (guid == owner_.getPlayerGuid() && activeMover() != 0 && activeMover() != guid) setActiveMover(guid);

    if (guid != owner_.getPlayerGuid()) {
        glm::vec3 canonical = core::coords::serverToCanonical(glm::vec3(serverX, serverY, serverZ));
        auto entity = owner_.getEntityManager().getEntity(guid);
        if (entity) {
            entity->setPosition(canonical.x, canonical.y, canonical.z,
                                core::coords::serverToCanonicalYaw(orientation));
        }
        LOG_INFO("MSG_MOVE_TELEPORT_ACK for remote entity 0x", std::hex, guid, std::dec, " - ignored for local player");
        return;
    }

    glm::vec3 canonical = core::coords::serverToCanonical(glm::vec3(serverX, serverY, serverZ));

    // Put somewhere else: a click-to-move is over (0x0072d1b0 -> 0x007272c0).
    owner_.stopClickToMove();

    // A destination that is the map origin itself, which is a server-side
    // area-trigger misfire rather than anywhere anyone goes.
    //
    // This used to be a two-thousand-yard box - |x| and |y| under a thousand
    // on map 0 - and returned from here without acknowledging. Both halves
    // were wrong, and together they are what desynced the player from the
    // server for the rest of the session.
    //
    // The box: canonical swaps x and y off the wire, so that test was a square
    // two thousand yards on a side centred on the origin of Eastern Kingdoms,
    // and Hillsbrad Foothills sits on it. AzerothCore's own game_tele puts
    // Southshore at -853, -533 - inside it. So the hearthstone to the
    // Southshore inn, the Southshore graveyard and the inn's own tavern
    // trigger were all "near-origin misfires".
    //
    // The silence: the server sets a teleport semaphore when it sends this and
    // clears it when the acknowledgement comes back. Until then
    // WorldSession::HandleMovementOpcodes returns at its first line and throws
    // away every movement packet we send. So the note this replaces - that
    // heartbeats from the real position would eventually convince the server -
    // had it exactly backwards: no heartbeat could be heard at all. The client
    // walked on from where it thought it was, the server kept the player where
    // it had put them, no creatures arrived anywhere near the player, and a
    // relog put them back at the server's copy.
    //
    // The acknowledgement is now unconditional. Only the position is refused,
    // and only for a destination within a few yards of the origin, which no
    // legitimate teleport is.
    constexpr float kOriginMisfireRadius = 50.0f;
    const bool originMisfire = owner_.getCurrentMapId() == 0 &&
        std::abs(canonical.x) < kOriginMisfireRadius &&
        std::abs(canonical.y) < kOriginMisfireRadius;

    if (originMisfire) {
        LOG_WARNING("MSG_MOVE_TELEPORT to the map origin canonical=(",
                    canonical.x, ", ", canonical.y, ", ", canonical.z,
                    ") - acknowledging so the server stops discarding our "
                    "movement, then telling it where we really are");
    } else {
        // Where the move took the player from, on the wire: a passenger moved
        // along a one-map route (below) crossed from the leg ending there.
        const glm::vec3 leftFrom =
            core::coords::canonicalToServer(glm::vec3(movementInfo.x, movementInfo.y, movementInfo.z));
        movementInfo.x = canonical.x;
        movementInfo.y = canonical.y;
        movementInfo.z = canonical.z;
        movementInfo.orientation = core::coords::serverToCanonicalYaw(orientation);
        // What the mover is doing ends here; what an aura gives it does not,
        // nor walking, a root or no gravity (placeMover). The server does not
        // send them again, and water walking went at every .go with the buff
        // still on.
        placeMover(movementInfo, endFallTimer());
        // Off the spline that carried it (0x0098b730 lets it go, unreported),
        // and a flight's camera follows the unit: it is here now as well.
        moveSplines_.erase(guid);
        forgetTaxi();
        noteServerPlace(canonical);
        if (auto self = owner_.getEntityManager().getEntity(guid)) {
            self->setPosition(canonical.x, canonical.y, canonical.z, movementInfo.orientation);
        }
        // Off whatever transport it was on, or onto the one the teleport
        // names. Kept, the old one went on placing the player by its offset
        // on it, so a .go away from a lift left them riding it.
        auto* transports = owner_.getTransportManager();
        owner_.forgetPendingPlayerTransport();
        if (teleportTransportGuid != 0 && transports &&
            transports->getTransport(teleportTransportGuid)) {
            owner_.setPlayerOnTransport(
                teleportTransportGuid,
                transports->serverToTransportLocal(teleportTransportGuid, teleportTransportOffset));
            // The transport's loading screen, as the client shows it when the
            // leg of the transport it carries changes (0x007134a0 ->
            // 0x0040ae30): used where the move is far enough to load the
            // world (WorldEntryCallbackHandler), let go where it is not.
            if (const auto* carried = transports->getTransport(teleportTransportGuid)) {
                owner_.armTransportLegCrossing(carried->entry, owner_.getCurrentMapId(),
                                               glm::vec2(leftFrom.x, leftFrom.y));
            }
        } else if (owner_.isOnTransport()) {
            owner_.clearPlayerTransport();
        }
    }

    // Clear cast bar on teleport - SpellHandler owns the casting_ flag
    if (owner_.getSpellHandler()) owner_.getSpellHandler()->resetCastState();

    // The acknowledgement waits for the place to be in. The client answers
    // from the same handler, after 0x006ecf80 has moved the player and read
    // in the area about the new place behind the loading screen
    // (0x006ed0f0: 0x006ecf80 -> 0x0074b620, then 0x007413f0 with 0xC7).
    // Sent first, the server took the player as arrived - creatures, area
    // triggers, the next teleport - while the ground was still loading.
    // The world-entry callback holds it when it queues that load
    // (WorldLoader::relocateOnLoadedMap lets it go); otherwise it goes now.
    if (const auto earlier = teleportAck_.arrive(counter, moveTime)) {
        // One from a teleport not yet answered: the server waits for it.
        LOG_WARNING("MSG_MOVE_TELEPORT_ACK: earlier acknowledgement still held - sent now");
        sendTeleportAck(*earlier);
    }

    // Only where the destination was taken. A misfire leaves the player where
    // they are, so there is no world entry to announce.
    if (!originMisfire && owner_.worldEntryCallbackRef()) {
        owner_.worldEntryCallbackRef()(owner_.currentMapIdRef(), serverX, serverY, serverZ, false);
    }
    if (const auto ack = teleportAck_.settle()) sendTeleportAck(*ack);
    if (originMisfire) sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
}

void MovementHandler::sendWorldportAck() {
    if (!owner_.getSocket()) return;
    network::Packet ack(wireOpcode(Opcode::MSG_MOVE_WORLDPORT_ACK));
    owner_.getSocket()->send(ack);
    LOG_INFO("Sent MSG_MOVE_WORLDPORT_ACK");
    owner_.askUnansweredAgain();
}

bool MovementHandler::releaseWorldportAck() {
    if (!worldportAck_.release()) return false;
    sendWorldportAck();
    return true;
}

bool MovementHandler::releaseTeleportAck() {
    const auto ack = teleportAck_.release();
    if (!ack) return false;
    sendTeleportAck(*ack);
    return true;
}

void MovementHandler::sendTeleportAck(const TeleportAckGate::Ack& pending) {
    if (!owner_.getSocket()) return;
    network::Packet ack(wireOpcode(Opcode::MSG_MOVE_TELEPORT_ACK));
    const bool legacyGuidAck =
        isActiveExpansion("classic") || isActiveExpansion("tbc") || isActiveExpansion("turtle");
    if (legacyGuidAck) {
        ack.writeUInt64(owner_.getPlayerGuid());
    } else {
        ack.writePackedGuid(owner_.getPlayerGuid());
    }
    ack.writeUInt32(pending.counter);
    ack.writeUInt32(pending.moveTime);
    owner_.getSocket()->send(ack);
    // Nothing follows it but what the keys still held start again
    // (0x007413f0, 0xC7: 0x005f95e0, 0x005fbbc0): a stop and a heartbeat sent
    // here told of a mover that was running on.
    LOG_WARNING("Sent MSG_MOVE_TELEPORT_ACK counter=", pending.counter);
    // The same event asks the base animation again (0x007413f0 -> 0x0073ed10, 0xC7).
    moverEvent(Opcode::MSG_MOVE_TELEPORT_ACK, movementInfo.flags, false);
}

// SMSG_TRANSFER_PENDING: the client's handler is 0x00401480.
// The map, and the transport entry and the map it leaves from only when the
// player goes with a transport (a ship that crosses to another map). A
// teleport from a transport has none: the server took the player off it.
void MovementHandler::handleTransferPending(network::Packet& packet) {
    transferPendingOnTransport_ = false;
    if (!packet.hasRemaining(4)) { packet.skipAll(); return; }
    const uint32_t pendingMapId = packet.readUInt32();
    if (packet.hasRemaining(8)) {
        const uint32_t transportEntry = packet.readUInt32();
        const uint32_t transportMapId = packet.readUInt32();
        transferPendingOnTransport_ = true;
        transferPendingTransportEntry_ = transportEntry;
        transferPendingTransportMapId_ = transportMapId;
        LOG_INFO("SMSG_TRANSFER_PENDING: map ", pendingMapId, " on transport ", transportEntry,
                 " from map ", transportMapId);
    }
    packet.skipAll();
}

void MovementHandler::handleNewWorld(network::Packet& packet) {
    if (!packet.hasRemaining(20)) {
        LOG_WARNING("SMSG_NEW_WORLD too short");
        return;
    }

    uint32_t mapId = packet.readUInt32();
    float serverX = packet.readFloat();
    float serverY = packet.readFloat();
    float serverZ = packet.readFloat();
    float orientation = packet.readFloat();
    // Leaving the world ends a click-to-move (0x00528f00 -> 0x007272c0).
    owner_.stopClickToMove();
    forgetCollisionHeight();
    // A ride to another map is one the server said goes with the transport
    // (SMSG_TRANSFER_PENDING). Anything else - a GM teleport, a hearthstone,
    // a portal from a tram car - is a world position, and the player is
    // off the transport: the client destroys the player object on every
    // SMSG_NEW_WORLD (0x00403b70 -> 0x004d6750) and only a transport block
    // puts it back on one. Kept on, the next map's transport with the same
    // guid took the player: a teleport to Thunder Bluff from the Deeprun
    // Tram landed them on a Kalimdor lift, in Dustwallow Marsh.
    const bool travelsWithTransport = transferPendingOnTransport_;
    transferPendingOnTransport_ = false;
    const bool crossMapTransportTransfer = travelsWithTransport && owner_.isOnTransport() &&
        mapId != owner_.getCurrentMapId();
    if (!crossMapTransportTransfer) owner_.forgetPendingPlayerTransport();
    if (!crossMapTransportTransfer && owner_.isOnTransport()) {
        owner_.clearPlayerTransport();
        owner_.playerTransportStickyGuidRef() = 0;
        owner_.playerTransportStickyTimerRef() = 0.0f;
    }
    if (crossMapTransportTransfer) {
        // While riding a cross-continent transport, SMSG_NEW_WORLD carries the
        // player's transport-local coordinates. They are not a world position
        // near map origin. Hold them until the same ship is registered on the
        // destination map, then compose them through its new transform.
        // The wire's offset: the ship's kind on the new map decides its
        // axes (TransportManager::serverToTransportLocal).
        owner_.beginPlayerTransportWorldTransfer(mapId, glm::vec3(serverX, serverY, serverZ));
        // 0x0040ad50: the loading screen draws the crossing.
        owner_.armLoadingScreenCrossing(transferPendingTransportEntry_, transferPendingTransportMapId_);
    } else {
        owner_.armLoadingScreenCrossing(0, 0);
    }
    const uint32_t transferAreaTriggerId = lastAreaTriggerId_;
    lastAreaTriggerId_ = 0;
    const bool hasPendingAreaTriggerDestination = pendingAreaTriggerDestinationValid_;
    const uint32_t pendingAreaTriggerDestinationMapId = pendingAreaTriggerDestinationMapId_;
    const glm::vec3 pendingAreaTriggerDestinationServerPos = pendingAreaTriggerDestinationServerPos_;
    const float pendingAreaTriggerDestinationServerO = pendingAreaTriggerDestinationServerO_;
    pendingAreaTriggerDestinationValid_ = false;
    const auto* knownTransferDestination = findKnownAreaTriggerDestination(transferAreaTriggerId, mapId);

    bool transferFallbackValid = false;
    glm::vec3 transferFallbackServerPos(0.0f);
    float transferFallbackServerO = orientation;
    if (hasPendingAreaTriggerDestination && pendingAreaTriggerDestinationMapId == mapId) {
        transferFallbackValid = true;
        transferFallbackServerPos = pendingAreaTriggerDestinationServerPos;
        transferFallbackServerO = pendingAreaTriggerDestinationServerO;
    } else if (knownTransferDestination) {
        transferFallbackValid = true;
        transferFallbackServerPos =
            glm::vec3(knownTransferDestination->serverX,
                      knownTransferDestination->serverY,
                      knownTransferDestination->serverZ);
        transferFallbackServerO = knownTransferDestination->serverO;
    }
    if (transferFallbackValid) {
        worldTransferFallbackValid_ = true;
        worldTransferFallbackMapId_ = mapId;
        worldTransferFallbackTriggerId_ = transferAreaTriggerId;
        worldTransferFallbackCanonicalPos_ = core::coords::serverToCanonical(transferFallbackServerPos);
        worldTransferFallbackCanonicalO_ = core::coords::serverToCanonicalYaw(transferFallbackServerO);
        orientation = transferFallbackServerO;
        LOG_WARNING("World transfer fallback armed: trigger=", transferAreaTriggerId,
                    " map=", mapId,
                    " canonical=(",
                    worldTransferFallbackCanonicalPos_.x, ", ",
                    worldTransferFallbackCanonicalPos_.y, ", ",
                    worldTransferFallbackCanonicalPos_.z, ")");
    } else {
        worldTransferFallbackValid_ = false;
    }

    LOG_INFO("SMSG_NEW_WORLD: mapId=", mapId,
             " pos=(", serverX, ", ", serverY, ", ", serverZ, ")",
             " orient=", orientation);

    glm::vec3 receivedCanonical = core::coords::serverToCanonical(glm::vec3(serverX, serverY, serverZ));
    const bool badEasternKingdomsNearOrigin =
        isCorruptOriginPosition(mapId, receivedCanonical.x, receivedCanonical.y,
                                receivedCanonical.z);
    if (badEasternKingdomsNearOrigin) {
        if (hasPendingAreaTriggerDestination && pendingAreaTriggerDestinationMapId == mapId) {
            LOG_WARNING("Correcting bad SMSG_NEW_WORLD near-origin destination for area trigger ",
                        transferAreaTriggerId,
                        ": received server=(", serverX, ", ", serverY, ", ", serverZ,
                        ") using pending server=(", pendingAreaTriggerDestinationServerPos.x, ", ",
                        pendingAreaTriggerDestinationServerPos.y, ", ",
                        pendingAreaTriggerDestinationServerPos.z, ")");
            serverX = pendingAreaTriggerDestinationServerPos.x;
            serverY = pendingAreaTriggerDestinationServerPos.y;
            serverZ = pendingAreaTriggerDestinationServerPos.z;
            orientation = pendingAreaTriggerDestinationServerO;
            receivedCanonical = core::coords::serverToCanonical(glm::vec3(serverX, serverY, serverZ));
        } else if (knownTransferDestination) {
            LOG_WARNING("Correcting bad SMSG_NEW_WORLD near-origin destination for area trigger ",
                        transferAreaTriggerId,
                        ": received server=(", serverX, ", ", serverY, ", ", serverZ,
                        ") using server=(", knownTransferDestination->serverX, ", ",
                        knownTransferDestination->serverY, ", ", knownTransferDestination->serverZ, ")");
            serverX = knownTransferDestination->serverX;
            serverY = knownTransferDestination->serverY;
            serverZ = knownTransferDestination->serverZ;
            orientation = knownTransferDestination->serverO;
            receivedCanonical = core::coords::serverToCanonical(glm::vec3(serverX, serverY, serverZ));
        } else {
            LOG_WARNING("SMSG_NEW_WORLD near-origin destination on map 0 without known area trigger fallback: trigger=",
                        transferAreaTriggerId,
                        " server=(", serverX, ", ", serverY, ", ", serverZ, ")");
        }
    }

    // Every SMSG_NEW_WORLD, the same map's too (0x00403b70): a spell held on
    // the cursor for a place or a unit of the old world does not wait in the
    // new one, and the spell histories start again.
    if (owner_.getSpellHandler()) owner_.getSpellHandler()->newWorld();
    // And the player object goes with every other (0x004d6750), which is the
    // client leaving the world: windows, timers, remembered units
    // (0x006e5090 -> 0x00528c30).
    owner_.onLeaveWorld(/*worldport=*/true);
    // The screen effect goes back to none (0x004f7020(0)); the player's row
    // comes back with the player (0x004f88b0), its fade-in from the start.
    if (auto* renderer = owner_.services().renderer) renderer->resetScreenEffect();
    // The loot rolls not yet answered are passed (0x006fafd0).
    if (auto* inventory = owner_.getInventoryHandler()) inventory->passLootRollsForWorldport();
    // The party's finder dungeon, ungrouped and off its map, is forgotten
    // (0x0052cc30) - against the map arrived on, which 0x004d7750 has made
    // current by then.
    owner_.forgetLfgDungeonOnWorldport(mapId);

    const bool isSameMap       = (mapId == owner_.currentMapIdRef());
    const bool isResurrection  = owner_.resurrectPendingRef();
    if (isSameMap && isResurrection) {
        LOG_INFO("SMSG_NEW_WORLD same-map resurrection - skipping world reload");

        glm::vec3 canonical = receivedCanonical;
        movementInfo.x = canonical.x;
        movementInfo.y = canonical.y;
        movementInfo.z = canonical.z;
        movementInfo.orientation = core::coords::serverToCanonicalYaw(orientation);
        movementInfo.flags  = 0;
        movementInfo.flags2 = 0;

        owner_.resurrectPendingRef()       = false;
        owner_.resurrectRequestPendingRef() = false;
        owner_.releasedSpiritRef()         = false;
        owner_.playerDeadRef()             = false;
        owner_.repopPendingRef()           = false;
        owner_.pendingSpiritHealerGuidRef() = 0;
        owner_.resurrectCasterGuidRef()    = 0;
        owner_.forgetCorpsePlace();
        owner_.forgetDeathRelease();
        owner_.corpseGuidRef()             = 0;
        owner_.clearHostileAttackers();
        owner_.stopAutoAttack();
        owner_.tabCycleStaleRef() = true;
        owner_.resetCastState();

        if (owner_.getSocket()) {
            network::Packet ack(wireOpcode(Opcode::MSG_MOVE_WORLDPORT_ACK));
            owner_.getSocket()->send(ack);
            LOG_INFO("Sent MSG_MOVE_WORLDPORT_ACK (resurrection)");
        }
        return;
    }

    owner_.currentMapIdRef() = mapId;
    owner_.inInstanceRef() = false;
    // Another map: the battlefield's opponents are behind (0x0054ca50).
    owner_.clearArenaRoster();
    if (owner_.getSocket()) {
        owner_.getSocket()->tracePacketsFor(std::chrono::seconds(12), "new_world");
    }

    glm::vec3 canonical = receivedCanonical;
    movementInfo.x = canonical.x;
    movementInfo.y = canonical.y;
    movementInfo.z = canonical.z;
    movementInfo.orientation = core::coords::serverToCanonicalYaw(orientation);
    movementInfo.flags = 0;
    movementInfo.flags2 = 0;
    serverMovementAllowed_ = true;
    owner_.resurrectPendingRef() = false;
    owner_.resurrectRequestPendingRef() = false;
    // A flight on the map left behind is over here; one that goes on has its
    // flag on the player made anew and its own spline on this map. The splines
    // went with the objects they moved (0x004d6750).
    forgetTaxi();
    moveSplines_.clear();
    // A ride's coordinates are its offset on the transport.
    if (crossMapTransportTransfer) knownPlace_.reset();
    else noteServerPlace(canonical);
    owner_.currentMountDisplayIdRef() = 0;
    if (owner_.mountCallbackRef()) {
        owner_.mountCallbackRef()(0);
    }

    // Every unit but the player leaves view (0x00524350 for each).
    std::vector<uint64_t> leaving;
    for (const auto& [guid, entity] : owner_.getEntityManager().getEntities()) {
        if (guid != owner_.getPlayerGuid() && entity->isUnit()) leaving.push_back(guid);
    }
    if (auto* combat = owner_.getCombatHandler()) {
        for (const uint64_t guid : leaving) combat->unitLeavesView(guid);
    }
    for (const auto& [guid, entity] : owner_.getEntityManager().getEntities()) {
        if (guid == owner_.getPlayerGuid()) continue;
        if (entity->getType() == ObjectType::UNIT && owner_.creatureDespawnCallbackRef()) {
            owner_.creatureDespawnCallbackRef()(guid);
        } else if (entity->getType() == ObjectType::PLAYER && owner_.playerDespawnCallbackRef()) {
            owner_.playerDespawnCallbackRef()(guid);
        } else if (entity->getType() == ObjectType::GAMEOBJECT && owner_.gameObjectDespawnCallbackRef()) {
            owner_.gameObjectDespawnCallbackRef()(guid);
        }
    }
    owner_.otherPlayerVisibleItemEntriesRef().clear();
    owner_.otherPlayerVisibleItemEnchantsRef().clear();
    owner_.otherPlayerVisibleDirtyRef().clear();
    otherPlayerMoveTimeMs_.clear();
    if (owner_.getSpellHandler()) owner_.getSpellHandler()->clearUnitCastStates();
    owner_.unitAurasCacheRef().clear();
    owner_.clearCombatText();
    owner_.getEntityManager().clear();
    owner_.clearHostileAttackers();
    owner_.worldStatesRef().clear();
    owner_.clearGossipPois();
    owner_.worldStateMapIdRef() = mapId;
    owner_.worldStateZoneIdRef() = 0;
    owner_.stopAutoAttack();
    if (auto* combat = owner_.getCombatHandler()) combat->forgetAttack();
    owner_.resetCastState();

    owner_.timeSinceLastPingRef() = 0.0f;
    if (owner_.getSocket()) {
        LOG_WARNING("World transfer keepalive: sending immediate ping");
        owner_.sendPing();
    }

    // The acknowledgement waits for the world: the client loads the map and
    // then sends it (0x00403b70, 0xdc), and no movement with it, the server
    // having put the player where it said. Sent first, the player stood in the
    // new world while its client was loading it. The world-entry callback
    // holds it when it queues the load, and the loader lets it go.
    if (worldportAck_.arrive(0, 0)) sendWorldportAck();
    if (owner_.worldEntryCallbackRef()) {
        owner_.worldEntryCallbackRef()(mapId, serverX, serverY, serverZ, isSameMap);
    }
    if (worldportAck_.settle()) sendWorldportAck();

    // No PLAYER_ENTERING_WORLD here. The client raises it once per world
    // entry, when the new world is in (0x00528010, event 0xff), which is
    // WorldLoader's after the load; raised here as well it came twice per
    // transfer, the first time straight after PLAYER_LEAVING_WORLD, with
    // the old zone's texts still up. Nor ZONE_CHANGED_NEW_AREA: the client
    // raises it only from the player's area (0x005204c0,
    // Renderer::updatePlayerArea), which forgets its texts on a new map so
    // the first area is still new.
}

// ============================================================
// Taxi / Flight Path Handlers
// ============================================================

void MovementHandler::loadTaxiDbc() {
    if (taxiDbcLoaded_) return;

    auto* am = owner_.services().assetManager;
    // Not an attempt: the assets are not there to read yet, and a
    // caller can reach this before they are. Marking it loaded here
    // meant one early call disabled this file for the whole session.
    if (!am || !am->isInitialized()) return;
    taxiDbcLoaded_ = true;

    auto nodesDbc = am->loadDBC("TaxiNodes.dbc");
    if (nodesDbc && nodesDbc->isLoaded()) {
        const auto* tnL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("TaxiNodes") : nullptr;
        // Cache field indices before the loop
        const uint32_t tnIdField   = tnL ? (*tnL)["ID"]    : 0;
        const uint32_t tnMapField  = tnL ? (*tnL)["MapID"] : 1;
        const uint32_t tnXField    = tnL ? (*tnL)["X"]     : 2;
        const uint32_t tnYField    = tnL ? (*tnL)["Y"]     : 3;
        const uint32_t tnZField    = tnL ? (*tnL)["Z"]     : 4;
        const uint32_t tnNameField = tnL ? (*tnL)["Name"]  : 5;
        // TaxiNodes.dbc ends with the two mount creature ids and nothing else.
        // There used to be a "fallback" pair read when both were zero, at 20
        // and 21 - which are the last name locale and the name flags word, so
        // a node with no mount took 16712190 as a display id. There is no
        // second pair to fall back to; a node without a mount has none.
        const uint32_t mountAllianceField = tnL ? (*tnL)["MountDisplayIdAlliance"] : 22;
        const uint32_t mountHordeField    = tnL ? (*tnL)["MountDisplayIdHorde"]    : 23;
        uint32_t fieldCount = nodesDbc->getFieldCount();
        for (uint32_t i = 0; i < nodesDbc->getRecordCount(); i++) {
            TaxiNode node;
            node.id = nodesDbc->getUInt32(i, tnIdField);
            node.mapId = nodesDbc->getUInt32(i, tnMapField);
            node.x = nodesDbc->getFloat(i, tnXField);
            node.y = nodesDbc->getFloat(i, tnYField);
            node.z = nodesDbc->getFloat(i, tnZField);
            node.name = nodesDbc->getString(i, tnNameField);
            if (fieldCount > mountHordeField) {
                node.mountDisplayIdAlliance = nodesDbc->getUInt32(i, mountAllianceField);
                node.mountDisplayIdHorde = nodesDbc->getUInt32(i, mountHordeField);
            }
            uint32_t nodeId = node.id;
            if (nodeId > 0) {
                taxiNodes_[nodeId] = std::move(node);
            }
            if (nodeId == 195) {
                std::string fields;
                for (uint32_t f = 0; f < fieldCount; f++) {
                    fields += std::to_string(f) + ":" + std::to_string(nodesDbc->getUInt32(i, f)) + " ";
                }
                LOG_INFO("TaxiNodes[195] fields: ", fields);
            }
        }
        LOG_INFO("Loaded ", taxiNodes_.size(), " taxi nodes from TaxiNodes.dbc");
    } else {
        LOG_WARNING("Could not load TaxiNodes.dbc");
    }

    auto pathDbc = am->loadDBC("TaxiPath.dbc");
    if (pathDbc && pathDbc->isLoaded()) {
        const auto* tpL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("TaxiPath") : nullptr;
        const uint32_t tpIdField   = tpL ? (*tpL)["ID"]       : 0;
        const uint32_t tpFromField = tpL ? (*tpL)["FromNode"] : 1;
        const uint32_t tpToField   = tpL ? (*tpL)["ToNode"]   : 2;
        const uint32_t tpCostField = tpL ? (*tpL)["Cost"]     : 3;
        for (uint32_t i = 0; i < pathDbc->getRecordCount(); i++) {
            TaxiPathEdge edge;
            edge.pathId = pathDbc->getUInt32(i, tpIdField);
            edge.fromNode = pathDbc->getUInt32(i, tpFromField);
            edge.toNode = pathDbc->getUInt32(i, tpToField);
            edge.cost = pathDbc->getUInt32(i, tpCostField);
            taxiPathEdges_.push_back(edge);
        }
        LOG_INFO("Loaded ", taxiPathEdges_.size(), " taxi path edges from TaxiPath.dbc");
    } else {
        LOG_WARNING("Could not load TaxiPath.dbc");
    }

    auto pathNodeDbc = am->loadDBC("TaxiPathNode.dbc");
    if (pathNodeDbc && pathNodeDbc->isLoaded()) {
        const auto* tpnL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("TaxiPathNode") : nullptr;
        const uint32_t tpnIdField    = tpnL ? (*tpnL)["ID"]        : 0;
        const uint32_t tpnPathField  = tpnL ? (*tpnL)["PathID"]    : 1;
        const uint32_t tpnIndexField = tpnL ? (*tpnL)["NodeIndex"] : 2;
        const uint32_t tpnMapField   = tpnL ? (*tpnL)["MapID"]     : 3;
        const uint32_t tpnXField     = tpnL ? (*tpnL)["X"]         : 4;
        const uint32_t tpnYField     = tpnL ? (*tpnL)["Y"]         : 5;
        const uint32_t tpnZField     = tpnL ? (*tpnL)["Z"]         : 6;
        for (uint32_t i = 0; i < pathNodeDbc->getRecordCount(); i++) {
            TaxiPathNode node;
            node.id = pathNodeDbc->getUInt32(i, tpnIdField);
            node.pathId = pathNodeDbc->getUInt32(i, tpnPathField);
            node.nodeIndex = pathNodeDbc->getUInt32(i, tpnIndexField);
            node.mapId = pathNodeDbc->getUInt32(i, tpnMapField);
            node.x = pathNodeDbc->getFloat(i, tpnXField);
            node.y = pathNodeDbc->getFloat(i, tpnYField);
            node.z = pathNodeDbc->getFloat(i, tpnZField);
            taxiPathNodes_[node.pathId].push_back(node);
        }
        for (auto& [pathId, nodes] : taxiPathNodes_) {
            std::sort(nodes.begin(), nodes.end(),
                [](const TaxiPathNode& a, const TaxiPathNode& b) {
                    return a.nodeIndex < b.nodeIndex;
                });
        }
        LOG_INFO("Loaded ", pathNodeDbc->getRecordCount(), " taxi path waypoints from TaxiPathNode.dbc");
    } else {
        LOG_WARNING("Could not load TaxiPathNode.dbc");
    }

    worldMapTransforms_ = readWorldMapTransforms(am->loadDBCOptional("WorldMapTransforms.dbc").get());
    if (auto dbc = am->loadDBC("WorldMapContinent.dbc"); dbc && dbc->isLoaded() && dbc->getFieldCount() >= 13) {
        // The first row for a map (0x005932e0).
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            taxiBounds_.emplace(dbc->getUInt32(i, 1),
                                TaxiMapBounds{dbc->getFloat(i, 9), dbc->getFloat(i, 10),
                                              dbc->getFloat(i, 11), dbc->getFloat(i, 12)});
        }
    }
    if (auto dbc = am->loadDBC("Map.dbc"); dbc && dbc->isLoaded() && dbc->getFieldCount() >= 66) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i)
            taxiMapExpansion_[dbc->getUInt32(i, 0)] = dbc->getUInt32(i, 63);
    }
}

void MovementHandler::handleShowTaxiNodes(network::Packet& packet) {
    ShowTaxiNodesData data;
    if (!ShowTaxiNodesParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_SHOWTAXINODES");
        return;
    }

    loadTaxiDbc();

    if (taxiMaskInitialized_) {
        for (uint32_t i = 0; i < TLK_TAXI_MASK_SIZE; ++i) {
            uint32_t newBits = data.nodeMask[i] & ~knownTaxiMask_[i];
            if (newBits == 0) continue;
            for (uint32_t bit = 0; bit < 32; ++bit) {
                if (newBits & (1u << bit)) {
                    uint32_t nodeId = i * 32 + bit + 1;
                    // Nothing said here: the original's line for a new
                    // flight point is ERR_NEWTAXIPATH, from
                    // SMSG_NEW_TAXI_PATH (0x006e2e90, case 0x1af). This
                    // printed "Discovered flight path: X" in English.
                    LOG_INFO("Taxi node ", nodeId, " newly known");
                }
            }
        }
    }

    for (uint32_t i = 0; i < TLK_TAXI_MASK_SIZE; ++i) {
        knownTaxiMask_[i] = data.nodeMask[i];
    }
    taxiMaskInitialized_ = true;

    // Fewer than two nodes known is nowhere to fly: ERR_TAXINOPATHS and no
    // map (0x006d6050 counts the mask's bits before 0x005932e0 opens it).
    uint32_t knownNodes = 0;
    for (uint32_t i = 0; i < TLK_TAXI_MASK_SIZE; ++i) knownNodes += static_cast<uint32_t>(std::popcount(data.nodeMask[i]));
    if (knownNodes < 2) {
        owner_.raiseUiError(interfaceString("ERR_TAXINOPATHS",
            "You don't know any flight locations connected to this one."));
        return;
    }

    currentTaxiData_ = data;
    taxiNpcGuid_ = data.npcGuid;
    buildTaxiMap();
    // With nothing on it the map does not open (0x005932e0).
    if (taxiMap_.nodes.empty()) {
        LOG_WARNING("Taxi map of node ", data.nearestNode, " lists nothing");
        return;
    }
    taxiWindowOpen_ = true;
    // TAXIMAP_OPENED opens with the flight master (0x005932e0 -> 0x0051fce0).
    owner_.setInteractionObject(taxiNpcGuid_, go_interaction::Window::Taxi);
    if (owner_.isGossipWindowOpen()) owner_.closeGossip();   // if 0x00512e60 has not already
    auto it = taxiNodes_.find(data.nearestNode);
    if (it != taxiNodes_.end()) {
        LOG_INFO("Taxi node ", data.nearestNode, " mounts: A=", it->second.mountDisplayIdAlliance,
                 " H=", it->second.mountDisplayIdHorde);
    }
    LOG_INFO("Taxi window opened, nearest node=", data.nearestNode);
    // The flight map opens on this and closes on its pair. Neither was fired,
    // so the original interface's map could not appear however complete the
    // functions behind it were.
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("TAXIMAP_OPENED", {});
    // 0x005932e0: tutorial 0x22 after TAXIMAP_OPENED.
    owner_.triggerTutorial(0x22);
}

void MovementHandler::applyTaxiMountForCurrentNode() {
    if (taxiMountActive_ || !owner_.mountCallbackRef()) return;
    auto it = taxiNodes_.find(currentTaxiData_.nearestNode);
    if (it == taxiNodes_.end()) {
        bool isAlliance = true;
        switch (owner_.playerRaceRef()) {
            case Race::ORC: case Race::UNDEAD: case Race::TAUREN: case Race::TROLL:
            case Race::GOBLIN: case Race::BLOOD_ELF:
                isAlliance = false; break;
            default: break;
        }
        uint32_t mountId = isAlliance ? 1210u : 1310u;
        taxiMountActive_ = true;
        LOG_INFO("Taxi mount fallback (node ", currentTaxiData_.nearestNode, " not in DBC): displayId=", mountId);
        owner_.mountCallbackRef()(mountId);
        return;
    }

    bool isAlliance = true;
    switch (owner_.playerRaceRef()) {
        case Race::ORC:
        case Race::UNDEAD:
        case Race::TAUREN:
        case Race::TROLL:
        case Race::GOBLIN:
        case Race::BLOOD_ELF:
            isAlliance = false;
            break;
        default:
            isAlliance = true;
            break;
    }
    uint32_t mountId = isAlliance ? it->second.mountDisplayIdAlliance
                                  : it->second.mountDisplayIdHorde;
    if (mountId == 541) mountId = 0;
    if (mountId == 0) {
        mountId = isAlliance ? it->second.mountDisplayIdHorde
                             : it->second.mountDisplayIdAlliance;
        if (mountId == 541) mountId = 0;
    }
    if (mountId == 0) {
        uint32_t gryphonId = owner_.services().gryphonDisplayId;
        uint32_t wyvernId = owner_.services().wyvernDisplayId;
        if (isAlliance && gryphonId != 0) mountId = gryphonId;
        if (!isAlliance && wyvernId != 0) mountId = wyvernId;
        if (mountId == 0) {
            mountId = (isAlliance ? wyvernId : gryphonId);
        }
    }
    if (mountId == 0) {
        if (it->second.mountDisplayIdAlliance != 0) mountId = it->second.mountDisplayIdAlliance;
        else if (it->second.mountDisplayIdHorde != 0) mountId = it->second.mountDisplayIdHorde;
    }
    if (mountId == 0) {
        static constexpr uint32_t kAllianceTaxiDisplays[] = {1210u, 1211u, 1212u, 1213u};
        static constexpr uint32_t kHordeTaxiDisplays[] = {1310u, 1311u, 1312u};
        mountId = isAlliance ? kAllianceTaxiDisplays[0] : kHordeTaxiDisplays[0];
    }
    if (mountId == 0) {
        mountId = isAlliance ? 30412u : 30413u;
    }
    if (mountId != 0) {
        taxiMountActive_ = true;
        LOG_INFO("Taxi mount apply: displayId=", mountId);
        owner_.mountCallbackRef()(mountId);
    }
}

glm::vec3 MovementHandler::evalTaxiCatmullRom(const glm::vec3& p0, const glm::vec3& p1,
                                               const glm::vec3& p2, const glm::vec3& p3, float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return 0.5f * (
        (2.0f * p1) +
        (-p0 + p2) * t +
        (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
        (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3
    );
}

void MovementHandler::startClientTaxiPath(const std::vector<uint32_t>& pathNodes) {
    taxiClientPath_.clear();
    taxiClientIndex_ = 0;
    taxiClientActive_ = false;
    taxiClientSegmentProgress_ = 0.0f;

    for (size_t i = 0; i + 1 < pathNodes.size(); i++) {
        uint32_t fromNode = pathNodes[i];
        uint32_t toNode = pathNodes[i + 1];
        uint32_t pathId = 0;
        for (const auto& edge : taxiPathEdges_) {
            if (edge.fromNode == fromNode && edge.toNode == toNode) {
                pathId = edge.pathId;
                break;
            }
        }
        if (pathId == 0) {
            LOG_WARNING("No taxi path found from node ", fromNode, " to ", toNode);
            continue;
        }
        auto pathIt = taxiPathNodes_.find(pathId);
        if (pathIt != taxiPathNodes_.end()) {
            for (const auto& wpNode : pathIt->second) {
                glm::vec3 serverPos(wpNode.x, wpNode.y, wpNode.z);
                glm::vec3 canonical = core::coords::serverToCanonical(serverPos);
                taxiClientPath_.push_back(canonical);
            }
        } else {
            LOG_WARNING("No spline waypoints found for taxi pathId ", pathId);
        }
    }

    if (taxiClientPath_.size() < 2) {
        taxiClientPath_.clear();
        for (uint32_t nodeId : pathNodes) {
            auto nodeIt = taxiNodes_.find(nodeId);
            if (nodeIt == taxiNodes_.end()) continue;
            glm::vec3 serverPos(nodeIt->second.x, nodeIt->second.y, nodeIt->second.z);
            taxiClientPath_.push_back(core::coords::serverToCanonical(serverPos));
        }
    }

    if (taxiClientPath_.size() < 2) {
        LOG_WARNING("Taxi path too short: ", taxiClientPath_.size(), " waypoints");
        return;
    }

    // Precompute each segment's real curve arc length (see taxiClientSegmentArcLengths_'s
    // comment for why - pacing must use this, not the straight-line chord length, to keep
    // total flight duration in line with the server's own identical-speed spline). Sampled
    // numerically since the Catmull-Rom curve has no closed-form arc length; 16 sub-steps
    // is more than enough resolution for a taxi path segment's typical curvature.
    taxiClientSegmentArcLengths_.clear();
    taxiClientSegmentArcLengths_.reserve(taxiClientPath_.size() - 1);
    constexpr int kArcLengthSamples = 16;
    for (size_t i = 0; i + 1 < taxiClientPath_.size(); i++) {
        glm::vec3 p0 = (i > 0) ? taxiClientPath_[i - 1] : taxiClientPath_[i];
        glm::vec3 p1 = taxiClientPath_[i];
        glm::vec3 p2 = taxiClientPath_[i + 1];
        glm::vec3 p3 = (i + 2 < taxiClientPath_.size()) ? taxiClientPath_[i + 2] : taxiClientPath_[i + 1];

        float arcLength = 0.0f;
        glm::vec3 prev = p1;
        for (int s = 1; s <= kArcLengthSamples; s++) {
            float t = static_cast<float>(s) / static_cast<float>(kArcLengthSamples);
            glm::vec3 cur = evalTaxiCatmullRom(p0, p1, p2, p3, t);
            arcLength += glm::length(cur - prev);
            prev = cur;
        }
        taxiClientSegmentArcLengths_.push_back(arcLength);
    }

    // NOTE: this only builds taxiClientPath_ as pending data - it deliberately does
    // NOT snap the player to the path start or set taxiClientActive_. CMSG_ACTIVATETAXI
    // can still be rejected by the server at this point; see activateTaxi()'s comment
    // and beginTaxiFlightMotion(), which does the actual activation once
    // SMSG_ACTIVATETAXIREPLY confirms success.
}

void MovementHandler::beginTaxiFlightMotion() {
    if (taxiClientPath_.size() < 2) return;

    glm::vec3 start = taxiClientPath_[0];
    glm::vec3 dir(0.0f);
    float dirLenSq = 0.0f;
    for (size_t i = 1; i < taxiClientPath_.size(); i++) {
        dir = taxiClientPath_[i] - start;
        dirLenSq = glm::dot(dir, dir);
        if (dirLenSq >= 1e-6f) {
            break;
        }
    }

    float initialOrientation = movementInfo.orientation;
    float initialRenderYaw = movementInfo.orientation;
    float initialPitch = 0.0f;
    float initialRoll = 0.0f;
    if (dirLenSq >= 1e-6f) {
        initialOrientation = std::atan2(dir.y, dir.x);
        glm::vec3 renderDir = core::coords::canonicalToRender(dir);
        initialRenderYaw = std::atan2(renderDir.y, renderDir.x);
        glm::vec3 dirNorm = dir * glm::inversesqrt(dirLenSq);
        initialPitch = std::asin(std::clamp(dirNorm.z, -1.0f, 1.0f));
    }

    movementInfo.x = start.x;
    movementInfo.y = start.y;
    movementInfo.z = start.z;
    movementInfo.orientation = initialOrientation;
    sanitizeMovementForTaxi();

    auto playerEntity = owner_.getEntityManager().getEntity(owner_.getPlayerGuid());
    if (playerEntity) {
        playerEntity->setPosition(start.x, start.y, start.z, initialOrientation);
    }

    if (owner_.taxiOrientationCallbackRef()) {
        owner_.taxiOrientationCallbackRef()(initialRenderYaw, initialPitch, initialRoll);
    }

    LOG_INFO("Taxi flight started with ", taxiClientPath_.size(), " spline waypoints");
    taxiServerCompletionPending_ = false;
    taxiClientActive_ = true;
}

// Ends the active client-simulated taxi flight. snapToFinalWaypoint controls
// whether the player is teleported to the known landing position first:
// - true: natural completion (the client spline reached its own end) - our
//   own path data is authoritative for where "arrived" means.
// - false: an authoritative server signal (SMSG_DISMOUNT with
//   UNIT_FLAG_TAXI_FLIGHT already cleared, or the equivalent
//   UNIT_FIELD_MOUNTDISPLAYID update) arrived ahead of our own spline
//   finishing.
//
//   The server's stop point may not match our path's precomputed final
//   waypoint - e.g. a genuinely interrupted/mid-route stop - so this does
//   NOT unconditionally snap there. But per AzerothCore's own
//   FlightPathMovementGenerator::DoFinalize() (confirmed by reading its
//   source): "update z position to ground ... this prevent cheating with
//   landing point at lags when client side flight end early in comparison
//   server side" - the server itself anticipates exactly this scenario and
//   corrects the player's own authoritative position to the true
//   destination on the terminal leg. So: if we're within a generous sanity
//   distance of our own known final waypoint (kAuthoritativeLandingSnapDist),
//   treat this as that same normal "client finished slightly early" case
//   and snap there - safety net for whatever spline/server pacing drift
//   remains despite arc-length-parameterizing taxiClientPath_'s pacing (see
//   taxiClientSegmentArcLengths_). Beyond the sanity distance, stay
//   conservative and leave the player where the spline currently has them,
//   per the original caution.
//
// The "known final waypoint" itself is taxiNodes_[taxiDestNodeId_]'s own
// registered position (TaxiNodes.dbc), NOT taxiClientPath_.back() (the last
// entry of the separate TaxiPathNode.dbc waypoint trace) - live-confirmed
// the two can disagree by 100+ yards, since a taxi path's waypoint trace
// doesn't necessarily terminate exactly on the destination node's actual
// coordinate. TaxiNodes.dbc's own position is what GM teleports/.gps
// validated as accurate throughout this whole effort, so it's the correct
// "true destination" to trust here. Falls back to taxiClientPath_.back()
// only if the destination node can't be resolved for some reason.
void MovementHandler::finishClientTaxiFlight(bool snapToFinalWaypoint) {
    glm::vec3 landingPos(0.0f);
    bool haveLandingPos = false;
    auto destNodeIt = taxiNodes_.find(taxiDestNodeId_);
    if (destNodeIt != taxiNodes_.end()) {
        landingPos = core::coords::serverToCanonical(
            glm::vec3(destNodeIt->second.x, destNodeIt->second.y, destNodeIt->second.z));
        haveLandingPos = true;
    } else if (!taxiClientPath_.empty()) {
        landingPos = taxiClientPath_.back();
        haveLandingPos = true;
    }

    if (!snapToFinalWaypoint && haveLandingPos) {
        constexpr float kAuthoritativeLandingSnapDist = 300.0f;
        glm::vec3 gap = landingPos - glm::vec3(movementInfo.x, movementInfo.y, movementInfo.z);
        if (glm::length(gap) <= kAuthoritativeLandingSnapDist) {
            LOG_INFO("Taxi landing (authoritative early completion): within sanity "
                     "distance of known final waypoint, snapping there instead of "
                     "current spline position (gap=", glm::length(gap), ")");
            snapToFinalWaypoint = true;
        }
    }
    if (snapToFinalWaypoint && haveLandingPos) {
        auto playerEntity = owner_.getEntityManager().getEntity(owner_.getPlayerGuid());
        if (playerEntity) {
            playerEntity->setPosition(landingPos.x, landingPos.y, landingPos.z,
                                      movementInfo.orientation);
        }
        movementInfo.x = landingPos.x;
        movementInfo.y = landingPos.y;
        movementInfo.z = landingPos.z;
        // Application's own per-frame render-position sync only runs while onTaxi
        // is true (see its "Sync character render position" block) - by the time
        // this function returns, onTaxiFlight_/taxiMountActive_ are already false,
        // so that sync won't pick up this correction. Push it to the renderer
        // directly instead - see playerPositionCorrectionCallbackRef()'s comment.
        if (owner_.playerPositionCorrectionCallbackRef()) {
            owner_.playerPositionCorrectionCallbackRef()(landingPos.x, landingPos.y, landingPos.z);
        }
        LOG_INFO("Taxi landing: snapped to final waypoint (",
                 landingPos.x, ", ", landingPos.y, ", ", landingPos.z, ")");
    }
    taxiClientActive_ = false;
    onTaxiFlight_ = false;
    taxiLandingCooldown_ = 2.0f;
    if (taxiMountActive_ && owner_.mountCallbackRef()) {
        owner_.mountCallbackRef()(0);
    }
    taxiMountActive_ = false;
    owner_.currentMountDisplayIdRef() = 0;
    // Some WotLK servers expose the taxi mount through vehicle data.
    // Clear that cached ID at landing so the dismount control does not
    // remain visible after the flight has completed.
    owner_.vehicleIdRef() = 0;
    taxiClientPath_.clear();
    taxiDestNodeId_ = 0;
    taxiServerCompletionPending_ = false;
    taxiRecoverPending_ = false;
    movementInfo.flags = 0;
    movementInfo.flags2 = 0;
    if (owner_.getSocket()) {
        sendMovement(Opcode::MSG_MOVE_STOP);
        sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    }
    LOG_INFO("Taxi flight landed (client path)");
}

void MovementHandler::deferServerTaxiCompletion() {
    if (!taxiClientActive_) return;
    taxiServerCompletionPending_ = true;
}

bool MovementHandler::isNearTaxiDestination(float maxDistance) const {
    if (!taxiClientActive_ || taxiClientPath_.empty()) return false;

    const glm::vec3& destination = taxiClientPath_.back();
    const float dx = movementInfo.x - destination.x;
    const float dy = movementInfo.y - destination.y;
    const float dz = movementInfo.z - destination.z;
    return dx * dx + dy * dy + dz * dz <= maxDistance * maxDistance;
}

void MovementHandler::updateClientTaxi(float deltaTime) {
    if (dismountGraceRemaining_ > 0.0f) {
        dismountGraceRemaining_ -= deltaTime;
    }

    // Live-confirmed: a CMSG_ACTIVATETAXI the server can't make sense of (e.g.
    // a stale start node vs. the player's actual position) can go completely
    // unanswered - no SMSG_ACTIVATETAXIREPLY at all, success or failure. Before
    // activation was deferred to the reply (see activateTaxi()'s comment), the
    // speculative flight start accidentally "recovered" from this since it
    // never waited on a reply in the first place. Now that it correctly does,
    // a dropped reply left taxiActivatePending_ true forever - every future
    // activateTaxi() call silently no-ops on its pending-guard, permanently
    // soft-locking the player's taxi system until relog. Time out and clear
    // pending state the same way an explicit failure reply would.
    if (taxiActivatePending_) {
        taxiActivateTimer_ += deltaTime;
        if (taxiActivateTimer_ > kTaxiActivateReplyTimeoutSeconds) {
            LOG_WARNING("Taxi activation reply timed out after ",
                        kTaxiActivateReplyTimeoutSeconds, "s - clearing pending state");
            taxiActivatePending_ = false;
            taxiActivateTimer_ = 0.0f;
            taxiClientPath_.clear();
            taxiClientIndex_ = 0;
            taxiClientSegmentProgress_ = 0.0f;
            if (taxiMountActive_ && owner_.mountCallbackRef()) {
                owner_.mountCallbackRef()(0);
            }
            taxiMountActive_ = false;
            onTaxiFlight_ = false;
        }
    }

    if (!isPreWotlk()) {
        updateServerTaxi(deltaTime);
        return;
    }
    if (!taxiClientActive_ || taxiClientPath_.size() < 2) return;
    auto playerEntity = owner_.getEntityManager().getEntity(owner_.getPlayerGuid());

    if (taxiClientIndex_ + 1 >= taxiClientPath_.size()) {
        finishClientTaxiFlight(/*snapToFinalWaypoint=*/true);
        return;
    }

    float remainingDistance = taxiClientSegmentProgress_ + (taxiClientSpeed_ * deltaTime);
    glm::vec3 start(0.0f);
    glm::vec3 end(0.0f);
    glm::vec3 dir(0.0f);
    float segmentLen = 0.0f;
    float t = 0.0f;

    while (true) {
        if (taxiClientIndex_ + 1 >= taxiClientPath_.size()) {
            finishClientTaxiFlight(/*snapToFinalWaypoint=*/true);
            return;
        }

        start = taxiClientPath_[taxiClientIndex_];
        end = taxiClientPath_[taxiClientIndex_ + 1];
        dir = end - start;
        float segLenSq = glm::dot(dir, dir);

        if (segLenSq < 1e-4f) {
            taxiClientIndex_++;
            continue;
        }
        // Pace by the curve's real arc length, not the straight-line chord -
        // see taxiClientSegmentArcLengths_'s comment. Fall back to the chord
        // length only if the precomputed array is somehow missing/stale.
        segmentLen = (taxiClientIndex_ < taxiClientSegmentArcLengths_.size())
            ? taxiClientSegmentArcLengths_[taxiClientIndex_]
            : std::sqrt(segLenSq);

        if (remainingDistance >= segmentLen) {
            remainingDistance -= segmentLen;
            taxiClientIndex_++;
            taxiClientSegmentProgress_ = 0.0f;
            continue;
        }

        taxiClientSegmentProgress_ = remainingDistance;
        t = taxiClientSegmentProgress_ / segmentLen;
        break;
    }

    glm::vec3 p0 = (taxiClientIndex_ > 0) ? taxiClientPath_[taxiClientIndex_ - 1] : start;
    glm::vec3 p1 = start;
    glm::vec3 p2 = end;
    glm::vec3 p3 = (taxiClientIndex_ + 2 < taxiClientPath_.size()) ?
                   taxiClientPath_[taxiClientIndex_ + 2] : end;

    glm::vec3 nextPos = evalTaxiCatmullRom(p0, p1, p2, p3, t);

    float t2 = t * t;
    glm::vec3 tangent = 0.5f * (
        (-p0 + p2) +
        2.0f * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t +
        3.0f * (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t2
    );
    float tangentLenSq = glm::dot(tangent, tangent);
    if (tangentLenSq < 1e-8f) {
        tangent = dir;
        tangentLenSq = glm::dot(tangent, tangent);
        if (tangentLenSq < 1e-8f) {
            tangent = glm::vec3(std::cos(movementInfo.orientation), std::sin(movementInfo.orientation), 0.0f);
            tangentLenSq = 1.0f; // unit vector
        }
    }

    float targetOrientation = std::atan2(tangent.y, tangent.x);

    glm::vec3 tangentNorm = tangent * glm::inversesqrt(std::max(tangentLenSq, 1e-8f));
    float pitch = std::asin(std::clamp(tangentNorm.z, -1.0f, 1.0f));

    float currentOrientation = movementInfo.orientation;
    float orientDiff = targetOrientation - currentOrientation;
    while (orientDiff > core::coords::PI) orientDiff -= core::coords::TWO_PI;
    while (orientDiff < -core::coords::PI) orientDiff += core::coords::TWO_PI;
    float roll = -orientDiff * 2.5f;
    roll = std::clamp(roll, -0.7f, 0.7f);

    float smoothOrientation = currentOrientation + orientDiff * std::min(1.0f, deltaTime * 3.0f);

    if (playerEntity) {
        playerEntity->setPosition(nextPos.x, nextPos.y, nextPos.z, smoothOrientation);
    }
    movementInfo.x = nextPos.x;
    movementInfo.y = nextPos.y;
    movementInfo.z = nextPos.z;
    movementInfo.orientation = smoothOrientation;

    // A server completion packet can arrive much earlier than the local DBC
    // spline endpoint (observed on CMaNGOS), and it is commonly sent only once.
    // Do not land at the packet's early position, but also do not discard it:
    // consume it inside a small landing radius so the mount comes off promptly
    // rather than lingering through the tail of a mismatched local path.
    if (taxiServerCompletionPending_ && isNearTaxiDestination()) {
        finishClientTaxiFlight(/*snapToFinalWaypoint=*/true);
        return;
    }

    if (owner_.taxiOrientationCallbackRef()) {
        glm::vec3 renderTangent = core::coords::canonicalToRender(tangent);
        float renderYaw = std::atan2(renderTangent.y, renderTangent.x);
        owner_.taxiOrientationCallbackRef()(renderYaw, pitch, roll);
    }
}

void MovementHandler::forgetTaxi() {
    onTaxiFlight_ = false;
    taxiFlagSeen_ = false;
    taxiFlagWait_ = 0.0f;
    taxiMountActive_ = false;
    taxiActivatePending_ = false;
    taxiActivateTimer_ = 0.0f;
    taxiClientActive_ = false;
    taxiClientPath_.clear();
    taxiServerCompletionPending_ = false;
    taxiRecoverPending_ = false;
    taxiStartGrace_ = 0.0f;
}

bool MovementHandler::playerTaxiFlag() const {
    const auto player = owner_.getEntityManager().getEntity(owner_.getPlayerGuid());
    return player && player->isUnit() &&
           (static_cast<const Unit&>(*player).getUnitFlags() & UNIT_FLAG_TAXI_FLIGHT) != 0;
}

void MovementHandler::updateServerTaxi(float deltaTime) {
    const bool flagged = playerTaxiFlag();
    if (flagged && !onTaxiFlight_) {
        // Taken up here as well where no request was made from here: a login
        // in flight, and the far side of a map's border.
        onTaxiFlight_ = true;
        sanitizeMovementForTaxi();
        LOG_INFO("Taxi flight under way");
    }
    if (!onTaxiFlight_) return;
    if (flagged) taxiFlagSeen_ = true;

    const uint64_t guid = owner_.getPlayerGuid();
    const auto player = owner_.getEntityManager().getEntity(guid);
    const bool carried = onMoveSpline(guid);
    if (player) {
        // The movement kept goes with the unit; the unit is not set, which
        // would end its spline.
        movementInfo.x = player->getX();
        movementInfo.y = player->getY();
        movementInfo.z = player->getZ();
        movementInfo.orientation = player->getOrientation();
        if (const auto heading = player->pathHeading(); heading && owner_.taxiOrientationCallbackRef()) {
            const glm::vec3 tangent = core::coords::canonicalToRender(heading->tangent);
            if (glm::dot(tangent, tangent) > 1e-8f) {
                const glm::vec3 ahead = core::coords::canonicalToRender(heading->ahead);
                const float pitch = std::asin(std::clamp(tangent.z / glm::length(tangent), -1.0f, 1.0f));
                owner_.taxiOrientationCallbackRef()(std::atan2(tangent.y, tangent.x), pitch,
                                                    flightBank(tangent, ahead));
            }
        }
    }
    if (flagged || carried) return;
    // Over once the server has taken the flag back and the last spline has
    // run out - or where a flight it accepted never came.
    if (!taxiFlagSeen_) {
        taxiFlagWait_ -= deltaTime;
        if (taxiFlagWait_ > 0.0f) return;
    }
    endServerTaxi();
}

void MovementHandler::endServerTaxi() {
    onTaxiFlight_ = false;
    taxiFlagSeen_ = false;
    taxiDestNodeId_ = 0;
    taxiLandingCooldown_ = 2.0f;
    sanitizeMovementForTaxi();
    // Some servers carry the taxi mount in the vehicle data.
    owner_.vehicleIdRef() = 0;
    // Where the last spline ended; nothing puts the player on the flight
    // master's place.
    if (auto player = owner_.getEntityManager().getEntity(owner_.getPlayerGuid())) {
        player->setPosition(player->getLatestX(), player->getLatestY(), player->getLatestZ(),
                            player->getOrientation());
        movementInfo.x = player->getX();
        movementInfo.y = player->getY();
        movementInfo.z = player->getZ();
        if (owner_.playerPositionCorrectionCallbackRef())
            owner_.playerPositionCorrectionCallbackRef()(movementInfo.x, movementInfo.y, movementInfo.z);
    }
    LOG_INFO("Taxi flight over");
}

void MovementHandler::handleActivateTaxiReply(network::Packet& packet) {
    ActivateTaxiReplyData data;
    if (!ActivateTaxiReplyParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_ACTIVATETAXIREPLY");
        return;
    }

    // Whether a reply arrived at all is the first thing worth knowing when a
    // flight does not happen, and it was only ever said at info level.
    LOG_WARNING("SMSG_ACTIVATETAXIREPLY: result=", data.result,
                " pending=", taxiActivatePending_);

    if (!taxiActivatePending_) {
        LOG_DEBUG("Ignoring stray taxi reply: result=", data.result);
        return;
    }

    if (data.result == 0) {
        if (onTaxiFlight_ && !taxiActivatePending_) {
            return;
        }
        onTaxiFlight_ = true;
        taxiStartGrace_ = std::max(taxiStartGrace_, 2.0f);
        sanitizeMovementForTaxi();
        // Closed here rather than through closeTaxi, which would clear the taxi
        // mount that is about to be applied - but the event has to be sent all
        // the same, with closeTaxi's own guard: the flight map hides on this,
        // and setting the flag alone left FrameXML's open for the whole flight.
        const bool taxiWasOpen = taxiWindowOpen_;
        taxiWindowOpen_ = false;
        if (taxiWasOpen && owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("TAXIMAP_CLOSED", {});
        }
        taxiActivatePending_ = false;
        taxiActivateTimer_ = 0.0f;
        if (!isPreWotlk()) {
            // The answer only lets the flight map go (0x006cee90 ->
            // 0x00590ba0). The mount, the flag and the spline are the
            // server's to send.
            taxiFlagSeen_ = false;
            taxiFlagWait_ = 5.0f;
            LOG_INFO("Taxi flight accepted");
            return;
        }
        applyTaxiMountForCurrentNode();
        // Now that the server has confirmed, actually move the player to the path
        // start and start advancing the spline (see activateTaxi()'s comment).
        beginTaxiFlightMotion();
        if (owner_.getSocket()) {
            sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
        }
        LOG_INFO("Taxi flight started!");
    } else {
        if (onTaxiFlight_ || taxiClientActive_) {
            LOG_WARNING("Ignoring stale taxi failure reply while flight is active: result=", data.result);
            taxiActivatePending_ = false;
            taxiActivateTimer_ = 0.0f;
            return;
        }
        LOG_WARNING("Taxi activation failed, result=", data.result);
        // The result's own ERR_TAXI* line, as 0x006cee90 shows it. This was
        // "Cannot take that flight path." in English for every result.
        if (const TaxiError* e = taxiReplyError(data.result)) showTaxiError(owner_, *e);
        taxiActivatePending_ = false;
        taxiActivateTimer_ = 0.0f;
        // Clear the pending path data built speculatively by activateTaxi() -
        // it was never committed (taxiClientActive_ never went true), but drop it
        // explicitly so a retry can't somehow pick up stale waypoints.
        taxiClientPath_.clear();
        taxiClientIndex_ = 0;
        taxiClientSegmentProgress_ = 0.0f;
        taxiDestNodeId_ = 0;
        if (taxiMountActive_ && owner_.mountCallbackRef()) {
            owner_.mountCallbackRef()(0);
        }
        taxiMountActive_ = false;
        onTaxiFlight_ = false;
    }
}

void MovementHandler::closeTaxi() {
    const bool wasOpen = taxiWindowOpen_;
    taxiWindowOpen_ = false;
    // Only when it was actually open: closeTaxi is also reached on paths that
    // never opened a window, and the flight map hides itself on this.
    if (wasOpen && owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("TAXIMAP_CLOSED", {});
    }

    if (taxiActivatePending_ || onTaxiFlight_ || taxiClientActive_) {
        return;
    }

    if (taxiMountActive_ && owner_.mountCallbackRef()) {
        LOG_INFO("closeTaxi: clearing taxi mount");
        owner_.mountCallbackRef()(0);
    }
    taxiMountActive_ = false;

    taxiActivatePending_ = false;
    onTaxiFlight_ = false;

    taxiLandingCooldown_ = 2.0f;
}

void MovementHandler::buildTaxiMap() {
    const uint32_t start = currentTaxiData_.nearestNode;
    const auto& account = owner_.accountState();
    taxiMap_ = wowee::game::buildTaxiMap(
        taxiNodes_, start, worldMapTransforms_,
        [this](uint32_t mapId) {
            const auto it = taxiBounds_.find(mapId);
            return it != taxiBounds_.end() ? it->second : TaxiMapBounds{};
        },
        [this](uint32_t nodeId) { return currentTaxiData_.isNodeKnown(nodeId); },
        [&](uint32_t mapId) {
            const auto it = taxiMapExpansion_.find(mapId);
            return !account.present || it == taxiMapExpansion_.end() || it->second <= account.accountExpansion;
        });

    std::vector<TaxiLeg> legs;
    std::set<std::pair<uint32_t, uint32_t>> seen;
    for (const auto& edge : taxiPathEdges_) {
        // The first row joining two nodes is theirs (0x005938d0).
        if (!seen.insert({edge.fromNode, edge.toNode}).second) continue;
        if (!taxiMap_.lists(edge.fromNode) || !taxiMap_.lists(edge.toNode)) continue;
        // 0x00592580: along the waypoints, where they are shown.
        float length = 1e11f;
        if (const auto path = taxiPathNodes_.find(edge.pathId); path != taxiPathNodes_.end()) {
            length = 0.0f;
            for (size_t i = 1; i < path->second.size(); ++i) {
                const auto& a = path->second[i - 1];
                const auto& b = path->second[i];
                const ShownPlace pa = shownPlace(worldMapTransforms_, a.mapId, a.x, a.y);
                const ShownPlace pb = shownPlace(worldMapTransforms_, b.mapId, b.x, b.y);
                length += glm::length(glm::vec3(pb.x - pa.x, pb.y - pa.y, b.z - a.z));
            }
        }
        legs.push_back({edge.fromNode, edge.toNode, edge.cost, length});
    }
    const bool alliance = owner_.isPlayerAlliance();
    taxiRoutes_.build(taxiMap_, legs, start, [&](uint32_t nodeId) {
        const auto it = taxiNodes_.find(nodeId);
        return it != taxiNodes_.end() &&
               taxiNodeRefused(alliance, it->second.mountDisplayIdAlliance, it->second.mountDisplayIdHorde);
    });
}

std::pair<float, float> MovementHandler::taxiLegPosition(uint32_t nodeId) const {
    const auto it = taxiNodes_.find(nodeId);
    if (it == taxiNodes_.end()) return {0.0f, 0.0f};
    return taxiLegPos(taxiMap_, worldMapTransforms_, it->second.mapId, it->second.x, it->second.y);
}

uint32_t MovementHandler::getTaxiCostTo(uint32_t destNodeId) const {
    return taxiRoutes_.cost(destNodeId, owner_.discountStanding(taxiNpcGuid_));
}

bool MovementHandler::hasTaxiRouteTo(uint32_t destNodeId) const { return taxiRoutes_.reaches(destNodeId); }

std::vector<uint32_t> MovementHandler::getTaxiRouteTo(uint32_t destNodeId) const {
    return taxiRoutes_.to(destNodeId);
}

void MovementHandler::activateTaxi(uint32_t destNodeId) {
    if (!owner_.getSocket() || owner_.getState() != WorldState::IN_WORLD) return;

    if (taxiActivatePending_ || onTaxiFlight_) {
        return;
    }

    uint32_t startNode = currentTaxiData_.nearestNode;
    if (startNode == 0 || destNodeId == 0 || startNode == destNodeId) return;

    taxiDestNodeId_ = destNodeId;

    if (owner_.isMounted()) {
        LOG_INFO("Taxi activate: dismounting current mount");
        if (owner_.mountCallbackRef()) owner_.mountCallbackRef()(0);
        owner_.currentMountDisplayIdRef() = 0;
        dismount();
    }

    {
        auto destIt = taxiNodes_.find(destNodeId);
        // No line: TakeTaxiNode (0x00591680 -> 0x005914b0) only sends. These
        // were English.
        if (destIt != taxiNodes_.end() && !destIt->second.name.empty()) {
            taxiDestName_ = destIt->second.name;
        } else {
            taxiDestName_.clear();
        }
    }

    // The route the cost map already worked out, rather than a second search of
    // the same graph.
    //
    // The one here searched every edge, including those reaching nodes the
    // player has never been to. The server refuses a flight the moment one
    // appears in the list - ERR_TAXINOTVISITED, checked node by node - so a
    // route through an undiscovered stop is turned down outright rather than
    // flown badly. buildTaxiMap already skips those, which is why the price
    // quoted came from a route this then declined to use.
    std::vector<uint32_t> path = getTaxiRouteTo(destNodeId);

    if (path.size() < 2) {
        LOG_WARNING("No taxi route through discovered nodes from ", startNode,
                    " to ", destNodeId);
        // ERR_TAXINOSUCHPATH in yellow, as 0x005914b0 refuses a node with no
        // route to it. This was English, in chat.
        showTaxiError(owner_, kTaxiNoSuchPath);
        return;
    }

    LOG_INFO("Taxi path: ", path.size(), " nodes, from ", startNode, " to ", destNodeId);

    // At warning level: the log carries nothing below it, and a flight that
    // goes wrong is otherwise reported with the request that caused it
    // invisible. A guid of zero here means no flight master was recorded, and
    // the server has nothing to answer.
    LOG_WARNING("Taxi activate: npc=0x", std::hex, taxiNpcGuid_, std::dec,
                " start=", startNode, " dest=", destNodeId,
                " pathLen=", path.size());
    if (!path.empty()) {
        std::string pathStr;
        for (size_t i = 0; i < path.size(); i++) {
            pathStr += std::to_string(path[i]);
            if (i + 1 < path.size()) pathStr += "->";
        }
        LOG_INFO("Taxi path nodes: ", pathStr);
    }

    uint32_t totalCost = getTaxiCostTo(destNodeId);
    LOG_INFO("Taxi activate: start=", startNode, " dest=", destNodeId, " cost=", totalCost);

    // A route of more than two nodes has to go as ACTIVATETAXIEXPRESS with the
    // whole path. CMSG_ACTIVATETAXI carries only a source and a destination,
    // and the server answers it by looking for a single TaxiPath joining the
    // two - so anything needing an intermediate stop has no such path, gets no
    // reply, and times out. Stormwind to Westfall is one hop and worked;
    // everything further did not.
    if (path.size() > 2) {
        auto expressPkt =
            ActivateTaxiExpressPacket::build(taxiNpcGuid_, path);
        owner_.getSocket()->send(expressPkt);
        LOG_WARNING("Taxi activate: sent EXPRESS for ", path.size(),
                    " nodes (a multi-hop route has no single path to ask for)");
    } else {
        auto basicPkt = ActivateTaxiPacket::build(taxiNpcGuid_, startNode, destNodeId);
        owner_.getSocket()->send(basicPkt);
    }

    // The window closes on the request, not on the reply, and FrameXML's taxi
    // frame only hides on the event. Clearing the flag alone closes this
    // client's own map and node list, which poll it, and leaves TaxiFrame up
    // for the whole flight - the reply's own send of this event cannot cover it,
    // because its guard reads the flag this line has already cleared.
    const bool taxiWasOpen = taxiWindowOpen_;
    taxiWindowOpen_ = false;
    if (taxiWasOpen && owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("TAXIMAP_CLOSED", {});
    }
    taxiActivatePending_ = true;
    taxiActivateTimer_ = 0.0f;
    // Do NOT mount, set onTaxiFlight_, or activate the client spline here.
    // CMSG_ACTIVATETAXI can be rejected by the server (insufficient gold, invalid
    // route, distance/state restrictions). Starting the flight speculatively meant
    // a rejection reply became indistinguishable from a stale/duplicate one once
    // updateClientTaxi() started running unconditionally every frame - the client
    // would just keep flying an unauthorized route. Everything that actually starts
    // the flight (mount, onTaxiFlight_, taxiClientActive_) now happens only in the
    // success branch of handleActivateTaxiReply(), once the server confirms.
    // taxiClientPath_ is still built below as pending data - see
    // startClientTaxiPath()'s comment.
    if (owner_.getSocket()) {
        sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    }

    if (owner_.taxiFlightStartCallbackRef()) {
        owner_.taxiFlightStartCallbackRef()();
    }
    if (isPreWotlk()) startClientTaxiPath(path);
}

// ============================================================
// Area Trigger Detection (moved from GameHandler)
// ============================================================

void MovementHandler::loadAreaTriggerDbc() {
    if (owner_.areaTriggerDbcLoadedRef()) return;

    auto* am = owner_.services().assetManager;
    // Don't latch "loaded" until the asset manager is actually ready - the
    // first checkAreaTriggers() tick can land well before asset manager
    // init finishes (observed in headless mode), and latching here first
    // would permanently strand areaTriggersRef() empty with no retry and
    // no warning, silently disabling every area trigger for the process's
    // lifetime.
    if (!am || !am->isInitialized()) return;
    owner_.areaTriggerDbcLoadedRef() = true;

    auto dbc = am->loadDBC("AreaTrigger.dbc");
    if (!dbc || !dbc->isLoaded()) {
        LOG_WARNING("Failed to load AreaTrigger.dbc");
        return;
    }

    owner_.areaTriggersRef().reserve(dbc->getRecordCount());
    for (uint32_t i = 0; i < dbc->getRecordCount(); i++) {
        GameHandler::AreaTriggerEntry at;
        at.id     = dbc->getUInt32(i, 0);
        at.mapId  = dbc->getUInt32(i, 1);
        // DBC stores positions in server/wire format (X=west, Y=north) - swap to canonical
        at.x = dbc->getFloat(i, 3);  // canonical X (north) = DBC field 3 (Y_wire)
        at.y = dbc->getFloat(i, 2);  // canonical Y (west)  = DBC field 2 (X_wire)
        at.z = dbc->getFloat(i, 4);
        at.radius    = dbc->getFloat(i, 5);
        at.boxLength = dbc->getFloat(i, 6);
        at.boxWidth  = dbc->getFloat(i, 7);
        at.boxHeight = dbc->getFloat(i, 8);
        at.boxYaw    = dbc->getFloat(i, 9);
        owner_.areaTriggersRef().push_back(at);
    }

    LOG_DEBUG("Loaded ", owner_.areaTriggersRef().size(), " area triggers from AreaTrigger.dbc");
}

void MovementHandler::checkAreaTriggers() {
    if (!owner_.isInWorld()) return;
    // Nor before a teleport is answered, for which the server has the player
    // where it was: the client answers from the teleport's own handler
    // (0x006ed0f0), and asks for the triggers in the updates after it.
    if (teleportAck_.pending() || worldportAck_.pending()) return;
    if (onTaxiFlight_ || taxiClientActive_) return;
    // The player's object there, and alive (0x006dbe30): not between two maps.
    const auto player = owner_.getEntityManager().getEntity(owner_.getPlayerGuid());
    if (!player || !player->isUnit() || static_cast<int32_t>(static_cast<const Unit&>(*player).getHealth()) <= 0) return;

    loadAreaTriggerDbc();

    // Sanity: if position is near map origin on Eastern Kingdoms (map 0),
    // something has corrupted movementInfo - skip area trigger check to
    // avoid firing Alterac/Hillsbrad triggers and causing a rogue teleport.
    if (isCorruptOriginPosition(owner_.getCurrentMapId(), movementInfo.x, movementInfo.y, movementInfo.z) &&
        !restoreWorldTransferFallbackIfNearOrigin("checkAreaTriggers")) {
        LOG_WARNING("checkAreaTriggers: position near map origin (", movementInfo.x, ", ", movementInfo.y, ", ",
                    movementInfo.z, ") on map 0 - skipping to avoid rogue teleport. onTransport=",
                    owner_.isOnTransport(), " transportGuid=0x", std::hex, owner_.playerTransportGuidRef(), std::dec);
        return;
    }

    const auto* at = areaTriggerWatch_.check(owner_.areaTriggersRef(), owner_.currentMapIdRef(), movementInfo.x,
                                             movementInfo.y, movementInfo.z);
    if (!at) return;
    // Where the mover is goes first (0x00724e70(0xee)): the server had it where
    // it last heard of it, up to half a second back and out of the trigger, and
    // a portal run into led nowhere.
    sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    network::Packet pkt(wireOpcode(Opcode::CMSG_AREATRIGGER));
    pkt.writeUInt32(at->id);
    lastAreaTriggerId_ = at->id;
    if (const auto* dest = findKnownAreaTriggerDestinationByTrigger(at->id)) {
        pendingAreaTriggerDestinationValid_ = true;
        pendingAreaTriggerDestinationMapId_ = dest->mapId;
        pendingAreaTriggerDestinationServerPos_ = glm::vec3(dest->serverX, dest->serverY, dest->serverZ);
        pendingAreaTriggerDestinationServerO_ = dest->serverO;
    }
    owner_.getSocket()->send(pkt);
    LOG_INFO("CMSG_AREATRIGGER: id=", at->id, " pos=(", movementInfo.x, ", ", movementInfo.y, ", ", movementInfo.z, ")");
}

// ============================================================
// Transport Attachments (moved from GameHandler)
// ============================================================

void MovementHandler::setTransportAttachment(uint64_t childGuid, ObjectType type, uint64_t transportGuid,
                                             const glm::vec3& localOffset, bool hasLocalOrientation,
                                             float localOrientation,
                                             bool offsetNeedsTransportResolution) {
    if (childGuid == 0 || transportGuid == 0) {
        return;
    }

    GameHandler::TransportAttachment& attachment = owner_.transportAttachmentsRef()[childGuid];
    attachment.type = type;
    attachment.transportGuid = transportGuid;
    attachment.localOffset = localOffset;
    attachment.hasLocalOrientation = hasLocalOrientation;
    attachment.localOrientation = localOrientation;
    attachment.offsetNeedsTransportResolution = offsetNeedsTransportResolution;
    attachment.moveFromLocal.reset();
}

void MovementHandler::clearTransportAttachment(uint64_t childGuid) {
    if (childGuid == 0) {
        return;
    }
    owner_.transportAttachmentsRef().erase(childGuid);
}

void MovementHandler::noteSeatOffset(uint64_t guid, uint64_t vehicle, const glm::vec3& wireOffset) {
    seatOffsets_[guid] = {.vehicle = vehicle, .offset = wireOffset};
}

std::optional<glm::vec3> MovementHandler::seatPlace(uint64_t guid) const {
    const uint64_t player = owner_.getPlayerGuid();
    const auto passenger = owner_.vehicleRoster().passenger(guid);
    if (!passenger || passenger->vehicleGuid == 0) return std::nullopt;
    const auto vehicle = owner_.getEntityManager().getEntity(passenger->vehicleGuid);
    if (!vehicle || !vehicle->isUnit()) return std::nullopt;
    glm::vec3 at(vehicle->getX(), vehicle->getY(), vehicle->getZ());
    float yaw = vehicle->getOrientation();
    if (passenger->vehicleGuid == player && owner_.activeMover() == player) {
        at = glm::vec3(movementInfo.x, movementInfo.y, movementInfo.z);
        yaw = movementInfo.orientation;
    }
    glm::vec3 offset(0.0f);
    if (const auto seat = seatOffsets_.find(guid); seat != seatOffsets_.end() &&
                                                   seat->second.vehicle == passenger->vehicleGuid) {
        offset = seat->second.offset;
    } else if (guid == player) {
        return std::nullopt;
    } else if (const auto held = owner_.transportAttachmentsRef().find(guid);
               held != owner_.transportAttachmentsRef().end() &&
               held->second.transportGuid == passenger->vehicleGuid) {
        // The create block's, as it came.
        offset = held->second.offsetNeedsTransportResolution
                     ? held->second.localOffset
                     : transport_local::toWire(false, held->second.localOffset);
    }
    return passengerPlace(at, yaw, offset);
}

void MovementHandler::carryVehiclePassengers() {
    // A passenger is its place on its vehicle wherever that goes (0x004f4460):
    // its distance, its click and its sounds were left where it got on. The
    // player's own as well, and what it is taken off the vehicle with: leaving
    // a transport takes the place through it into the world (0x006ec400 ->
    // 0x00722010).
    const uint64_t player = owner_.getPlayerGuid();
    for (const auto& [guid, passenger] : owner_.vehicleRoster().passengers()) {
        const auto entity = owner_.getEntityManager().getEntity(guid);
        if (!entity || entity->isActivelyMoving()) continue;
        const auto place = seatPlace(guid);
        if (!place) continue;
        if (guid == player && moverInfoOwner_ == 0) {
            movementInfo.x = place->x;
            movementInfo.y = place->y;
            movementInfo.z = place->z;
        }
        const glm::vec3 was(entity->getX(), entity->getY(), entity->getZ());
        if (glm::dot(*place - was, *place - was) > 0.0001f)
            entity->setPosition(place->x, place->y, place->z, entity->getOrientation());
    }
    std::erase_if(seatOffsets_, [this](const auto& seat) {
        const auto passenger = owner_.vehicleRoster().passenger(seat.first);
        return !passenger || passenger->vehicleGuid != seat.second.vehicle;
    });
}

void MovementHandler::updateAttachedTransportChildren(float /*deltaTime*/) {
    carryVehiclePassengers();
    if (!owner_.getTransportManager() || owner_.transportAttachmentsRef().empty()) {
        return;
    }

    constexpr float kPosEpsilonSq = 0.0001f;
    constexpr float kOriEpsilon = 0.001f;
    std::vector<uint64_t> stale;
    stale.reserve(8);

    for (auto& [childGuid, attachment] : owner_.transportAttachmentsRef()) {
        auto entity = owner_.getEntityManager().getEntity(childGuid);
        if (!entity) {
            stale.push_back(childGuid);
            continue;
        }

        ActiveTransport* transport = owner_.getTransportManager()->getTransport(attachment.transportGuid);
        if (!transport) {
            continue;
        }

        // The deck's turn since last carried turns the passenger's drawn
        // facing with it (Entity::carryModelFacing): its facing on the
        // transport is its own (0x004f42a0), the world one the deck's plus it.
        if (attachment.carriedFacing) {
            entity->carryModelFacing(core::coords::normalizeAngleRad(transport->facing - *attachment.carriedFacing));
        }
        attachment.carriedFacing = transport->facing;

        if (attachment.offsetNeedsTransportResolution) {
            attachment.localOffset = owner_.getTransportManager()->serverToTransportLocal(
                attachment.transportGuid, attachment.localOffset);
            attachment.offsetNeedsTransportResolution = false;
        }

        // A monster move on the transport under way is its own until it
        // ends: along its path, facing along it (0x0073c8e0), and only on
        // arriving the facing it asked for (0x006eb0b0). Putting it at its
        // destination here every frame ended the move as it began.
        //
        // It runs on the transport, though: the spline is set on the
        // movement whose place is the one on the transport (0x0098c770), and
        // the world's is that put through the transport each frame
        // (0x004f4460). So its ends go where the transport is now, and a
        // move on a moving deck stays on it.
        if (entity->isActivelyMoving() && onMoveSpline(childGuid)) {
            if (entity->onFramedPath()) {
                if (const auto frame = transportFrame(attachment.transportGuid)) entity->setMoveFrame(*frame);
            } else if (attachment.moveFromLocal) {
                auto* transports = owner_.getTransportManager();
                entity->setMoveEnds(
                    transports->getPlayerWorldPosition(attachment.transportGuid, *attachment.moveFromLocal),
                    transports->getPlayerWorldPosition(attachment.transportGuid, attachment.localOffset));
            }
            continue;
        }

        glm::vec3 composed = owner_.getTransportManager()->getPlayerWorldPosition(
            attachment.transportGuid, attachment.localOffset);

        float composedOrientation = entity->getOrientation();
        if (attachment.hasLocalOrientation) {
            // The transport's facing plus its own on it (0x004f42a0), so it
            // turns with the deck. This added the hull's drawn rotation to a
            // facing taken off the wire negated or a quarter turn round, and
            // each kind of transport draws its hull its own way.
            composedOrientation =
                transport_local::worldYaw(transport->facing, attachment.localOrientation);
        }

        glm::vec3 oldPos(entity->getX(), entity->getY(), entity->getZ());
        float oldOrientation = entity->getOrientation();
        glm::vec3 delta = composed - oldPos;
        const bool positionChanged = glm::dot(delta, delta) > kPosEpsilonSq;
        const bool orientationChanged = std::abs(composedOrientation - oldOrientation) > kOriEpsilon;
        if (!positionChanged && !orientationChanged) {
            continue;
        }

        entity->setPosition(composed.x, composed.y, composed.z, composedOrientation);

        if (attachment.type == ObjectType::UNIT) {
            if (owner_.creatureMoveCallbackRef()) {
                owner_.creatureMoveCallbackRef()(childGuid, composed.x, composed.y, composed.z, 0);
            }
        } else if (attachment.type == ObjectType::GAMEOBJECT) {
            if (owner_.gameObjectMoveCallbackRef()) {
                owner_.gameObjectMoveCallbackRef()(childGuid, composed.x, composed.y, composed.z, composedOrientation);
            }
        }
    }

    for (uint64_t guid : stale) {
        owner_.transportAttachmentsRef().erase(guid);
    }
}

// ============================================================
// Follow target (moved from GameHandler)
// ============================================================

void MovementHandler::followTarget() {
    if (owner_.getState() != WorldState::IN_WORLD) {
        LOG_WARNING("Cannot follow: not in world");
        return;
    }

    // ERR_GENERIC_NO_TARGET with nobody to follow, as FollowUnit()
    // (0x005224c0) says it for /follow on its own. These were English.
    if (owner_.getTargetGuid() == 0) {
        owner_.raiseUiError(interfaceString("ERR_GENERIC_NO_TARGET", "You have no target."));
        owner_.playGameErrorSound("ERR_GENERIC_NO_TARGET");
        return;
    }

    auto target = owner_.getTarget();
    if (!target) {
        owner_.raiseUiError(interfaceString("ERR_GENERIC_NO_TARGET", "You have no target."));
        owner_.playGameErrorSound("ERR_GENERIC_NO_TARGET");
        return;
    }

    followGuid(owner_.getTargetGuid());
}

// The follow state names a guid, not "whatever is targeted" - so the only
// thing a named follow needs is the guid to put in there. The name has
// already been turned into one by the time this is reached.
void MovementHandler::followGuid(uint64_t guid) {
    if (owner_.getState() != WorldState::IN_WORLD) {
        LOG_WARNING("Cannot follow: not in world");
        return;
    }

    auto target = owner_.getEntityManager().getEntity(guid);
    if (!target) {
        owner_.raiseUiError(interfaceString("ERR_GENERIC_NO_TARGET", "You have no target."));
        owner_.playGameErrorSound("ERR_GENERIC_NO_TARGET");
        return;
    }

    // 0x0072b4a0's follow case: a player on the player's side (0x00729bd0 -
    // not oneself, neither charmed, one faction group, not attackable), then
    // the player's own state - no health, stunned (UNIT_FIELD_FLAGS 0x40000),
    // channeling - each refused with its line.
    auto refuse = [&](const char* key, const char* enUS) {
        owner_.raiseUiError(interfaceString(key, enUS));
        owner_.playGameErrorSound(key);
    };
    const uint64_t me = owner_.getPlayerGuid();
    if (target->getType() != ObjectType::PLAYER || guid == me || !owner_.unitsCooperate(me, guid)) {
        refuse("ERR_INVALID_FOLLOW_TARGET", "You can't follow that unit.");
        return;
    }
    if (auto self = owner_.getEntityManager().getEntity(me)) {
        const auto field = [&](UF f) {
            const uint16_t i = fieldIndex(f);
            return i == 0xFFFF ? 0u : self->getField(i);
        };
        if (static_cast<int32_t>(field(UF::UNIT_FIELD_HEALTH)) < 1) {
            refuse("ERR_PLAYER_DEAD", "You can't do that when you're dead.");
            return;
        }
        if ((field(UF::UNIT_FIELD_FLAGS) & 0x40000u) != 0) {
            refuse("ERR_GENERIC_STUNNED", "You are stunned");
            return;
        }
        if (field(UF::UNIT_CHANNEL_SPELL) != 0) {
            refuse("ERR_TOOBUSYTOFOLLOW", "You're too busy to follow anything!");
            return;
        }
    }
    // The follow is click-to-move's kind 3 (0x00727400): any other ends
    // first, it is not started thirty yards or more away (0x00715c70, which
    // says ERR_AUTOFOLLOW_TOO_FAR), and from then on 0x007317a0 steers it -
    // stopping within three yards, setting off again past four and a half
    // (scaled by the run or walk speed over 7), and giving up at thirty.
    // No chat line: AUTOFOLLOW_BEGIN, fired by the start with the name
    // followed, is what says so (FrameXML's AutoFollowStatus).
    owner_.clickToMoveApproach(guid, click_to_move::Kind::Follow);
}

void MovementHandler::cancelFollow() {
    // A follow is click-to-move's kind 3: ended as 0x007272c0(0, 1) ends it,
    // with AUTOFOLLOW_END - and no chat line, AUTOFOLLOW_END being what says
    // so (AUTOFOLLOWSTOP).
    owner_.stopClickToMoveFollow();
}

// ============================================================
// The active mover
// ============================================================

uint64_t MovementHandler::activeMover() const {
    return activeMover_ ? *activeMover_ : owner_.getPlayerGuid();
}

bool MovementHandler::moverPossessed() const {
    const uint64_t mover = activeMover();
    return moverPossessed_ && mover != 0 && mover != owner_.getPlayerGuid();
}

const MovementInfo& MovementHandler::playerMovement() const { return movementOf(owner_.getPlayerGuid()); }

bool MovementHandler::unitHasClientControl(uint64_t guid) const {
    if (guid == 0) return false;
    if (guid == owner_.getPlayerGuid()) return serverMovementAllowed_;
    const auto it = unitControl_.find(guid);
    return it != unitControl_.end() && it->second;
}

void MovementHandler::endFallForServerMove(uint64_t guid, bool hasPath) {
    // A move of the server's ends the fall the unit is in, unless it is on one
    // already (0x0073c8e0 -> 0x006ed7e0(0, 0) -> 0x006e9980 -> 0x00988490); a
    // stop in any case (0x006f11b0 -> 0x006ed7e0(0, 1)). What steps the player
    // - or what it drives - is told.
    const uint64_t stepped = activeMover() != 0 ? activeMover() : owner_.getPlayerGuid();
    if (guid == stepped && (!hasPath || !onMoveSpline(guid))) {
        // Its flags with it, without a landing said or played.
        if ((movementInfo.flags & (static_cast<uint32_t>(MovementFlags::FALLING) |
                                   static_cast<uint32_t>(MovementFlags::FALLINGFAR))) != 0) {
            movementInfo.time = movementClockMs();
            endFall(movementInfo, endFallTimer());
        }
        if (owner_.fallEndsCallbackRef()) owner_.fallEndsCallbackRef()();
    }
}

void MovementHandler::noteMoveSpline(uint64_t guid, bool hasPath, uint32_t splineFlags, uint32_t durationMs,
                                     uint32_t splineId) {
    endFallForServerMove(guid, hasPath);
    const uint32_t now = movementClockMs();
    if (auto spline = vehicle::moveSplineFrom(hasPath, splineFlags, durationMs, now)) {
        spline->splineId = splineId;
        moveSplines_[guid] = *spline;
    } else {
        moveSplines_.erase(guid);
    }
    // Those that ran out are of no more use.
    if (moveSplines_.size() > 256)
        std::erase_if(moveSplines_, [now](const auto& kv) { return !vehicle::moveSplineRunning(kv.second, now); });
}

std::optional<glm::mat4> MovementHandler::transportFrame(uint64_t transportGuid) const {
    auto* transports = owner_.getTransportManager();
    if (!transports || !transports->getTransport(transportGuid)) return std::nullopt;
    const auto at = [&](const glm::vec3& wire) {
        return transports->getPlayerWorldPosition(transportGuid, transports->serverToTransportLocal(transportGuid, wire));
    };
    const glm::vec3 origin = at(glm::vec3(0.0f));
    glm::mat4 frame(1.0f);
    frame[0] = glm::vec4(at(glm::vec3(1.0f, 0.0f, 0.0f)) - origin, 0.0f);
    frame[1] = glm::vec4(at(glm::vec3(0.0f, 1.0f, 0.0f)) - origin, 0.0f);
    frame[2] = glm::vec4(at(glm::vec3(0.0f, 0.0f, 1.0f)) - origin, 0.0f);
    frame[3] = glm::vec4(origin, 1.0f);
    return frame;
}

void MovementHandler::takeBlockSpline(const UpdateBlock& block, const std::shared_ptr<Entity>& entity,
                                      uint32_t queuedMs) {
    if (isPreWotlk() || !entity || !entity->isUnit() || !block.hasMovement) return;
    // Whatever it was on is gone (0x006f1520: 0x0098b730, or overwritten).
    moveSplines_.erase(block.guid);
    if (!block.spline) return;
    const SplineBlockData& spline = *block.spline;
    const bool self = block.guid == owner_.getPlayerGuid();
    // Only a unit whose flags say forward or back is moved along it
    // (0x0098ca00); without any way of moving it is not stepped at all
    // (0x006f1520 -> 0x006eaf50(1)).
    if ((block.moveFlags & 0x3u) == 0) return;
    // On a transport the points are on it. A vehicle's passenger is drawn in
    // its seat, and the player's own riding is its own.
    const bool onTransport = block.onTransport && block.transportGuid != 0;
    if (onTransport) {
        const auto carrier = owner_.getEntityManager().getEntity(block.transportGuid);
        if (self || (carrier && carrier->isUnit()) || vehicle::isVehicleGuid(block.transportGuid) ||
            vehicle::isPlayerGuid(block.transportGuid)) {
            return;
        }
    }

    const auto place = [onTransport](const glm::vec3& wire) {
        return onTransport ? wire : core::coords::serverToCanonical(wire);
    };
    Entity::BlockSpline run;
    run.points.reserve(spline.waypoints.size());
    for (const glm::vec3& point : spline.waypoints) run.points.push_back(place(point));
    run.endPoint = place(spline.endPoint);
    run.curved = (spline.splineFlags & SplineFlagWotlk::UNCOMPRESSED_MASK) != 0;
    run.curvedLengths = spline.splineMode != 0;
    run.cyclic = (spline.splineFlags & SplineFlagWotlk::CYCLIC) != 0;
    run.enterCycle = run.cyclic && (spline.splineFlags & SplineFlagWotlk::ENTER_CYCLE) != 0;
    run.framed = onTransport;
    run.durationMs = spline.duration;
    run.durationFactor = spline.durationFactor;
    run.nextDurationFactor = spline.nextDurationFactor;
    // It began as long before the block was read as the block says
    // (0x004f4b50: +0x24 = now - passed).
    run.passedMs = spline.timePassed + queuedMs;
    run.arcs = spline.hasParabolic;
    run.arcAcceleration = spline.verticalAcceleration;
    run.arcStartMs = spline.parabolicStartTime;
    if ((spline.splineFlags & (SplineFlagWotlk::FALLING | SplineFlagWotlk::PARABOLIC)) == SplineFlagWotlk::FALLING) {
        // From where its fall began (+0x84), which only a block with FALLING
        // says (0x00988990); without it the unit is at the end at once.
        if (block.moveFlags & static_cast<uint32_t>(MovementFlags::FALLING)) {
            run.fallTop = (onTransport ? block.transportZ : block.z) +
                          fallDistance(static_cast<float>(block.fallTime) * 0.001f, block.fallVerticalSpeed, false);
        } else {
            run.over = true;
        }
    }
    const uint32_t durationMs = Entity::scaledDuration(run.durationMs, run.durationFactor);
    if (run.over) run.passedMs = std::max(run.passedMs, durationMs);

    noteMoveSpline(block.guid, true, spline.splineFlags, durationMs, spline.splineId);
    const auto kept = moveSplines_.find(block.guid);
    if (kept == moveSplines_.end()) return;  // let go already (0x400)
    kept->second.startMs -= run.passedMs;
    // A frozen one is kept and goes nowhere (0x0098ca00).
    if ((spline.splineFlags & SplineFlagWotlk::FROZEN) != 0) {
        kept->second.cyclic = true;
        return;
    }

    const float facingBefore = entity->getOrientation();
    entity->startBlockSpline(run);
    entity->moveBeganAfter(owner_.sinceUnitsAdvanced());
    const bool keepsFacing =
        (spline.splineFlags & (SplineFlagWotlk::FALLING | SplineFlagWotlk::ORIENT_FIXED)) != 0;
    const auto frame = onTransport ? transportFrame(block.transportGuid) : std::nullopt;
    if (frame) entity->setMoveFrame(*frame);
    entity->faceAlongMove(std::nullopt,
                          keepsFacing || spline.waypoints.size() < 3 ? std::optional<float>(facingBefore) : std::nullopt,
                          (spline.splineFlags & SplineFlagWotlk::ORIENT_INVERSED) != 0);
    // Along the path once the transport it lies on is there.
    if (onTransport && !frame) entity->setOrientation(facingBefore);
    if (onTransport) {
        auto* transports = owner_.getTransportManager();
        const bool there = transports && transports->getTransport(block.transportGuid);
        const glm::vec3 end = entity->framedEnd();
        owner_.setTransportAttachment(block.guid, entity->getType(), block.transportGuid,
                                      there ? transports->serverToTransportLocal(block.transportGuid, end) : end,
                                      spline.hasFinalAngle, spline.finalAngle, !there);
    }
    // What it faces at the end (0x006eb0b0), the target as it is then.
    const auto facingTo = [](const glm::vec3& from, const glm::vec3& to) -> std::optional<float> {
        const float dx = to.x - from.x;
        const float dy = to.y - from.y;
        if (std::abs(dx) <= 0.01f && std::abs(dy) <= 0.01f) return std::nullopt;
        return std::atan2(-dy, dx);
    };
    const uint64_t transportGuid = onTransport ? block.transportGuid : 0;
    if (spline.hasFinalAngle) {
        entity->faceOnArrival([this, transportGuid, angle = spline.finalAngle](const glm::vec3&) -> std::optional<float> {
            if (transportGuid == 0) return core::coords::serverToCanonicalYaw(angle);
            auto* transports = owner_.getTransportManager();
            return transport_local::worldYaw(
                transports ? transports->transportFacing(transportGuid).value_or(0.0f) : 0.0f, angle);
        });
    } else if (spline.hasFinalTarget) {
        entity->faceOnArrival([this, facingTo, guid = spline.finalTarget](const glm::vec3& at) -> std::optional<float> {
            const auto target = owner_.getEntityManager().getEntity(guid);
            if (!target) return std::nullopt;
            return facingTo(at, glm::vec3(target->getX(), target->getY(), target->getZ()));
        });
    } else if (spline.hasFinalPoint) {
        entity->faceOnArrival([this, facingTo, transportGuid, spot = spline.finalPoint](const glm::vec3& at) -> std::optional<float> {
            if (transportGuid == 0) return facingTo(at, core::coords::serverToCanonical(spot));
            const auto frame = transportFrame(transportGuid);
            if (!frame) return std::nullopt;
            return facingTo(at, glm::vec3(*frame * glm::vec4(spot, 1.0f)));
        });
    }
    // Its flags are the block's, and arriving takes forward and back off.
    entity->setMonsterMoveFlags(block.moveFlags, !run.cyclic);
}

bool MovementHandler::onMoveSpline(uint64_t guid) {
    const auto it = moveSplines_.find(guid);
    if (it == moveSplines_.end()) return false;
    if (vehicle::moveSplineRunning(it->second, movementClockMs())) return true;
    // The active mover's is kept for updateMoverSplineDone to report.
    if (guid != activeMover()) moveSplines_.erase(it);
    return false;
}

void MovementHandler::updateHeartbeat() {
    if (!owner_.isInWorld()) return;
    const uint32_t now = movementClockMs();
    const bool onServerSpline = owner_.isOnTaxiFlight() || onMoveSpline(activeMover());
    if (heartbeatDue(movementInfo.flags, onServerSpline, now, lastMoveSendMs_)) sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
}

void MovementHandler::updateMoverSplineDone() {
    if (isPreWotlk()) return;
    const uint64_t mover = activeMover();
    const auto it = moveSplines_.find(mover);
    if (it == moveSplines_.end() || vehicle::moveSplineRunning(it->second, movementClockMs())) return;
    const uint32_t splineId = it->second.splineId;
    moveSplines_.erase(it);
    // Where it ended (0x006e9470): the player's own movement is the last
    // frame's. On a flight the unit stays there, and does not drift on, until
    // the server moves it.
    if (mover == owner_.getPlayerGuid()) {
        if (auto player = owner_.getEntityManager().getEntity(mover)) {
            const glm::vec3 end(player->getLatestX(), player->getLatestY(), player->getLatestZ());
            if (onTaxiFlight_) player->setPosition(end.x, end.y, end.z, player->getOrientation());
            movementInfo.x = end.x;
            movementInfo.y = end.y;
            movementInfo.z = end.z;
        }
    }
    // It falls from there (0x006eae70: the spline freed, then 0x0098b710) -
    // onto the ground right under it, or from as high as the spline left it.
    if (owner_.placedFallCallbackRef()) owner_.placedFallCallbackRef()();
    // 0x0071f210: the mover's movement block and the spline's id.
    if (owner_.getSocket()) sendMovement(Opcode::CMSG_MOVE_SPLINE_DONE, splineId);
}

std::optional<uint32_t> MovementHandler::keptMoveSplineFlags(uint64_t guid) const {
    // A spline that has run out is marked finished (0x400) rather than
    // freed (0x0098ba90, 0x006eaf50).
    const auto it = moveSplines_.find(guid);
    if (it == moveSplines_.end()) return std::nullopt;
    return it->second.flags | (vehicle::moveSplineRunning(it->second, movementClockMs()) ? 0u : 0x400u);
}

void MovementHandler::updateMoverTimeSkip(float deltaTime) {
    // 0x006f1490, each frame (the idle event, slot 6) -> 0x006f13e0: a mover
    // whose movement is suspended because the world under it is not loaded
    // (flag 0x8000000, set by 0x00711ab0) does not move; for the active
    // mover the frame's time is sent as skipped (0x00717d90) and added to
    // the base its movement times count from, so the next move's time
    // leaves the stall out. Not while it rides a transport (0x00717d90).
    if (isPreWotlk() || owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) {
        timeSkipCarryMs_ = 0.0f;
        return;
    }
    auto* renderer = owner_.services().renderer;
    auto* camera = renderer ? renderer->getCameraController() : nullptr;
    if (!camera || camera->isOnTaxi() || movementInfo.transportGuid != 0 ||
        !camera->followTargetOverUnstreamedGround()) {
        timeSkipCarryMs_ = 0.0f;
        return;
    }
    timeSkipCarryMs_ += deltaTime * 1000.0f;
    const auto skipped = static_cast<uint32_t>(timeSkipCarryMs_);
    if (skipped == 0) return;
    timeSkipCarryMs_ -= static_cast<float>(skipped);
    const uint64_t mover = activeMover();
    if (mover == 0) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_MOVE_TIME_SKIPPED));
    packet.writePackedGuid(mover);
    packet.writeUInt32(skipped);
    owner_.getSocket()->send(packet);
    movementClockStart_ += std::chrono::milliseconds(skipped);
}

std::optional<uint32_t> MovementHandler::moveSplineFlags(uint64_t guid) {
    if (!onMoveSpline(guid)) return std::nullopt;
    return moveSplines_[guid].flags;
}

uint64_t MovementHandler::playerFarsight() const {
    const uint16_t index = fieldIndex(UF::PLAYER_FARSIGHT);
    auto player = owner_.getEntityManager().getEntity(owner_.getPlayerGuid());
    if (!player || index == 0xFFFF) return 0;
    return (static_cast<uint64_t>(player->getField(static_cast<uint16_t>(index + 1))) << 32) |
           player->getField(index);
}

uint64_t MovementHandler::cameraTarget() const {
    const uint64_t farsight = playerFarsight();
    if (farsight != 0 && owner_.getEntityManager().getEntity(farsight)) return farsight;
    return owner_.getPlayerGuid();
}

const MovementInfo& MovementHandler::movementOf(uint64_t guid) const {
    const uint64_t player = owner_.getPlayerGuid();
    const uint64_t owner = moverInfoOwner_ != 0 ? moverInfoOwner_ : player;
    if (guid == player && owner != player) return heldPlayerMovement_;
    return movementInfo;
}

MovementInfo MovementHandler::movementNow(uint64_t guid) const {
    // A forced change is answered from the unit's movement as it stands at
    // that moment - 0x006ed0f0 answers it where the step has brought the unit
    // to, and 0x00990420 lets the answers by while a spline carries it. What
    // is kept here is a frame old; and where a load came between, as old as
    // the load: the answer to the collision height of a dismount put a flight
    // two hundred yards back, on the server.
    MovementInfo info = movementOf(guid);
    // One run out and reported (updateMoverSplineDone) carries it no more.
    if (info.hasFlag(MovementFlags::ONTRANSPORT) || !moveSplines_.count(guid)) return info;
    const auto unit = owner_.getEntityManager().getEntity(guid);
    if (!unit) return info;
    if (const auto at = unit->splinePlaceAfter(owner_.sinceUnitsAdvanced())) {
        info.x = at->position.x;
        info.y = at->position.y;
        info.z = at->position.z;
        info.orientation = at->orientation;
    }
    return info;
}

MovementInfo* MovementHandler::movementFor(uint64_t guid) {
    const uint64_t player = owner_.getPlayerGuid();
    const uint64_t owner = moverInfoOwner_ != 0 ? moverInfoOwner_ : player;
    if (guid == owner) return &movementInfo;
    if (guid == player) return &heldPlayerMovement_;
    return nullptr;
}

void MovementHandler::resetActiveMover() {
    // 0x006e7f50 -> 0x00513880 -> 0x00717c50: the player moves itself again
    // and controls nothing else.
    const uint64_t player = owner_.getPlayerGuid();
    if (moverInfoOwner_ != 0 && moverInfoOwner_ != player) movementInfo = heldPlayerMovement_;
    activeMover_ = player;
    moverInfoOwner_ = 0;
    moverPossessed_ = false;
    unitControl_.clear();
    pendingControl_.reset();
    farSightAnnounced_ = false;
    pitchingToTarget_ = false;
    aimKeys_ = 0;
    aimKeyPitching_ = false;
    heldPitchStart_.reset();
    pitchStartSent_ = false;
    heldTurnStart_.reset();
    turnStartSent_ = false;
}

void MovementHandler::setActiveMover(uint64_t guid) {
    const uint64_t old = activeMover();
    if (old == guid) return;
    const uint64_t player = owner_.getPlayerGuid();
    auto& entities = owner_.getEntityManager();
    if (old != 0 && entities.getEntity(old) && owner_.getState() == WorldState::IN_WORLD && owner_.getSocket()) {
        // 0x00729010 -> 0x006ee920 -> 0x006ed7e0 -> 0x006e9980: the old
        // mover's movement stops and says so, as itself.
        constexpr uint32_t kMoving = static_cast<uint32_t>(MovementFlags::FORWARD) |
                                     static_cast<uint32_t>(MovementFlags::BACKWARD);
        constexpr uint32_t kStrafing = static_cast<uint32_t>(MovementFlags::STRAFE_LEFT) |
                                       static_cast<uint32_t>(MovementFlags::STRAFE_RIGHT);
        constexpr uint32_t kTurning = static_cast<uint32_t>(MovementFlags::TURN_LEFT) |
                                      static_cast<uint32_t>(MovementFlags::TURN_RIGHT);
        constexpr uint32_t kPitching = static_cast<uint32_t>(MovementFlags::PITCH_UP) |
                                       static_cast<uint32_t>(MovementFlags::PITCH_DOWN);
        // ... and its +0xbc: only where it has no move spline, or one let go.
        if (!onMoveSpline(old)) {
            if (movementInfo.flags & kTurning) sendMovement(Opcode::MSG_MOVE_STOP_TURN);
            if (movementInfo.flags & kPitching) sendMovement(Opcode::MSG_MOVE_STOP_PITCH);
            if (movementInfo.flags & kStrafing) sendMovement(Opcode::MSG_MOVE_STOP_STRAFE);
            if (movementInfo.flags & kMoving) sendMovement(Opcode::MSG_MOVE_STOP);
            // The player no longer moves itself (0x00724e70(0x2d1)).
            if (old == player) {
                MovementInfo wire = moverWireInfo();
                owner_.getSocket()->send(MovementPacket::build(Opcode::CMSG_MOVE_NOT_ACTIVE_MOVER, wire, player));
            }
        }
    }
    pitchingToTarget_ = false;
    heldPitchStart_.reset();
    pitchStartSent_ = false;
    heldTurnStart_.reset();
    turnStartSent_ = false;
    aimKeyPitching_ = false;
    activeMover_ = guid;
    if (guid == 0) return;
    LOG_INFO("Active mover: 0x", std::hex, guid, std::dec);
    if (owner_.getSocket()) owner_.getSocket()->send(SetActiveMoverPacket::build(guid));

    // movementInfo becomes the new mover's: the player's comes back where the
    // unit it moved in its stead is - it rode there - and another unit's is
    // where it stands, as its own movement said last.
    const uint64_t owner = moverInfoOwner_ != 0 ? moverInfoOwner_ : player;
    if (owner != guid) {
        constexpr uint32_t kTransient = 0x000000FFu | static_cast<uint32_t>(MovementFlags::ONTRANSPORT) |
                                        static_cast<uint32_t>(MovementFlags::FALLING) |
                                        static_cast<uint32_t>(MovementFlags::FALLINGFAR);
        const MovementInfo last = movementInfo;
        if (owner == player) heldPlayerMovement_ = movementInfo;
        if (guid == player) {
            movementInfo = heldPlayerMovement_;
            // A vehicle carried the player to where it stopped, on its seat;
            // a possessed unit went off without it, and the player is where
            // it stood.
            if (!moverPossessed_) {
                const auto self = entities.getEntity(player);
                movementInfo.x = self ? self->getX() : last.x;
                movementInfo.y = self ? self->getY() : last.y;
                movementInfo.z = self ? self->getZ() : last.z;
            }
            movementInfo.flags &= ~kTransient;
            moverInfoOwner_ = 0;
            moverPossessed_ = false;
        } else {
            moverPossessed_ = vehicle::moverIsPossessed(guid, player, owner_.vehicleRoster().vehicleId(guid),
                                                        owner_.vehicleRoster().passenger(player).has_value());
            MovementInfo unitInfo;
            if (auto unit = entities.getEntity(guid)) {
                unitInfo.x = unit->getX();
                unitInfo.y = unit->getY();
                unitInfo.z = unit->getZ();
                unitInfo.orientation = unit->getOrientation();
                unitInfo.flags = unit->getReportedMoveFlags() & ~kTransient;
            }
            unitInfo.flags2 = owner_.vehicleRoster().moveFlags2(guid);
            unitInfo.pitch = owner_.vehicleRoster().pitch(guid);
            movementInfo = unitInfo;
            moverInfoOwner_ = guid;
            if (auto unit = entities.getEntity(guid)) {
                LOG_INFO("Active mover 0x", std::hex, guid, " flags 0x", unitInfo.flags, " flags2 0x",
                         unitInfo.flags2, std::dec, " run ", unit->movementSpeeds().run, " possessed ",
                         moverPossessed_ ? 1 : 0);
            }
        }
        isFalling_ = false;
        fallStartMs_ = 0;
    }
    // 0x00747f40: VEHICLE_UPDATE, and the new mover's aim.
    owner_.fireAddonEvent("VEHICLE_UPDATE", {});
    owner_.announceVehicleAim();
}

void MovementHandler::applyControlUpdate(uint64_t guid, bool allow) {
    const uint64_t player = owner_.getPlayerGuid();
    if (guid == player) {
        bool changed = (serverMovementAllowed_ != allow);
        serverMovementAllowed_ = allow;
        // 0x0071c930: the player's control - PLAYER_CONTROL_LOST / GAINED
        // (0x00520fe0) - and its movement stopped where it moves itself.
        if (changed && !allow) {
            if (activeMover() == player) {
                movementInfo.flags &= ~(static_cast<uint32_t>(MovementFlags::FORWARD) |
                                        static_cast<uint32_t>(MovementFlags::BACKWARD) |
                                        static_cast<uint32_t>(MovementFlags::STRAFE_LEFT) |
                                        static_cast<uint32_t>(MovementFlags::STRAFE_RIGHT) |
                                        static_cast<uint32_t>(MovementFlags::TURN_LEFT) |
                                        static_cast<uint32_t>(MovementFlags::TURN_RIGHT));
                owner_.sendMovement(Opcode::MSG_MOVE_STOP);
                owner_.sendMovement(Opcode::MSG_MOVE_STOP_STRAFE);
                owner_.sendMovement(Opcode::MSG_MOVE_STOP_TURN);
                owner_.sendMovement(Opcode::MSG_MOVE_STOP_SWIM);
            }
            owner_.fireAddonEvent("PLAYER_CONTROL_LOST", {});
        } else if (changed && allow) {
            owner_.fireAddonEvent("PLAYER_CONTROL_GAINED", {});
        }
    } else {
        unitControl_[guid] = allow;
    }
    // 0x0072cca0: the mover follows.
    const uint64_t target = cameraTarget();
    if (const auto mover = vehicle::moverAfterControlUpdate(guid, allow, target, activeMover(),
                                                            unitHasClientControl(target))) {
        setActiveMover(*mover);
    }
}

void MovementHandler::onUnitCreated(uint64_t guid) {
    if (pendingControl_ && pendingControl_->first == guid) {
        const bool allow = pendingControl_->second;
        pendingControl_.reset();
        applyControlUpdate(guid, allow);
    }
}

void MovementHandler::onFarsightChanged(uint64_t farsight) {
    // 0x006e2880: CMSG_FAR_SIGHT the first time the object is there to look
    // through, and 0 once it is cleared; the mover by what the player looks
    // through.
    const uint64_t player = owner_.getPlayerGuid();
    const bool known = farsight != 0 && owner_.getEntityManager().getEntity(farsight) != nullptr;
    if (const auto say = vehicle::farSightAnnouncement(farsight, known, farSightAnnounced_)) {
        farSightAnnounced_ = *say;
        if (owner_.getSocket() && wireOpcode(Opcode::CMSG_FAR_SIGHT) != 0xFFFF)
            owner_.getSocket()->send(VehiclePackets::farSight(*say));
    }
    setActiveMover(vehicle::moverAfterFarsight(farsight, known, unitHasClientControl(farsight), player,
                                               unitHasClientControl(player)));
}

void MovementHandler::clearFarSightImmediate() {
    // 0x006e2b00: only once CMSG_FAR_SIGHT has said so (+0x1858 0x1). The
    // camera then asks again which vehicle it rides and moves for the
    // player's seat (0x00604b90, 0x0074c4a0 -> 0x0074c0e0, which returns at
    // once for the seat it last moved for); here the camera works out its
    // vehicle every frame and moves for a seat as the seat changes.
    if (!farSightAnnounced_) return;
    auto player = owner_.getEntityManager().getEntity(owner_.getPlayerGuid());
    const uint16_t field = fieldIndex(UF::PLAYER_FARSIGHT);
    if (!player || field == 0xFFFF) return;
    onFarsightChanged((static_cast<uint64_t>(player->getField(static_cast<uint16_t>(field + 1))) << 32) |
                      player->getField(field));
}

MovementSpeeds MovementHandler::moverSpeeds() const {
    const uint64_t mover = activeMover();
    if (mover != owner_.getPlayerGuid()) {
        if (auto unit = owner_.getEntityManager().getEntity(mover)) return unit->movementSpeeds();
    }
    MovementSpeeds speeds;
    speeds.walk = serverWalkSpeed_;
    speeds.run = serverRunSpeed_;
    speeds.runBack = serverRunBackSpeed_;
    speeds.swim = serverSwimSpeed_;
    speeds.swimBack = serverSwimBackSpeed_;
    speeds.flight = serverFlightSpeed_;
    speeds.flightBack = serverFlightBackSpeed_;
    speeds.turnRate = serverTurnRate_;
    speeds.pitchRate = serverPitchRate_;
    return speeds;
}

glm::vec3 MovementHandler::transportOffsetToWire(uint64_t transportGuid, const glm::vec3& localOffset) const {
    if (const TransportManager* transports = owner_.getTransportManager())
        return transports->transportLocalToServer(transportGuid, localOffset);
    return core::coords::canonicalToServer(localOffset);
}

float MovementHandler::transportFacingToWire(uint64_t transportGuid, float canonicalYaw, float kept) const {
    const TransportManager* transports = owner_.getTransportManager();
    const std::optional<float> facing = transports ? transports->transportFacing(transportGuid) : std::nullopt;
    return facing ? transport_local::localFacing(*facing, canonicalYaw) : kept;
}

MovementInfo MovementHandler::moverWireInfo() {
    MovementInfo wire = movementNow(moverInfoOwner_ != 0 ? moverInfoOwner_ : owner_.getPlayerGuid());
    wire.time = nextMovementTimestampMs();
    const glm::vec3 serverPos = core::coords::canonicalToServer(glm::vec3(wire.x, wire.y, wire.z));
    wire.x = serverPos.x;
    wire.y = serverPos.y;
    wire.z = serverPos.z;
    if (wire.hasFlag(MovementFlags::ONTRANSPORT))
        wire.transportO = transportFacingToWire(wire.transportGuid, wire.orientation, wire.transportO);
    wire.orientation = core::coords::canonicalToServerYaw(wire.orientation);
    if (wire.hasFlag(MovementFlags::ONTRANSPORT)) {
        wire.transportTime = wire.time;
        wire.transportTime2 = wire.time;
        const glm::vec3 t =
            transportOffsetToWire(wire.transportGuid, glm::vec3(wire.transportX, wire.transportY, wire.transportZ));
        wire.transportX = t.x;
        wire.transportY = t.y;
        wire.transportZ = t.z;
    }
    return wire;
}

float MovementHandler::moverPitchRate() const {
    const float rate = moverSpeeds().pitchRate;
    return rate > 0.0f ? rate : core::coords::PI;
}

void MovementHandler::startMoverPitch(bool up) {
    sendMovement(up ? Opcode::MSG_MOVE_START_PITCH_UP : Opcode::MSG_MOVE_START_PITCH_DOWN);
}

void MovementHandler::stopMoverPitch() {
    sendMovement(Opcode::MSG_MOVE_STOP_PITCH);
}

void MovementHandler::requestMoverPitch(float pitch) {
    // 0x005fb3a0 for the mover, held in its row's limits by the caller.
    if ((movementInfo.flags2 & 0x0010) == 0) {
        // At once (event 0x14 -> MSG_MOVE_SET_PITCH).
        pitchingToTarget_ = false;
        movementInfo.pitch = pitch;
        sendMovement(Opcode::MSG_MOVE_SET_PITCH);
        return;
    }
    // Over time (0x0072d470 -> 0x006ef490, event 0x36 -> 0x006ef7a0): up or
    // down at its pitch rate, and stopped (event 0x33) when it is there.
    if (pitchingToTarget_ && pitchTarget_ == pitch) return;  // on its way there already
    const auto way = vehicle::pitchOverTime(movementInfo.pitch, pitch, moverPitchRate());
    pitchTarget_ = pitch;
    pitchingToTarget_ = true;
    pitchStopMs_ = movementClockMs() + way.durationMs;
    startMoverPitch(way.up);
}

void MovementHandler::setMoverPitchNow(float pitch) {
    const vehicle::VehicleRecord* row = owner_.vehicleRowOf(activeMover());
    if (row && (row->flags & vehicle::kVehicleFlagPitchLimits)) pitch = vehicle::clampAim(row, pitch);
    pitchingToTarget_ = false;
    movementInfo.pitch = pitch;
    sendMovement(Opcode::MSG_MOVE_SET_PITCH);
}

void MovementHandler::setAimKey(bool up, bool held) {
    const int bit = up ? 1 : 2;
    aimKeys_ = held ? (aimKeys_ | bit) : (aimKeys_ & ~bit);
    // 0x005fb1a0: the keys' way, the input control's 0x80000 while it pitches.
    const int way = vehicle::aimKeyDirection((aimKeys_ & 1) != 0, (aimKeys_ & 2) != 0);
    if (way != 0) {
        pitchingToTarget_ = false;
        if (!aimKeyPitching_) {
            startMoverPitch(way > 0);
            aimKeyPitching_ = true;
        }
    } else if (aimKeyPitching_) {
        stopMoverPitch();
        aimKeyPitching_ = false;
        // VEHICLE_ANGLE_UPDATE with the mover's pitch (0x005fa910).
        owner_.fireVehicleAngleUpdate(movementInfo.pitch);
    }
}

bool MovementHandler::updateMoverPitch(float deltaTime) {
    const uint32_t now = movementClockMs();
    // 0x00721b90: a full-speed turner's START_TURN, then a full-speed
    // pitcher's START_PITCH, after 100 ms.
    if (heldTurnStart_ && now - heldTurnStartMs_ > vehicle::kPitchStartHoldMs) {
        const Opcode held = *heldTurnStart_;
        heldTurnStart_.reset();
        flushingPitchStart_ = true;
        sendMovement(held);
        flushingPitchStart_ = false;
    }
    if (heldPitchStart_ && now - heldPitchStartMs_ > vehicle::kPitchStartHoldMs) {
        const Opcode held = *heldPitchStart_;
        heldPitchStart_.reset();
        flushingPitchStart_ = true;
        sendMovement(held);
        flushingPitchStart_ = false;
    }
    constexpr uint32_t kPitching = static_cast<uint32_t>(MovementFlags::PITCH_UP) |
                                   static_cast<uint32_t>(MovementFlags::PITCH_DOWN);
    if ((movementInfo.flags & kPitching) == 0) return false;
    // The mover pitches at its pitch rate, in its vehicle row's limits
    // (0x006ef490's clamp).
    const float before = movementInfo.pitch;
    const float sign = movementInfo.hasFlag(MovementFlags::PITCH_UP) ? 1.0f : -1.0f;
    float pitch = before + sign * moverPitchRate() * deltaTime;
    if (pitchingToTarget_) {
        if ((sign > 0.0f && pitch >= pitchTarget_) || (sign < 0.0f && pitch <= pitchTarget_) ||
            static_cast<int32_t>(now - pitchStopMs_) >= 0) {
            pitch = pitchTarget_;
            movementInfo.pitch = pitch;
            pitchingToTarget_ = false;
            stopMoverPitch();  // event 0x33
            return pitch != before;
        }
    }
    const vehicle::VehicleRecord* row = owner_.vehicleRowOf(activeMover());
    if (row && (row->flags & vehicle::kVehicleFlagPitchLimits)) pitch = vehicle::clampAim(row, pitch);
    movementInfo.pitch = pitch;
    return pitch != before;
}

void MovementHandler::stopMoverForRequest() {
    constexpr uint32_t kTurning = static_cast<uint32_t>(MovementFlags::TURN_LEFT) |
                                  static_cast<uint32_t>(MovementFlags::TURN_RIGHT);
    constexpr uint32_t kPitching = static_cast<uint32_t>(MovementFlags::PITCH_UP) |
                                   static_cast<uint32_t>(MovementFlags::PITCH_DOWN);
    constexpr uint32_t kStrafing = static_cast<uint32_t>(MovementFlags::STRAFE_LEFT) |
                                   static_cast<uint32_t>(MovementFlags::STRAFE_RIGHT);
    if (movementInfo.flags & kTurning) sendMovement(Opcode::MSG_MOVE_STOP_TURN);
    if (movementInfo.flags & kPitching) sendMovement(Opcode::MSG_MOVE_STOP_PITCH);
    if (movementInfo.flags & kStrafing) sendMovement(Opcode::MSG_MOVE_STOP_STRAFE);
    movementInfo.flags &= ~(static_cast<uint32_t>(MovementFlags::FORWARD) |
                            static_cast<uint32_t>(MovementFlags::BACKWARD) | kTurning | kPitching | kStrafing);
    pitchingToTarget_ = false;
}

void MovementHandler::sendChangeSeatsOnControlledVehicle(uint64_t vehicleGuid, uint8_t seat) {
    const uint64_t mover = activeMover();
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || mover == 0) return;
    stopMoverForRequest();
    owner_.getSocket()->send(
        VehiclePackets::changeSeatsOnControlledVehicle(mover, moverWireInfo(), vehicleGuid, seat));
}

void MovementHandler::sendDismissControlledVehicle() {
    const uint64_t mover = activeMover();
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket() || mover == 0) return;
    stopMoverForRequest();
    owner_.getSocket()->send(VehiclePackets::dismissControlledVehicle(mover, moverWireInfo()));
}

} // namespace game
} // namespace wowee
