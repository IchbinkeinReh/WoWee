#include "game/transport_manager.hpp"
#include "game/game_object_rotation.hpp"
#include "game/transport_local.hpp"
#include "game/transport_boarding.hpp"

#include <set>
#include "game/transport_clock_sync.hpp"
#include "game/transport_animator.hpp"
#include "game/game_utils.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/movement_limits.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>
#include <chrono>
#include <cmath>
#include <limits>

namespace wowee::game {

namespace {

bool seedDeeprunTramStationPhase(ActiveTransport& transport,
                                 const PathEntry& pathEntry,
                                 const glm::vec3& serverPosition) {
    const auto& spline = pathEntry.spline;
    const auto& keys = spline.keys();
    if (!pathEntry.fromDBC || spline.durationMs() == 0 || keys.empty()) {
        return false;
    }

    size_t maxOffsetIdx = 0;
    float maxAbsX = 0.0f;
    for (size_t i = 0; i < keys.size(); ++i) {
        const float absX = std::abs(keys[i].position.x);
        if (absX > maxAbsX) {
            maxAbsX = absX;
            maxOffsetIdx = i;
        }
    }

    if (maxAbsX < 100.0f) {
        return false;
    }

    const bool pathMaxIsPositive = keys[maxOffsetIdx].position.x > 0.0f;
    const bool serverAtPositiveEnd = serverPosition.x > 1000.0f;
    const bool useMaxOffsetStation = (pathMaxIsPositive == serverAtPositiveEnd);

    size_t stationIdx = maxOffsetIdx;
    if (!useMaxOffsetStation) {
        stationIdx = 0;
        for (size_t step = 1; step < keys.size(); ++step) {
            const size_t idx = (maxOffsetIdx + step) % keys.size();
            if (std::abs(keys[idx].position.x) < 1.0f) {
                stationIdx = idx;
                break;
            }
        }
    }

    // CMaNGOS's "presence echo" for these objects is always the same static DB spawn
    // row, not a live position - it tells us this spawn coordinate corresponds to
    // local path point stationIdx (a geometric fact about how this entry's path is
    // anchored in world space), not that the tram is *currently* there. Correct
    // basePosition so the path curve sits in the right place; leave localClockMs
    // alone so it keeps reflecting the absolute-time-derived phase set at
    // registration (see nowEpochMs comment above) instead of snapping back to
    // "docked at this waypoint" every time an echo arrives.
    const glm::vec3 stationOffset = keys[stationIdx].position;
    const glm::vec3 candidateBasePosition = serverPosition - stationOffset;

    // The useMaxOffsetStation==true branch assumes the server's static echo represents
    // the tram docked at the path's far/max-offset end - true for most of these six
    // paths, but at least one (176085) has local key data where that assumption is
    // simply wrong: the echo position is really the near-origin "home" point, and
    // trusting the heuristic silently shifted basePosition by ~2481 units toward the
    // opposite station. That desyncs this car's *logical* position (used for boarding
    // distance and path evaluation) from where it's actually spawned/rendered, while
    // leaving it looking fine on screen - "climbed onto the middle car, it drove away
    // without me, hung in midair" reported live, and very likely also this entry's
    // contribution to "solo cars" (its real position no longer lines up with its train).
    // registerTransport() already computed a sane basePosition from the spawn position
    // directly; a legitimate correction should only be a modest nudge from that, not a
    // jump on the order of the whole route length. Reject implausible corrections
    // rather than trust the heuristic blindly.
    // Note: returning true here (not false) is deliberate even though no correction is
    // applied. The caller's false-branch fallback resets localClockMs to 0 and slams
    // basePosition to the raw server position - correct for the genuinely-no-usable-path
    // cases above, but here registerTransport() already left the transport in a good
    // state (sane basePosition, correctly-seeded localClockMs); rejecting a bad
    // correction should mean "leave that alone", not "wipe it and start over".
    constexpr float kMaxPlausibleCorrectionDist = 300.0f;
    if (glm::distance(candidateBasePosition, transport.basePosition) > kMaxPlausibleCorrectionDist) {
        // Entry 176085 rejects this every re-sync cycle by design (its real spawn is
        // legitimately far from the heuristic's candidate) - routine, not worth WARN.
        LOG_DEBUG("Deeprun tram station anchor correction rejected as implausible: guid=0x",
                    std::hex, transport.guid, std::dec,
                    " entry=", transport.entry, " pathId=", transport.pathId,
                    " candidateBase=(", candidateBasePosition.x, ",", candidateBasePosition.y, ",", candidateBasePosition.z, ")",
                    " existingBase=(", transport.basePosition.x, ",", transport.basePosition.y, ",", transport.basePosition.z, ")");
        return true;
    }

    transport.basePosition = candidateBasePosition;
    transport.position = transport.basePosition + spline.evaluatePosition(transport.localClockMs % spline.durationMs());
    transport.hasServerYaw = false;

    LOG_DEBUG("Deeprun tram station anchor corrected: guid=0x", std::hex, transport.guid, std::dec,
                " entry=", transport.entry,
                " pathId=", transport.pathId,
                " localClockMs=", transport.localClockMs,
                " stationOffset=(", stationOffset.x, ",", stationOffset.y, ",", stationOffset.z, ")",
                " base=(", transport.basePosition.x, ",", transport.basePosition.y, ",", transport.basePosition.z, ")",
                " serverPos=(", serverPosition.x, ",", serverPosition.y, ",", serverPosition.z, ")");
    return true;
}

} // namespace

TransportManager::TransportManager() = default;
TransportManager::~TransportManager() = default;

void TransportManager::update(float deltaTime) {
    elapsedTime_ += deltaTime;

    for (auto& [guid, transport] : transports_) {
        // Once we have server clock offset, we can predict server time indefinitely
        // No need for watchdog - keep using the offset even if server updates stop
        updateTransportMovement(transport, deltaTime);
    }
}

void TransportManager::registerTransport(uint64_t guid,
                                         uint32_t wmoInstanceId,
                                         uint32_t pathId,
                                         const glm::vec3& spawnWorldPos,
                                         uint32_t entry,
                                         uint32_t displayId,
                                         bool isM2,
                                         float spawnOrientation) {
    auto* pathEntry = pathRepo_.findPath(pathId);
    if (!pathEntry) {
        LOG_ERROR("TransportManager: Path ", pathId, " not found for transport ", guid);
        return;
    }

    const auto& spline = pathEntry->spline;
    if (spline.keyCount() == 0) {
        LOG_ERROR("TransportManager: Path ", pathId, " has no waypoints");
        return;
    }

    ActiveTransport transport;
    transport.guid = guid;
    transport.wmoInstanceId = wmoInstanceId;
    transport.pathId = pathId;
    transport.entry = entry;
    transport.displayId = displayId;
    transport.isM2 = isM2;
    transport.worldCoords = pathEntry->worldCoords;
    transport.allowBootstrapVelocity = false;

    // A moving path registered under someone else's entry is a borrowed route.
    //
    // Marked here rather than where the borrowing is decided, because it is
    // decided in more than one place: the Tirisfal zeppelin was flagged on its
    // first spawn and then respawned unflagged on the far side of a world
    // transfer, so the arrival-side instance went back to flying the wrong
    // route with a passenger aboard. Every registration comes through here.
    if (!isM2 && !pathEntry->worldCoords && spline.durationMs() > 0 &&
        spline.keyCount() > 1 && pathId != entry) {
        transport.borrowedPath = true;
        static std::set<uint32_t> saidBorrow;
        if (saidBorrow.insert(entry).second) {
            LOG_WARNING("Transport entry=", entry, " is flying path ", pathId,
                        ", which is not its own - following server position "
                        "updates instead of animating it");
        }
    }

    // CRITICAL: Set basePosition from spawn position and t=0 offset
    // For stationary paths (1 waypoint), just use spawn position directly
    if (spline.durationMs() == 0 || spline.keyCount() <= 1 || transport.borrowedPath) {
        // Stationary transport, or one on a borrowed route it will not fly -
        // either way the spawn position is where it is, not an offset into a
        // path. Inferring a base from the borrowed spline's first waypoint put
        // the hull a whole route away from the dock the server spawned it at.
        transport.basePosition = spawnWorldPos;
        transport.position = spawnWorldPos;
    } else if (pathEntry->worldCoords) {
        // World-coordinate path (TaxiPathNode) - points are absolute world positions
        transport.basePosition = glm::vec3(0.0f);
        transport.position = spline.evaluatePosition(0);
    } else {
        // Moving transport - infer base from first path offset
        glm::vec3 offset0 = spline.evaluatePosition(0);
        transport.basePosition = spawnWorldPos - offset0;  // Infer base from spawn
        transport.position = spawnWorldPos;  // Start at spawn position (base + offset0)

        // TransportAnimation paths are local offsets; first waypoint is expected near origin.
        // Warn only if the local path itself looks suspicious.
        glm::vec3 firstWaypoint = spline.keys()[0].position;
        if (glm::dot(firstWaypoint, firstWaypoint) > 100.0f) {
            LOG_WARNING("Transport 0x", std::hex, guid, std::dec, " path ", pathId,
                        ": first local waypoint far from origin: (",
                        firstWaypoint.x, ",", firstWaypoint.y, ",", firstWaypoint.z, ")");
        }
    }

    // The authored yaw as the hull is drawn: a turn to the server's facing,
    // as the spawner draws any game object (0x00712ee0) - a model's default
    // faces +renderX, M2 or WMO. A WMO hull was placed at the canonical yaw,
    // a quarter turn behind its passengers' frame. The transform below is
    // rebuilt from scratch every frame, so a rotation that skips this puts the
    // model at right angles to where it was placed. Only route-less
    // transports read this - a tangent-derived heading carries its own
    // convention and is left alone.
    transport.spawnYaw = core::coords::canonicalToServerYaw(spawnOrientation);
    // Identity discarded the authored placement, so every transport started
    // life facing the same way whatever the server said.
    transport.rotation = glm::angleAxis(transport.spawnYaw, glm::vec3(0.0f, 0.0f, 1.0f));
    // Where the server placed it, whichever kind it is drawn as.
    transport.facing = core::coords::canonicalToServerYaw(spawnOrientation);
    transport.playerOnBoard = false;
    transport.playerLocalOffset = glm::vec3(0.0f);
    transport.hasDeckBounds = false;
    transport.localClockMs = 0;
    transport.hasServerClock = false;
    transport.serverClockOffsetMs = 0;
    // Start with client-side animation for all DBC paths with real movement.
    // If the server sends actual position updates, updateServerTransport() will switch
    // to server-driven mode. This ensures transports like trams (which the server doesn't
    // stream updates for) still animate, while ships/zeppelins switch to server authority.
    // The borrowed-path test above turned this off and this line turned it back
    // on, sixty lines later and out of sight: the zeppelin flew the borrowed
    // route under client animation while the server drove it somewhere else, so
    // it played both journeys at once and read as two hulls.
    transport.useClientAnimation =
        (pathEntry->fromDBC && spline.durationMs() > 0 && !transport.borrowedPath);
    transport.clientAnimationReverse = false;
    transport.serverYaw = 0.0f;
    transport.hasServerYaw = false;
    transport.dockYaw = 0.0f;
    transport.hasDockYaw = false;
    transport.lastServerUpdate = 0.0f;
    transport.serverUpdateCount = 0;
    transport.serverLinearVelocity = glm::vec3(0.0f);
    transport.serverAngularVelocity = 0.0f;
    transport.hasServerVelocity = false;

    if (transport.useClientAnimation && spline.durationMs() > 0) {
        // Seed to a stable phase derived from absolute time (see nowEpochMs comment)
        // so elevators don't all start at t=0, and so paired/opposing transports like
        // the Deeprun Tram's two cars land at the same relative phase for every client.
        // Deeprun tram cars use a rounded duration for the modulo (see
        // deeprunTramSeedDurationMs comment) since sibling cars' own path durations
        // don't quite match and that mismatch was decorrelating their seeds.
        const uint32_t seedDurationMs = isDeeprunTramTransport(transport)
            ? deeprunTramSeedDurationMs(spline.durationMs())
            : spline.durationMs();
        transport.localClockMs = static_cast<uint32_t>(nowEpochMs() % seedDurationMs);
        seedFromServerPathTime(transport);
        LOG_INFO("TransportManager: Enabled client animation for transport 0x",
                 std::hex, guid, std::dec, " path=", pathId,
                 " durationMs=", spline.durationMs(), " seedMs=", transport.localClockMs,
                 (pathEntry->worldCoords ? " [worldCoords]" : (pathEntry->zOnly ? " [z-only]" : "")));
    }

    updateTransformMatrices(transport);

    // CRITICAL: Update WMO renderer with initial transform
    pushTransform(transport);

    if (const auto parent = parentRotations_.find(guid); parent != parentRotations_.end())
        transport.parentRotation = parent->second;

    transports_[guid] = transport;

    // A route phase the server published before this was registered.
    if (auto pendingClock = pendingRouteClocks_.find(guid);
        pendingClock != pendingRouteClocks_.end()) {
        const auto [phase, periodMs] = pendingClock->second;
        pendingRouteClocks_.erase(pendingClock);
        applyServerRouteClock(guid, phase, periodMs);
    }

    glm::vec3 renderPos = core::coords::canonicalToRender(transport.position);
    LOG_INFO("TransportManager: Registered transport 0x", std::hex, guid, std::dec,
             " at path ", pathId, " with ", (pathEntry ? pathEntry->spline.keyCount() : 0u), " waypoints",
             " wmoInstanceId=", wmoInstanceId,
             " entry=", entry,
             " displayId=", displayId,
             " isM2=", isM2,
             " spawnPos=(", spawnWorldPos.x, ", ", spawnWorldPos.y, ", ", spawnWorldPos.z, ")",
             " basePos=(", transport.basePosition.x, ", ", transport.basePosition.y, ", ", transport.basePosition.z, ")",
             " initialRenderPos=(", renderPos.x, ", ", renderPos.y, ", ", renderPos.z, ")");

    if (isDeeprunTramTransport(transport)) {
        LOG_DEBUG("Deeprun tram registered: guid=0x", std::hex, guid, std::dec,
                    " entry=", entry,
                    " displayId=", displayId,
                    " pathId=", pathId,
                    " instanceId=", wmoInstanceId,
                    " isM2=", isM2,
                    " mode=", (transport.useClientAnimation ? "client" : "server"),
                    " spawn=(", spawnWorldPos.x, ",", spawnWorldPos.y, ",", spawnWorldPos.z, ")",
                    " base=(", transport.basePosition.x, ",", transport.basePosition.y, ",", transport.basePosition.z, ")");
    }
}
void TransportManager::clearTransports() {
    const size_t count = transports_.size();
    transports_.clear();
    // Held phases belong to the map that published them; a reused GUID on the
    // next map must not inherit one.
    pendingRouteClocks_.clear();
    parentRotations_.clear();
    if (count != 0) {
        LOG_INFO("TransportManager: Cleared ", count, " transports for map transition");
    }
}

void TransportManager::resolveAndRegisterSpawn(uint64_t guid,
                                               uint32_t entry,
                                               uint32_t displayId,
                                               const glm::vec3& canonicalSpawnPos,
                                               uint32_t wmoInstanceId,
                                               bool isM2,
                                               bool preferServerData,
                                               float spawnOrientation) {
    // TransportAnimation.dbc is indexed by GameObject entry.
    uint32_t pathId = entry;

    // Check if we have a real usable path, otherwise remap/infer/fall back to stationary.
    // Elevators used to be in this list - 807, 808, 2454 and 1587 are lifts,
    // not airships - and the stricter "must travel 25 units" check below then
    // rejected their short vertical path, dropping them into the inference
    // that borrows a nearby route.
    const bool shipOrZeppelinDisplay = isVehicleTransportDisplay(displayId);
    bool hasUsablePath = hasPathForEntry(entry);
    if (shipOrZeppelinDisplay) {
        hasUsablePath = hasUsableMovingPathForEntry(entry, 25.0f);
    }
    if (preferServerData) {
        // Strict server-authoritative mode: no inferred/remapped fallback routes.
        if (!hasUsablePath) {
            std::vector<glm::vec3> path = { canonicalSpawnPos };
            loadPathFromNodes(pathId, path, false, 0.0f);
            LOG_INFO("Auto-spawned transport in strict server-first mode (stationary fallback): entry=", entry,
                     " displayId=", displayId, " wmoInstance=", wmoInstanceId);
        } else {
            LOG_INFO("Auto-spawned transport in server-first mode with entry DBC path: entry=", entry,
                     " displayId=", displayId, " wmoInstance=", wmoInstanceId);
        }
    } else if (!hasUsablePath) {
        bool allowZOnly = (displayId == 455 || displayId == 462);
        // Continent-crossing ships are server-driven MO_TRANSPORT objects whose
        // route only ever comes from their taxi path (TaxiPathNode.dbc via GO
        // template data[0]), assigned by the GO-query hook. They must NOT infer a
        // nearby TransportAnimation.dbc path at spawn: a ship spawned in a harbour
        // can otherwise borrow an unrelated local animation loop (e.g. an elevator)
        // and circle in place until - or unless - its taxi path arrives. The ship
        // guard in pickFallbackMovingPath already returns 0 for these displays; skip
        // inference too so the same guard actually holds, leaving the ship docked.
        const bool looksLikeShip = isOceanGoingTransportDisplay(displayId);
        uint32_t inferredPath =
            looksLikeShip ? 0u : inferDbcPathForSpawn(canonicalSpawnPos, 1200.0f, allowZOnly);
        if (inferredPath != 0) {
            pathId = inferredPath;
            LOG_INFO("Auto-spawned transport with inferred path: entry=", entry,
                     " inferredPath=", pathId, " displayId=", displayId,
                     " wmoInstance=", wmoInstanceId);
        } else {
            uint32_t remappedPath = pickFallbackMovingPath(entry, displayId);
            if (remappedPath != 0) {
                pathId = remappedPath;
                LOG_INFO("Auto-spawned transport with remapped fallback path: entry=", entry,
                         " remappedPath=", pathId, " displayId=", displayId,
                         " wmoInstance=", wmoInstanceId);
            } else {
                std::vector<glm::vec3> path = { canonicalSpawnPos };
                loadPathFromNodes(pathId, path, false, 0.0f);
                LOG_INFO("Auto-spawned transport with stationary path: entry=", entry,
                         " displayId=", displayId, " wmoInstance=", wmoInstanceId);
            }
        }
    } else {
        LOG_INFO("Auto-spawned transport with real path: entry=", entry,
                 " displayId=", displayId, " wmoInstance=", wmoInstanceId);
    }

    registerTransport(guid, wmoInstanceId, pathId, canonicalSpawnPos, entry, displayId,
                      isM2, spawnOrientation);

    if (displayId == 3831u) {
        if (auto* tr = getTransport(guid)) {
            LOG_DEBUG("Auto-spawned Deeprun tram transport: guid=0x",
                        std::hex, guid, std::dec,
                        " entry=", entry,
                        " pathId=", tr->pathId,
                        " isM2=", tr->isM2,
                        " mode=", (tr->useClientAnimation ? "client" : "server"));
        }
    }
}

ActiveTransport* TransportManager::getTransport(uint64_t guid) {
    auto it = transports_.find(guid);
    if (it != transports_.end()) {
        return &it->second;
    }
    return nullptr;
}

glm::vec3 TransportManager::getPlayerWorldPosition(uint64_t transportGuid, const glm::vec3& localOffset) {
    auto* transport = getTransport(transportGuid);
    if (!transport) {
        LOG_WARNING("getPlayerWorldPosition: transport 0x", std::hex, transportGuid, std::dec,
                    " not found - returning localOffset as-is (callers should guard)");
        return localOffset;
    }
    // Through the transport's matrix as it is now (0x004f4460), an M2 lift's
    // as much as a WMO ship's: one turned by TransportRotation.dbc carries
    // its riders round with it rather than only turning their facings.
    return transport_local::toWorld(!transport->isM2, passengerFrame(*transport), localOffset);
}

std::optional<glm::vec3> TransportManager::worldToTransportLocal(
    uint64_t transportGuid, const glm::vec3& canonicalWorld) const {
    const auto it = transports_.find(transportGuid);
    if (it == transports_.end()) return std::nullopt;
    return transport_local::toLocal(!it->second.isM2, passengerFrame(it->second), canonicalWorld);
}

glm::mat4 TransportManager::passengerFrame(const ActiveTransport& transport) {
    // A WMO hull is drawn by its facing (or its TransportRotation turn), so
    // its render transform is the client's matrix (0x00712ee0). An M2 one's
    // is its place and the rotation the client gives it - its facing, or its
    // TransportRotation turn - not the drawn one, which for the Deeprun tram
    // follows its track instead.
    if (!transport.isM2) return transport.transform;
    return transport_local::frame(
        transport.position,
        transport.pathRotation.value_or(game_object_rotation::fromFacing(transport.facing)));
}

std::optional<glm::mat4> TransportManager::passengerRenderFrame(uint64_t transportGuid) const {
    const auto it = transports_.find(transportGuid);
    if (it == transports_.end()) return std::nullopt;
    return passengerFrame(it->second);
}

glm::vec3 TransportManager::serverToTransportLocal(
    uint64_t transportGuid, const glm::vec3& serverOffset) const {
    const auto it = transports_.find(transportGuid);
    // The wire's offset is in the transport's own frame, which is how it is
    // kept: a WMO transport's as it comes, an M2 one's on canonical axes.
    if (it == transports_.end()) return transport_local::fromWire(false, serverOffset);
    return transport_local::fromWire(!it->second.isM2, serverOffset);
}

glm::vec3 TransportManager::transportLocalToServer(
    uint64_t transportGuid, const glm::vec3& localOffset) const {
    const auto it = transports_.find(transportGuid);
    if (it == transports_.end()) return transport_local::toWire(false, localOffset);
    return transport_local::toWire(!it->second.isM2, localOffset);
}

std::optional<float> TransportManager::transportFacing(uint64_t transportGuid) const {
    const auto it = transports_.find(transportGuid);
    if (it == transports_.end()) return std::nullopt;
    return it->second.facing;
}

bool TransportManager::isPointOnTransportDeck(uint64_t transportGuid,
                                               const glm::vec3& canonicalPosition,
                                               float maxFloorDelta) const {
    const auto floor = getTransportDeckFloorHeight(transportGuid, canonicalPosition);
    if (!floor) return false;

    const float floorDelta = canonicalPosition.z - *floor;
    return floorDelta >= -0.35f && floorDelta <= maxFloorDelta;
}

void TransportManager::noteServerPathTime(uint64_t transportGuid, uint32_t pathTimeMs) {
    serverPathTimes_[transportGuid] = {pathTimeMs, elapsedTime_};
    if (auto* transport = getTransport(transportGuid)) seedFromServerPathTime(*transport);
}

void TransportManager::seedFromServerPathTime(ActiveTransport& transport) {
    // Elevators only - a z-only path played on the client's clock. A ship's or
    // a zeppelin's block carries the server's own ms instead, and its phase
    // comes from the route fields (applyServerRouteClock).
    if (!transport.useClientAnimation || transport.hasServerRouteClock) return;
    auto it = serverPathTimes_.find(transport.guid);
    if (it == serverPathTimes_.end()) return;
    const auto* pathEntry = pathRepo_.findPath(transport.pathId);
    if (!pathEntry || !pathEntry->zOnly) return;
    const uint32_t durationMs = pathEntry->spline.durationMs();
    if (durationMs == 0) return;
    // Seeded from the wall clock, the lift ran at an arbitrary offset from
    // the server's, so the doors - which the server opens on its own
    // timeline - shut just as the car arrived.
    const double sinceMs = std::max(0.0, (elapsedTime_ - it->second.receivedAt) * 1000.0);
    const uint64_t now = static_cast<uint64_t>(it->second.pathTimeMs) + static_cast<uint64_t>(sinceMs);
    transport.localClockMs = static_cast<uint32_t>(now % durationMs);
    LOG_INFO("Transport 0x", std::hex, transport.guid, std::dec, " at the server's path time ",
             transport.localClockMs, "ms of ", durationMs, "ms");
}

void TransportManager::applyServerRouteClock(uint64_t transportGuid, float phase,
                                             uint32_t periodMs) {
    if (periodMs == 0) return;
    if (!(phase >= 0.0f) || phase >= 1.0f) return;   // also rejects NaN

    auto* transport = getTransport(transportGuid);
    if (!transport) {
        // The create block carries this, and a transport is not registered
        // when it arrives - registration waits on the model loading, which
        // takes several frames. The server publishes the phase when the object
        // is created and rarely again, so dropping it here meant a hull that
        // never adopted the server's clock at all and invented one from
        // distance over speed instead. Held until registerTransport asks.
        pendingRouteClocks_[transportGuid] = {phase, periodMs};
        return;
    }

    const bool firstSample = !transport->hasServerRouteClock;
    transport->hasServerRouteClock = true;
    transport->routePhase = phase;
    transport->routePeriodMs = periodMs;
    transport->routePhaseAtTime = elapsedTime_;

    if (firstSample) {
        LOG_WARNING("Transport 0x", std::hex, transportGuid, std::dec,
                 " adopted server route clock: period=", periodMs, "ms phase=", phase,
                 " (client period was ", [&]() -> uint32_t {
                     const auto* p = pathRepo_.findPath(transport->pathId);
                     return p ? p->spline.durationMs() : 0u;
                 }(), "ms)");
    }
}

void TransportManager::applyDoodadMotionState(ActiveTransport& transport, bool moving) {
    if (transport.isM2 || !wmoRenderer_ || transport.wmoInstanceId == 0) return;

    // 163 = ShipMoving, 164 = ShipStop. Stopping is a one-shot that settles into
    // the model's idle pose; running is a loop.
    constexpr uint32_t kShipMoving = 163u;
    constexpr uint32_t kShipStop = 164u;
    const uint32_t want = moving ? kShipMoving : kShipStop;

    const bool sameState = (transport.appliedDoodadAnim == static_cast<int>(want));
    if (sameState && transport.appliedDoodadCount != 0) return;

    const size_t touched =
        wmoRenderer_->setInstanceDoodadAnimation(transport.wmoInstanceId, want, moving);
    transport.appliedDoodadAnim = static_cast<int>(want);
    transport.appliedDoodadCount = touched;
}

bool TransportManager::isTransportCollisionReady(uint64_t transportGuid) const {
    if (!wmoRenderer_) return false;
    const auto it = transports_.find(transportGuid);
    if (it == transports_.end() || it->second.isM2 || it->second.wmoInstanceId == 0) {
        return false;
    }
    return wmoRenderer_->instanceHasCollisionGeometry(it->second.wmoInstanceId);
}

uint64_t TransportManager::groundTransportUnder(const glm::vec3& canonicalPosition) const {
    const glm::vec3 render = core::coords::canonicalToRender(canonicalPosition);
    uint64_t owner = 0;
    float ownerDeck = -std::numeric_limits<float>::infinity();
    for (const auto& [guid, transport] : transports_) {
        // A hull on the other map is held still and hidden there.
        if (!transport.onThisMap || transport.wmoInstanceId == 0) continue;
        std::optional<float> deck;
        if (transport.isM2) {
            if (!m2Renderer_) continue;
            // The car's own floor under the feet, within a step of them -
            // not a box about it, which covers its doorway and the ground
            // outside.
            deck = m2Renderer_->getInstanceFloorHeight(
                transport.wmoInstanceId, render.x, render.y,
                render.z + rendering::movement::kMaxStepUp);
            if (deck) {
                const float delta = render.z - *deck;
                if (delta < -rendering::movement::kMaxStepUp || delta > 2.0f) deck.reset();
            }
        } else {
            deck = getTransportDeckFloorHeight(guid, canonicalPosition);
            if (deck) {
                const float delta = canonicalPosition.z - *deck;
                if (delta < -0.35f || delta > 1.25f) deck.reset();
            }
        }
        if (deck && *deck > ownerDeck) {
            ownerDeck = *deck;
            owner = guid;
        }
    }
    return owner;
}

bool TransportManager::stillHoldsPassenger(uint64_t transportGuid,
                                           const glm::vec3& canonicalPosition) const {
    const auto it = transports_.find(transportGuid);
    if (it == transports_.end()) return false;
    const ActiveTransport& transport = it->second;
    if (!transport.isM2) return false;
    // No model to ask: 0x0070b360 keeps the passenger.
    if (transport.wmoInstanceId == 0 || !m2Renderer_) return true;
    glm::vec3 boxMin, boxMax;
    // A model not loaded yet holds nobody (0x0077ffb0).
    if (!m2Renderer_->getInstanceCollisionBox(transport.wmoInstanceId, boxMin, boxMax)) return false;
    // The mover's place on the transport, in its frame (the movement's +0x10).
    const glm::vec3 local = transport_local::toWire(
        false, transport_local::toLocal(false, passengerFrame(transport), canonicalPosition));
    return transport_boarding::insideCollisionBox(local, boxMin, boxMax);
}

std::optional<float> TransportManager::getTransportDeckFloorHeight(
    uint64_t transportGuid,
    const glm::vec3& canonicalPosition) const {
    if (!wmoRenderer_) return std::nullopt;
    const auto it = transports_.find(transportGuid);
    if (it == transports_.end() || it->second.isM2 || it->second.wmoInstanceId == 0) {
        return std::nullopt;
    }

    const glm::vec3 renderPosition = core::coords::canonicalToRender(canonicalPosition);
    float normalZ = 0.0f;
    const auto floor = wmoRenderer_->getInstanceFloorHeight(
        it->second.wmoInstanceId,
        renderPosition.x, renderPosition.y, renderPosition.z + 0.35f,
        &normalZ);
    if (!floor || normalZ < 0.55f) return std::nullopt;
    return floor;
}
void TransportManager::loadPathFromNodes(uint32_t pathId, const std::vector<glm::vec3>& waypoints, bool looping, float speed) {
    pathRepo_.loadPathFromNodes(pathId, waypoints, looping, speed);
}

void TransportManager::setDeckBounds(uint64_t guid, const glm::vec3& min, const glm::vec3& max) {
    auto* transport = getTransport(guid);
    if (!transport) {
        LOG_ERROR("TransportManager: Cannot set deck bounds for unknown transport ", guid);
        return;
    }

    transport->deckMin = min;
    transport->deckMax = max;
    transport->hasDeckBounds = true;
}

void TransportManager::updateTransportMovement(ActiveTransport& transport, float deltaTime) {
    auto* pathEntry = pathRepo_.findPath(transport.pathId);
    if (!pathEntry) {
        return;
    }

    // A borrowed route belongs to another transport. Its position comes from
    // updateServerTransport and nowhere else; evaluating the spline here is
    // what carried a rider along a journey the hull was not making.
    if (transport.borrowedPath) {
        updateTransformMatrices(transport);
        pushTransform(transport);
        applyDoodadMotionState(transport, /*moving=*/transport.hasServerVelocity);
        return;
    }

    const auto& spline = pathEntry->spline;
    if (spline.keyCount() == 0) {
        return;
    }

    // Stationary transport (durationMs = 0)
    if (spline.durationMs() == 0) {
        // Just update transform (position already set)
        updateTransformMatrices(transport);
        pushTransform(transport);
        applyDoodadMotionState(transport, /*moving=*/false);
        return;
    }

    // Compute path time via ClockSync
    uint32_t pathTimeMs = 0;
    if (!clockSync_.computePathTime(transport, spline, elapsedTime_, deltaTime, pathTimeMs)) {
        // Strict server-authoritative mode: do not guess movement between server snapshots.
        updateTransformMatrices(transport);
        pushTransform(transport);
        return;
    }

    // A cross-continent route spends part of its cycle on the other map. The
    // slice holds the hull still there rather than sweeping it across, and
    // there is nothing here to draw, stand on, or board until it returns.
    //
    // Not while someone is standing on it. Hiding the hull and skipping the
    // pose below takes the deck out from under a rider and freezes them at a
    // position the world has already streamed away from - the client has no
    // authority to carry them across a map boundary, only the server does, and
    // until it says so the honest thing is to keep the deck where it is. The
    // slice holds the hull at its last dock through the off-map stretch, so
    // this parks the rider at the pier rather than dropping them mid-ocean.
    const bool carryingRider = riderTransportGuid_ != 0 && transport.guid == riderTransportGuid_;
    const bool presentNow = pathEntry->presentAt(pathTimeMs) || carryingRider;
    if (presentNow != transport.onThisMap) {
        transport.onThisMap = presentNow;
        LOG_INFO("Transport 0x", std::hex, transport.guid, std::dec,
                 " entry=", transport.entry,
                 (presentNow ? " arrived on this map" : " left for the other map"),
                 " at route time ", pathTimeMs, "ms");
    }
    if (!presentNow) {
        setInstanceHidden(transport, true);
        applyDoodadMotionState(transport, /*moving=*/false);
        return;
    }

    // Evaluate position + rotation via Animator
    animator_.evaluateAndApply(transport, *pathEntry, pathTimeMs);
    // An elevator or a tram (type 11) with TransportRotation rows turns along
    // its TransportAnimation path (0x007139e0 -> 0x007106d0 -> 0x0070dc10).
    // A ship on its TaxiPath route is not one.
    if (pathEntry->fromDBC && !pathEntry->worldCoords) applyPathRotation(transport, pathTimeMs);

    // Update transform matrices
    updateTransformMatrices(transport);
    pushTransform(transport);
    applyDoodadMotionState(transport, /*moving=*/!transport.atDockDwell);

    // Debug logging every 600 frames (~10 seconds at 60fps)
    static int debugFrameCount = 0;
    if (debugFrameCount++ % 600 == 0) {
        LOG_DEBUG("Transport 0x", std::hex, transport.guid, std::dec,
                 " pathTime=", pathTimeMs, "ms / ", spline.durationMs(), "ms",
                 " pos=(", transport.position.x, ", ", transport.position.y, ", ", transport.position.z, ")",
                 " mode=", (transport.useClientAnimation ? "client" : "server"),
                 " isM2=", transport.isM2);
    }
}

// Push transform to the appropriate renderer (WMO or M2).
void TransportManager::setInstanceHidden(const ActiveTransport& transport, bool hidden) {
    // Only WMO hulls. An M2 transport is a tram car or a lift, which never
    // leaves its map, so onThisMap is always true for one and this is never
    // asked to hide it.
    if (!transport.isM2 && wmoRenderer_) {
        wmoRenderer_->setInstanceHidden(transport.wmoInstanceId, hidden);
    }
}

void TransportManager::pushTransform(ActiveTransport& transport) {
    setInstanceHidden(transport, !transport.onThisMap);
    if (transport.isM2) {
        if (m2Renderer_) m2Renderer_->setInstanceTransform(transport.wmoInstanceId, transport.transform);
    } else {
        if (wmoRenderer_) {
            wmoRenderer_->setInstanceTransform(transport.wmoInstanceId, transport.transform);
            // Tell the static-world floor query this deck is in motion, so it
            // only counts as a floor when it is underfoot. Set here rather than
            // once at register so it survives an instance rebuild from
            // streaming - the call is idempotent and cheap.
            wmoRenderer_->setInstanceIsTransport(transport.wmoInstanceId, true);
        }
    }
}

void TransportManager::updateTransformMatrices(ActiveTransport& transport) {
    // Convert position from canonical to render coordinates for WMO rendering
    // Canonical: +X=North, +Y=West, +Z=Up
    // Render: renderX=wowY (west), renderY=wowX (north), renderZ=wowZ (up)
    glm::vec3 renderPos = core::coords::canonicalToRender(transport.position);

    // Convert rotation from canonical to render space using proper basis change
    // Canonical → Render is a 90° CCW rotation around Z (swaps X and Y)
    // Proper formula: q_render = q_basis * q_canonical * q_basis^-1
    glm::quat basisRotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    glm::quat basisInverse = glm::conjugate(basisRotation);
    glm::quat renderRot = basisRotation * transport.rotation * basisInverse;


    // Build transform matrix: translate * rotate * scale
    glm::mat4 translation = glm::translate(glm::mat4(1.0f), renderPos);
    glm::mat4 rotation = glm::mat4_cast(renderRot);
    glm::mat4 scale = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f));  // No scaling for transports

    transport.transform = translation * rotation * scale;
    transport.invTransform = glm::inverse(transport.transform);
}

