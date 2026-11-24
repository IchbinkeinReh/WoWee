// src/game/spline_packet.cpp
// Consolidated spline packet parsing - replaces 7 duplicated parsing locations.
// Ported from: world_packets.cpp, world_packets_entity.cpp, packet_parsers_classic.cpp,
//              packet_parsers_tbc.cpp, movement_handler.cpp.
#include "game/spline_packet.hpp"
#include "core/logger.hpp"
#include <cmath>

namespace wowee::game {

// ── Packed-delta decoding ───────────────────────────────────────

glm::vec3 decodePackedDelta(uint32_t packed, const glm::vec3& midpoint) {
    // 11-bit signed X, 11-bit signed Y, 10-bit signed Z
    // Scaled by 0.25, subtracted from midpoint
    int32_t sx = static_cast<int32_t>(packed & 0x7FF);
    if (sx & 0x400) sx |= static_cast<int32_t>(0xFFFFF800);   // sign-extend 11-bit

    int32_t sy = static_cast<int32_t>((packed >> 11) & 0x7FF);
    if (sy & 0x400) sy |= static_cast<int32_t>(0xFFFFF800);   // sign-extend 11-bit

    int32_t sz = static_cast<int32_t>((packed >> 22) & 0x3FF);
    if (sz & 0x200) sz |= static_cast<int32_t>(0xFFFFFC00);   // sign-extend 10-bit

    return glm::vec3(
        midpoint.x - static_cast<float>(sx) * 0.25f,
        midpoint.y - static_cast<float>(sy) * 0.25f,
        midpoint.z - static_cast<float>(sz) * 0.25f
    );
}

// ── MonsterMove spline body (post-splineFlags) ─────────────────

namespace {

/// The head every MonsterMove spline body starts with, whatever expansion
/// wrote it: an optional animation block, the duration, an optional parabolic
/// block, then the point count.
///
/// Written out twice, once in each of the two parsers below, and the only
/// difference between the copies was which bit means animation. That bit moved
/// in WotLK, from 0x00400000 to 0x00200000, so the caller passes the one its
/// generation uses; honouring the other reads five bytes that are not there
/// and shifts every field after it.
///
/// This is the order AzerothCore's WriteCommonMonsterMovePart writes.
///
/// Answers false only on a truncated packet. A point count of zero is a
/// complete body, not a failure: the server sends one for a spline that just
/// turns a creature on the spot.
bool readSplineBodyHead(network::Packet& packet, SplineBlockData& out,
                        uint32_t splineFlags, uint32_t animationBit,
                        uint32_t& outPointCount) {
    out.splineFlags = splineFlags;
    outPointCount = 0;

    // Animation: uint8 animType + uint32 animStartTime
    if (splineFlags & animationBit) {
        if (!packet.hasRemaining(5)) return false;
        out.hasAnimation = true;
        out.animationType = packet.readUInt8();
        out.animationStartTime = packet.readUInt32();
    }

    if (!packet.hasRemaining(4)) return false;
    out.duration = packet.readUInt32();

    // Parabolic: float vertAccel + uint32 startTime
    if (splineFlags & SplineFlag::PARABOLIC_MM) {
        if (!packet.hasRemaining(8)) return false;
        out.hasParabolic = true;
        out.verticalAcceleration = packet.readFloat();
        out.parabolicStartTime = packet.readUInt32();
    }

    if (!packet.hasRemaining(4)) return false;
    const uint32_t pointCount = packet.readUInt32();
    // The client has no limit of its own (0x0073c8e0 reads what the count
    // says): the bytes there are the limit. Four a point at the least, packed;
    // a count read out of the wrong bytes is usually enormous.
    if (pointCount > packet.getRemainingSize() / 4) return false;
    outPointCount = pointCount;
    return true;
}

}  // namespace

bool parseMonsterMoveSplineBody(
    network::Packet& packet,
    SplineBlockData& out,
    uint32_t splineFlags,
    const glm::vec3& startPos,
    bool useTbcUncompressedMask,
    SplineFlagSet flagSet)
{
    const bool wotlk = (flagSet == SplineFlagSet::Wotlk);
    const uint32_t animationBit = wotlk ? SplineFlagWotlk::ANIMATION
                                        : SplineFlag::ANIMATION;

    uint32_t pointCount = 0;
    if (!readSplineBodyHead(packet, out, splineFlags, animationBit, pointCount)) return false;
    if (pointCount == 0) return true;

    // Determine compressed vs uncompressed
    uint32_t uncompMask = useTbcUncompressedMask
        ? SplineFlag::UNCOMPRESSED_MASK_TBC
        : (wotlk ? SplineFlagWotlk::UNCOMPRESSED_MASK
                 : SplineFlag::UNCOMPRESSED_MASK);
    bool uncompressed = (splineFlags & uncompMask) != 0;

    if (uncompressed) {
        // All waypoints as absolute float3, last one is destination
        for (uint32_t i = 0; i + 1 < pointCount; ++i) {
            if (!packet.hasRemaining(12)) return true; // Partial parse OK
            float wx = packet.readFloat();
            float wy = packet.readFloat();
            float wz = packet.readFloat();
            out.waypoints.emplace_back(wx, wy, wz);
        }
        if (!packet.hasRemaining(12)) return true;
        out.destination.x = packet.readFloat();
        out.destination.y = packet.readFloat();
        out.destination.z = packet.readFloat();
        out.hasDest = true;
    } else {
        // Compressed: first float3 is destination, rest are packed deltas from midpoint
        if (!packet.hasRemaining(12)) return true;
        out.destination.x = packet.readFloat();
        out.destination.y = packet.readFloat();
        out.destination.z = packet.readFloat();
        out.hasDest = true;

        if (pointCount > 1) {
            glm::vec3 mid = (startPos + out.destination) * 0.5f;
            for (uint32_t i = 0; i + 1 < pointCount; ++i) {
                if (!packet.hasRemaining(4)) break;
                uint32_t packed = packet.readUInt32();
                out.waypoints.push_back(decodePackedDelta(packed, mid));
            }
        }
    }

    return true;
}

// ── Vanilla MonsterMove spline body (always compressed) ─────────

bool parseMonsterMoveFacing(network::Packet& packet, MonsterMoveData& data, bool& stopped) {
    stopped = false;
    if (!packet.hasRemaining(1)) return false;
    data.moveType = packet.readUInt8();

    // Stop: the packet ends here, and where it stopped is where it already is.
    if (data.moveType == 1) {
        data.destX = data.x;
        data.destY = data.y;
        data.destZ = data.z;
        data.hasDest = false;
        stopped = true;
        return true;
    }

    if (data.moveType == 2) {
        // FacingSpot - the point to face on arriving.
        if (!packet.hasRemaining(12)) return false;
        data.facingSpotX = packet.readFloat();
        data.facingSpotY = packet.readFloat();
        packet.readFloat();
    } else if (data.moveType == 3) {
        // FacingTarget
        if (!packet.hasRemaining(8)) return false;
        data.facingTarget = packet.readUInt64();
    } else if (data.moveType == 4) {
        // FacingAngle
        if (!packet.hasRemaining(4)) return false;
        data.facingAngle = packet.readFloat();
    }
    return true;
}

bool parseMonsterMoveSplineBodyVanilla(
    network::Packet& packet,
    SplineBlockData& out,
    uint32_t splineFlags,
    const glm::vec3& startPos)
{
    uint32_t pointCount = 0;
    if (!readSplineBodyHead(packet, out, splineFlags, SplineFlag::ANIMATION, pointCount)) {
        return false;
    }
    if (pointCount == 0) return true;

    // Always compressed in Vanilla: dest (12 bytes) + packed deltas (4 bytes each)
    if (!packet.hasRemaining(12) || pointCount - 1 > (packet.getRemainingSize() - 12) / 4) return false;

    out.destination.x = packet.readFloat();
    out.destination.y = packet.readFloat();
    out.destination.z = packet.readFloat();
    out.hasDest = true;

    if (pointCount > 1) {
        glm::vec3 mid = (startPos + out.destination) * 0.5f;
        for (uint32_t i = 0; i + 1 < pointCount; ++i) {
            uint32_t packed = packet.readUInt32();
            out.waypoints.push_back(decodePackedDelta(packed, mid));
        }
    }

    return true;
}

// ── Classic/Turtle movement update spline block ─────────────────

bool parseClassicMoveUpdateSpline(
    network::Packet& packet,
    SplineBlockData& out)
{
    // splineFlags
    if (!packet.hasRemaining(4)) return false;
    out.splineFlags = packet.readUInt32();
    LOG_DEBUG("  [Classic] Spline: flags=0x", std::hex, out.splineFlags, std::dec);

    // FINAL_POINT / FINAL_TARGET / FINAL_ANGLE
    if (out.splineFlags & SplineFlag::FINAL_POINT) {
        if (!packet.hasRemaining(12)) return false;
        out.hasFinalPoint = true;
        out.finalPoint.x = packet.readFloat();
        out.finalPoint.y = packet.readFloat();
        out.finalPoint.z = packet.readFloat();
    } else if (out.splineFlags & SplineFlag::FINAL_TARGET) {
        if (!packet.hasRemaining(8)) return false;
        out.hasFinalTarget = true;
        out.finalTarget = packet.readUInt64();
    } else if (out.splineFlags & SplineFlag::FINAL_ANGLE) {
        if (!packet.hasRemaining(4)) return false;
        out.hasFinalAngle = true;
        out.finalAngle = packet.readFloat();
    }

    // timePassed + duration + splineId + pointCount = 16 bytes
    if (!packet.hasRemaining(16)) return false;
    out.timePassed = packet.readUInt32();
    out.duration = packet.readUInt32();
    out.splineId = packet.readUInt32();

    uint32_t pointCount = packet.readUInt32();

    // All points uncompressed (12 bytes each) + endPoint (12 bytes)
    // Classic: NO splineMode byte
    if (!packet.hasRemaining(12) || pointCount > (packet.getRemainingSize() - 12) / 12) return false;
    for (uint32_t i = 0; i < pointCount; ++i) {
        float px = packet.readFloat();
        float py = packet.readFloat();
        float pz = packet.readFloat();
        out.waypoints.emplace_back(px, py, pz);
    }

    out.endPoint.x = packet.readFloat();
    out.endPoint.y = packet.readFloat();
    out.endPoint.z = packet.readFloat();
    out.hasEndPoint = true;

    return true;
}

// ── WotLK movement update spline block ──────────────────────────

bool parseWotlkMoveUpdateSpline(
    network::Packet& packet,
    SplineBlockData& out)
{
    // 0x004f4a50 steps over this block and 0x004f4b50 reads it, and neither
    // has another layout to try nor a limit of its own: the flags, the end's
    // facing, 0x1c bytes, the node count, twelve bytes a node, the mode byte
    // and the end point. There were five more layouts here, for when the
    // right one was refused, and it was refused at 256 nodes, a mode over 3 or
    // an end point 20000 yards off: one of the others then took whatever lay
    // where it looked for a count, a nought mostly, and the rest of the packet
    // was read shifted - up to 400 creatures and objects of a city lost to one
    // that walks round it.
    const auto refuse = [&](const char* why) {
        LOG_WARNING("WotLK spline parse failed: ", why, " splineFlags=0x", std::hex, out.splineFlags, std::dec,
                    " remaining=", packet.getRemainingSize());
        return false;
    };
    if (!packet.hasRemaining(4)) return refuse("no flags");
    out.splineFlags = packet.readUInt32();

    // In the client's order: an angle, else a target, else a point.
    if (out.splineFlags & SplineFlagWotlk::FINAL_ANGLE) {
        if (!packet.hasRemaining(4)) return refuse("no facing angle");
        out.hasFinalAngle = true;
        out.finalAngle = packet.readFloat();
    } else if (out.splineFlags & SplineFlagWotlk::FINAL_TARGET) {
        if (!packet.hasRemaining(8)) return refuse("no facing target");
        out.hasFinalTarget = true;
        out.finalTarget = packet.readUInt64();
    } else if (out.splineFlags & SplineFlagWotlk::FINAL_POINT) {
        if (!packet.hasRemaining(12)) return refuse("no facing point");
        out.hasFinalPoint = true;
        out.finalPoint.x = packet.readFloat();
        out.finalPoint.y = packet.readFloat();
        out.finalPoint.z = packet.readFloat();
    }

    // 0x1c: the time passed, the duration, the id, the two duration factors,
    // the vertical acceleration and its start, whatever the flags. Nothing for
    // ANIMATION (0x00200000): an animation's type and start time are
    // SMSG_MONSTER_MOVE's, not this block's.
    if (!packet.hasRemaining(0x1c + 4)) return refuse("no header");
    out.timePassed = packet.readUInt32();
    out.duration = packet.readUInt32();
    out.splineId = packet.readUInt32();
    out.durationFactor = packet.readFloat();
    out.nextDurationFactor = packet.readFloat();
    out.verticalAcceleration = packet.readFloat();
    out.parabolicStartTime = packet.readUInt32();
    out.hasParabolic = (out.splineFlags & SplineFlagWotlk::PARABOLIC) != 0;

    const uint32_t pointCount = packet.readUInt32();
    constexpr size_t kAfterPoints = 1 + 12;  // the mode and the end point
    if (!packet.hasRemaining(kAfterPoints) ||
        pointCount > (packet.getRemainingSize() - kAfterPoints) / 12) {
        LOG_WARNING("WotLK spline: ", pointCount, " nodes in ", packet.getRemainingSize(), " bytes");
        return refuse("more nodes than there are bytes for");
    }
    out.waypoints.reserve(pointCount);
    for (uint32_t i = 0; i < pointCount; ++i) {
        const float px = packet.readFloat();
        const float py = packet.readFloat();
        const float pz = packet.readFloat();
        out.waypoints.emplace_back(px, py, pz);
    }
    out.splineMode = packet.readUInt8();
    out.endPoint.x = packet.readFloat();
    out.endPoint.y = packet.readFloat();
    out.endPoint.z = packet.readFloat();
    out.hasEndPoint = true;
    return true;
}

} // namespace wowee::game