void TransportManager::updateServerTransport(uint64_t guid, const glm::vec3& position, float orientation) {
    auto* transport = getTransport(guid);
    if (!transport) {
        LOG_WARNING("TransportManager::updateServerTransport: Transport not found: 0x", std::hex, guid, std::dec);
        return;
    }

    auto* pathEntry = pathRepo_.findPath(transport->pathId);

    // The facing the server gives it - a game object's own (0x00712db0) -
    // except on a ship this client sails along its route, which the client
    // turns along the route itself (0x007134a0) and the animator follows.
    if (!(transport->useClientAnimation && transport->worldCoords && !transport->isM2)) {
        transport->facing = core::coords::canonicalToServerYaw(orientation);
    }

    // MO_TRANSPORT spawn orientation is the authored dock alignment. Preserve
    // it before TaxiPathNode assignment replaces the stationary spawn path;
    // route tangent yaw is correct underway but points the bow into the pier
    // during a repeated-position dwell.
    if (!transport->isM2 && !transport->worldCoords) {
        transport->dockYaw = orientation;
        transport->hasDockYaw = true;
    }

    if (!pathEntry || pathEntry->spline.durationMs() == 0) {
        // No path or stationary - handle directly before delegating to ClockSync.
        // Still track update count so future path assignments work.
        transport->serverUpdateCount++;
        transport->lastServerUpdate = elapsedTime_;
        transport->basePosition = position;
        transport->position = position;
        transport->rotation = glm::angleAxis(transport->facing, glm::vec3(0.0f, 0.0f, 1.0f));
        updateTransformMatrices(*transport);
        pushTransform(*transport);
        return;
    }

    if (transport->isM2 && isDeeprunTramTransport(*transport) && pathEntry->fromDBC && isPreWotlk()) {
        // CMangos sends occasional position echoes for Deeprun subway cars, but the client
        // owns the TransportAnimation.dbc path phase. Treat those samples as presence/yaw
        // hints rather than switching the M2 tram into stationary server-driven mode.
        const bool firstUpdate = transport->serverUpdateCount == 0;
        transport->serverUpdateCount++;
        transport->lastServerUpdate = elapsedTime_;
        transport->useClientAnimation = true;
        transport->clientAnimationReverse = false;
        transport->hasServerClock = false;
        const glm::vec3 baseDelta = position - transport->basePosition;
        const bool stationEcho = glm::dot(baseDelta, baseDelta) < 4.0f;
        if (firstUpdate || stationEcho) {
            if (!seedDeeprunTramStationPhase(*transport, *pathEntry, position)) {
                transport->basePosition = position;
                transport->position = position;
                transport->localClockMs = 0;
                transport->hasServerYaw = false;
                transport->rotation = glm::angleAxis(orientation, glm::vec3(0.0f, 0.0f, 1.0f));
            }
        }
        if (transport->serverUpdateCount <= 3) {
            LOG_DEBUG("Deeprun tram server update kept client-driven: guid=0x", std::hex, guid, std::dec,
                        " entry=", transport->entry,
                        " displayId=", transport->displayId,
                        " pathId=", transport->pathId,
                        " pos=(", position.x, ",", position.y, ",", position.z, ")",
                        " orientation=", orientation);
        }
        updateTransformMatrices(*transport);
        pushTransform(*transport);
        return;
    }

    // Delegate clock sync, yaw correction, and velocity bootstrap to ClockSync.
    // A borrowed-route transport is handed no path at all: it still gets the
    // server's position and the heading-vs-velocity yaw correction, and none of
    // the phase seeding that would put it back on someone else's route.
    clockSync_.processServerUpdate(*transport,
                                   transport->borrowedPath ? nullptr : pathEntry,
                                   position, orientation, elapsedTime_);
    // The first sample reseeds a client-animated path from the wall clock.
    seedFromServerPathTime(*transport);

    updateTransformMatrices(*transport);
    pushTransform(*transport);
}

void TransportManager::rebindTransportInstance(uint64_t guid, uint32_t instanceId, bool isM2, uint32_t displayId) {
    auto* transport = getTransport(guid);
    if (!transport) return;

    const bool changed = transport->wmoInstanceId != instanceId ||
                         transport->isM2 != isM2 ||
                         (displayId != 0 && transport->displayId != displayId);
    transport->wmoInstanceId = instanceId;
    transport->isM2 = isM2;
    if (displayId != 0) {
        transport->displayId = displayId;
    }

    updateTransformMatrices(*transport);
    pushTransform(*transport);

    if (changed && isDeeprunTramTransport(*transport)) {
        LOG_INFO("Deeprun tram rebound to render instance: guid=0x", std::hex, guid, std::dec,
                    " instanceId=", instanceId,
                    " isM2=", isM2,
                    " displayId=", transport->displayId,
                    " pathId=", transport->pathId);
    }
}

bool TransportManager::loadTransportAnimationDBC(pipeline::AssetManager* assetMgr) {
    return pathRepo_.loadTransportAnimationDBC(assetMgr);
}

bool TransportManager::loadTransportRotationDBC(pipeline::AssetManager* assetMgr) {
    return pathRepo_.loadTransportRotationDBC(assetMgr);
}

void TransportManager::setParentRotation(uint64_t transportGuid, const glm::quat& rotation) {
    parentRotations_[transportGuid] = rotation;
    if (auto* transport = getTransport(transportGuid)) transport->parentRotation = rotation;
}

bool TransportManager::applyPathRotation(ActiveTransport& transport, uint32_t pathTimeMs) const {
    const auto track = pathRepo_.findRotationTrack(transport.entry);
    if (!track) return false;
    const auto rotation = transport_rotation::rotationAt(
        *track->keys, pathTimeMs + track->startMs, track->endMs, transport.parentRotation);
    if (!rotation) return false;
    // Its rotation now (0x004f43b0), so its facing is this one's
    // (0x004f4630) and its passengers' facings follow it. Drawn by it: the
    // hull's rotation is taken through the canonical basis turn that
    // updateTransformMatrices puts back, so the render matrix is the
    // client's - its axes are the wire's.
    transport.facing = core::coords::normalizeAngleRad(game_object_rotation::facing(*rotation));
    transport.pathRotation = *rotation;
    const glm::quat basis = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    transport.rotation = glm::conjugate(basis) * *rotation * basis;
    return true;
}

bool TransportManager::loadTaxiPathNodeDBC(pipeline::AssetManager* assetMgr) {
    return pathRepo_.loadTaxiPathNodeDBC(assetMgr);
}

bool TransportManager::hasTaxiPath(uint32_t taxiPathId) const {
    return pathRepo_.hasTaxiPath(taxiPathId);
}

bool TransportManager::hasTaxiPathForMap(uint32_t taxiPathId, uint32_t mapId) const {
    return pathRepo_.hasTaxiPathForMap(taxiPathId, mapId);
}

bool TransportManager::assignTaxiPathToTransport(uint32_t entry, uint32_t taxiPathId, uint32_t mapId) {
    auto* taxiEntry = pathRepo_.findTaxiPath(taxiPathId, mapId);
    if (!taxiEntry) {
        LOG_WARNING("No TaxiPathNode segment for taxiPathId=", taxiPathId, " on mapId=", mapId,
                    " (path exists on another map: ", pathRepo_.hasTaxiPath(taxiPathId), ")");
        return false;
    }

    // Assign to every transport with this entry. TaxiPathNode paths are absolute
    // world-coordinate routes (worldCoords=true, basePosition=0), so they are
    // independent of where the GO spawned. MO_TRANSPORT boats spawn at their dock
    // (a non-origin position), not at (0,0,0), so an origin-only gate here left
    // every continent-crossing ship without its route and falling back to an
    // unrelated TransportAnimation path (Deeprun Tram) instead.
    bool assignedAny = false;
    for (auto& [guid, transport] : transports_) {
        if (transport.entry != entry) continue;

        // Keep the authoritative spawn sample long enough to phase Kraken's
        // per-continent path segment. Seeding this route from wall-clock modulo
        // made its visual arrival unrelated to the server's map-transfer time.
        const glm::vec3 serverSpawnPosition = transport.position;

        // Copy the taxi path into the main paths (indexed by GO entry for this
        // transport), rebuilt at this hull's own speed when the server has
        // published a period to solve it from.
        //
        // The shared slice is timed at a nominal speed, and the authored dock
        // dwells do not scale with it. So a wrong speed does not merely make
        // the cycle the wrong length - the dwell lands in the wrong part of it,
        // and a phase the server publishes maps onto the wrong stretch of
        // route. Solved, the two timelines are proportional and the phase means
        // the same thing on both ends.
        const TaxiRoute* route = pathRepo_.findTaxiRoute(taxiPathId);
        PathEntry copied(taxiEntry->spline, entry, taxiEntry->zOnly,
                         /*dbc=*/false, taxiEntry->worldCoords);
        copied.presentMs = taxiEntry->presentMs;
        if (route && transport.hasServerRouteClock) {
            const float speed =
                TransportPathRepository::taxiRouteSpeedFor(*route, transport.routePeriodMs);
            if (speed > 0.0f) {
                PathEntry retimed =
                    TransportPathRepository::buildTaxiRouteSlice(*route, mapId, speed);
                if (retimed.spline.keyCount() >= 2) {
                    retimed.pathId = entry;
                    LOG_INFO("Transport entry=", entry, " route ", taxiPathId,
                             " retimed to the server's ", transport.routePeriodMs,
                             "ms period at ", speed, " yd/s");
                    copied = std::move(retimed);
                }
            }
        }
        pathRepo_.storePath(entry, std::move(copied));

        auto* storedEntry = pathRepo_.findPath(entry);

        // Update transport to use the new path
        transport.pathId = entry;
        transport.worldCoords = true;
        transport.borrowedPath = false;  // This route is its own now.
        transport.basePosition = glm::vec3(0.0f);  // World-coordinate path, no base offset
        transport.useClientAnimation = true;  // Server won't send position updates

        // Most legacy routes retain their established deterministic phase. Kraken
        // crosses map boundaries, so anchor each freshly loaded segment to the
        // server-authored spawn location; otherwise it can still be approaching
        // Borean Tundra when the server correctly transfers the rider back to map 0.
        if (storedEntry && storedEntry->spline.durationMs() > 0) {
            // Where the server says this hull is, not what time it happens to
            // be. A wall-clock modulo put the phase somewhere unrelated to the
            // route, so the zeppelin was departing its tower while the server
            // had it arriving at the other one. The server's own published
            // phase overrides this the moment it lands; this is what the hull
            // does until then, and the position the server spawned it at is
            // the only evidence there is.
            if (!transport.hasServerRouteClock && transport.serverUpdateCount > 0 &&
                !storedEntry->spline.keys().empty()) {
                const size_t nearest = storedEntry->spline.findNearestKey(serverSpawnPosition);
                transport.localClockMs = storedEntry->spline.keys()[nearest].timeMs;
            } else if (!transport.hasServerRouteClock) {
                transport.localClockMs = static_cast<uint32_t>(
                    nowEpochMs() % storedEntry->spline.durationMs());
            }
            transport.localClockMs %= storedEntry->spline.durationMs();
            transport.position = storedEntry->spline.evaluatePosition(transport.localClockMs);
            transport.onThisMap = storedEntry->presentAt(transport.localClockMs);
        }

        updateTransformMatrices(transport);
        pushTransform(transport);

        LOG_INFO("Assigned TaxiPathNode path to transport 0x", std::hex, guid, std::dec,
                 " entry=", entry, " taxiPathId=", taxiPathId,
                 " waypoints=", storedEntry ? storedEntry->spline.keyCount() : 0u,
                 " duration=", storedEntry ? storedEntry->spline.durationMs() : 0u, "ms",
                 " startPos=(", transport.position.x, ", ", transport.position.y, ", ", transport.position.z, ")");
        assignedAny = true;
    }

    if (!assignedAny) {
        LOG_DEBUG("No registered transport found for entry=", entry, " taxiPathId=", taxiPathId);
    }
    return assignedAny;
}

bool TransportManager::hasPathForEntry(uint32_t entry) const {
    return pathRepo_.hasPathForEntry(entry);
}

bool TransportManager::hasUsableMovingPathForEntry(uint32_t entry, float minXYRange) const {
    return pathRepo_.hasUsableMovingPathForEntry(entry, minXYRange);
}

uint32_t TransportManager::inferDbcPathForSpawn(const glm::vec3& spawnWorldPos,
                                               float maxDistance,
                                               bool allowZOnly) const {
    return pathRepo_.inferDbcPathForSpawn(spawnWorldPos, maxDistance, allowZOnly);
}

uint32_t TransportManager::inferMovingPathForSpawn(const glm::vec3& spawnWorldPos, float maxDistance) const {
    return pathRepo_.inferMovingPathForSpawn(spawnWorldPos, maxDistance);
}

uint32_t TransportManager::pickFallbackMovingPath(uint32_t entry, uint32_t displayId) const {
    return pathRepo_.pickFallbackMovingPath(entry, displayId);
}

} // namespace wowee::game
