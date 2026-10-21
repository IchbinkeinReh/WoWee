// src/game/transport_clock_sync.cpp
// Clock synchronization and yaw correction for transports.
// Extracted from TransportManager (Phase 3a of spline refactoring).
#include "game/transport_clock_sync.hpp"
#include "game/transport_manager.hpp"
#include "game/transport_path_repository.hpp"
#include "core/coordinates.hpp"
#include "math/spline.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <glm/gtc/constants.hpp>
#include <cmath>

namespace wowee::game {

bool TransportClockSync::computePathTime(
    ActiveTransport& transport,
    const math::CatmullRomSpline& spline,
    double elapsedTime,
    float deltaTime,
    uint32_t& outPathTimeMs) const
{
    uint32_t nowMs = static_cast<uint32_t>(elapsedTime * 1000.0);
    uint32_t durationMs = spline.durationMs();

    // The server's own route clock wins over anything the client worked out for
    // itself. It publishes the phase as a fraction of the route period, so it
    // maps onto whatever timeline this spline happens to have without needing the
    // two periods to agree - and it keeps agreeing as the ride goes on, because
    // the phase advances at the server's rate rather than one derived from
    // distance over speed.
    //
    // Without it the client picked its own period, and a ferry whose period came
    // out shorter than the server's simply lapped its shore until the server's
    // schedule caught up.
    if (transport.hasServerRouteClock && transport.routePeriodMs > 0 && durationMs > 0) {
        const double sinceSampleMs =
            std::max(0.0, (elapsedTime - transport.routePhaseAtTime) * 1000.0);
        const double serverMs =
            static_cast<double>(transport.routePhase) * transport.routePeriodMs + sinceSampleMs;
        double wrapped = std::fmod(serverMs, static_cast<double>(transport.routePeriodMs));
        if (wrapped < 0.0) wrapped += transport.routePeriodMs;
        const double fraction = wrapped / static_cast<double>(transport.routePeriodMs);
        outPathTimeMs = static_cast<uint32_t>(fraction * durationMs) % durationMs;
        return true;
    }

    if (transport.hasServerClock) {
        // Predict server time using clock offset (works for both client and server-driven modes)
        int64_t serverTimeMs = static_cast<int64_t>(nowMs) + transport.serverClockOffsetMs;
        int64_t mod = static_cast<int64_t>(durationMs);
        int64_t wrapped = serverTimeMs % mod;
        if (wrapped < 0) wrapped += mod;
        outPathTimeMs = static_cast<uint32_t>(wrapped);
        return true;
    }

    if (transport.useClientAnimation && transport.hasPathClockAnchor &&
        !transport.clientAnimationReverse) {
        // A lift on the server's path time: the clock plus its offset, modulo
        // the path (0x00710640).
        const int64_t mod = static_cast<int64_t>(durationMs);
        int64_t t = (TransportManager::nowSteadyMs() + transport.pathClockAnchorMs) % mod;
        if (t < 0) t += mod;
        transport.localClockMs = static_cast<uint32_t>(t);
        outPathTimeMs = transport.localClockMs;
        return true;
    }

    if (transport.useClientAnimation) {
        // Pure local clock (no server sync yet, client-driven), counting
        // whole milliseconds and carrying the rest to the next frame.
        transport.localClockRemainderMs += std::max(0.0, static_cast<double>(deltaTime) * 1000.0);
        uint32_t dtMs = static_cast<uint32_t>(transport.localClockRemainderMs);
        transport.localClockRemainderMs -= dtMs;
        if (!transport.clientAnimationReverse) {
            transport.localClockMs += dtMs;
        } else {
            if (dtMs > durationMs) {
                dtMs %= durationMs;
            }
            if (transport.localClockMs >= dtMs) {
                transport.localClockMs -= dtMs;
            } else {
                transport.localClockMs = durationMs - (dtMs - transport.localClockMs);
            }
        }
        outPathTimeMs = transport.localClockMs % durationMs;
        return true;
    }

    // Strict server-authoritative mode: do not guess movement between server snapshots.
    return false;
}

void TransportClockSync::processServerUpdate(
    ActiveTransport& transport,
    const PathEntry* pathEntry,
    const glm::vec3& position,
    float orientation,
    double elapsedTime)
{
    const bool hadPrevUpdate = (transport.serverUpdateCount > 0);
    const double prevUpdateTime = transport.lastServerUpdate;
    const glm::vec3 prevPos = transport.position;

    const bool hasPath = (pathEntry != nullptr);
    const bool isZOnlyPath = (hasPath && pathEntry->fromDBC && pathEntry->zOnly && pathEntry->spline.durationMs() > 0);
    const bool isWorldCoordPath = (hasPath && pathEntry->worldCoords && pathEntry->spline.durationMs() > 0);

    // Don't let (0,0,0) server updates override a TaxiPathNode world-coordinate path
    if (isWorldCoordPath && glm::dot(position, position) < 1.0f) {
        transport.serverUpdateCount++;
        transport.lastServerUpdate = elapsedTime;
        transport.serverYaw = orientation;
        transport.hasServerYaw = true;
        return;
    }

    // Track server updates
    transport.serverUpdateCount++;
    transport.lastServerUpdate = elapsedTime;
    // Z-only elevators and world-coordinate paths (TaxiPathNode) always stay client-driven.
    // For other DBC paths (trams, ships): only switch to server-driven mode when the server
    // sends a position that actually differs from the current position, indicating it's
    // actively streaming movement data (not just echoing the spawn position).
    if (isZOnlyPath || isWorldCoordPath) {
        transport.useClientAnimation = true;
    } else if (transport.useClientAnimation && hasPath && pathEntry->fromDBC) {
        glm::vec3 pd = position - transport.position;
        float posDeltaSq = glm::dot(pd, pd);
        if (posDeltaSq > 1.0f) {
            // Server sent a meaningfully different position - it's actively driving this transport
            transport.useClientAnimation = false;
            LOG_INFO("Transport 0x", std::hex, transport.guid, std::dec,
                     " switching to server-driven (posDeltaSq=", posDeltaSq, ")");
        }
        // Otherwise keep client animation (server just echoed spawn pos or sent small jitter)
    } else if (!hasPath || !pathEntry->fromDBC) {
        // No DBC path - purely server-driven
        transport.useClientAnimation = false;
    }
    transport.clientAnimationReverse = false;

    // Server-authoritative transport mode:
    // Trust explicit server world position/orientation directly for all moving transports.
    transport.hasServerClock = false;
    if (transport.serverUpdateCount == 1) {
        // Seed once from first authoritative update; keep stable base for fallback phase estimation.
        // For z-only elevator paths, keep the spawn-derived basePosition (the DBC path is local offsets).
        if (!isZOnlyPath) {
            transport.basePosition = position;
        }
    }
    transport.position = position;
    transport.serverYaw = orientation;
    transport.hasServerYaw = true;
    // Drawn by its facing, as the client draws a game object by its rotation
    // (0x00712ee0), so the hull and its passengers agree.
    transport.rotation = glm::angleAxis(transport.facing, glm::vec3(0.0f, 0.0f, 1.0f));

    if (hadPrevUpdate) {
        const double dt = elapsedTime - prevUpdateTime;
        if (dt > 0.001) {
            glm::vec3 v = (position - prevPos) / static_cast<float>(dt);
            float speedSq = glm::dot(v, v);
            constexpr float kMinAuthoritativeSpeed = 0.15f;
            constexpr float kMaxSpeed = 60.0f;
            if (speedSq >= kMinAuthoritativeSpeed * kMinAuthoritativeSpeed) {
                if (speedSq > kMaxSpeed * kMaxSpeed) {
                    v *= (kMaxSpeed * glm::inversesqrt(speedSq));
                }

                transport.serverLinearVelocity = v;
                transport.serverAngularVelocity = 0.0f;
                transport.hasServerVelocity = true;
            }
        }
    } else {
        // Seed fallback path phase from the first authoritative sample.
        if (pathEntry && pathEntry->spline.keyCount() > 0 && pathEntry->spline.durationMs() > 0) {
            if (transport.useClientAnimation) {
                // Client-animated transports (no real position stream - see
                // TransportManager::updateServerTransport's Deeprun-tram branch and
                // comment on nowEpochMs()) need a phase anchored to absolute time, not
                // to whichever waypoint happens to geometrically match wherever this
                // first echo landed. The previous "nearest waypoint" seed here made
                // this transport's phase depend on incidental details of its first
                // update rather than real elapsed time - same bug as the two other
                // seed sites, just reached by a different path (this fires whenever
                // this is the first authoritative sample for a transport
                // TransportManager::registerTransport()/resolveAndRegisterSpawn()
                // didn't already classify into the Deeprun-specific early-return
                // branch in updateServerTransport()).
                const uint32_t seedDurationMs = TransportManager::isDeeprunTramTransport(transport)
                    ? TransportManager::deeprunTramSeedDurationMs(pathEntry->spline.durationMs())
                    : pathEntry->spline.durationMs();
                transport.localClockMs = static_cast<uint32_t>(TransportManager::nowEpochMs() % seedDurationMs);
            } else {
                // Nearest-waypoint seed is legitimate here: this transport is
                // genuinely server-position-driven (e.g. a boat), so "which point on
                // the path does our first real position correspond to" is a real
                // question with a real geometric answer.
                glm::vec3 local = position - transport.basePosition;
                size_t bestIdx = pathEntry->spline.findNearestKey(local);
                transport.localClockMs = pathEntry->spline.keys()[bestIdx].timeMs % pathEntry->spline.durationMs();
            }
        }

        // Bootstrap velocity from mapped DBC path on first authoritative sample.
        if (transport.allowBootstrapVelocity && pathEntry && pathEntry->spline.keyCount() >= 2 && pathEntry->spline.durationMs() > 0) {
            bootstrapVelocityFromPath(transport, *pathEntry);
        } else if (!transport.allowBootstrapVelocity) {
            LOG_INFO("Transport 0x", std::hex, transport.guid, std::dec,
                     " DBC bootstrap velocity disabled for this transport");
        }
    }
}

void TransportClockSync::bootstrapVelocityFromPath(
    ActiveTransport& transport,
    const PathEntry& pathEntry) const
{
    // Bootstrap velocity from nearest DBC path segment on first authoritative sample.
    // This avoids "stalled at dock" when server sends sparse transport snapshots.
    const auto& keys = pathEntry.spline.keys();
    glm::vec3 local = transport.position - transport.basePosition;
    size_t bestIdx = pathEntry.spline.findNearestKey(local);

    float bestDistSq = 0.0f;
    {
        glm::vec3 d = keys[bestIdx].position - local;
        bestDistSq = glm::dot(d, d);
    }

    constexpr float kMaxBootstrapNearestDist = 80.0f;
    if (bestDistSq > (kMaxBootstrapNearestDist * kMaxBootstrapNearestDist)) {
        LOG_WARNING("Transport 0x", std::hex, transport.guid, std::dec,
                    " skipping DBC bootstrap velocity: nearest path point too far (dist=",
                    std::sqrt(bestDistSq), ", path=", transport.pathId, ")");
        return;
    }

    size_t n = keys.size();
    uint32_t durMs = pathEntry.spline.durationMs();
    constexpr float kMinBootstrapSpeed = 0.25f;
    constexpr float kMaxSpeed = 60.0f;

    auto tryApplySegment = [&](size_t a, size_t b) {
        uint32_t t0 = keys[a].timeMs;
        uint32_t t1 = keys[b].timeMs;
        if (b == 0 && t1 <= t0 && durMs > 0) {
            t1 = durMs;
        }
        if (t1 <= t0) return;
        glm::vec3 seg = keys[b].position - keys[a].position;
        float dtSeg = static_cast<float>(t1 - t0) / 1000.0f;
        if (dtSeg <= 0.001f) return;
        glm::vec3 v = seg / dtSeg;
        float speedSq = glm::dot(v, v);
        if (speedSq < kMinBootstrapSpeed * kMinBootstrapSpeed) return;
        if (speedSq > kMaxSpeed * kMaxSpeed) {
            v *= (kMaxSpeed * glm::inversesqrt(speedSq));
        }
        transport.serverLinearVelocity = v;
        transport.serverAngularVelocity = 0.0f;
        transport.hasServerVelocity = true;
    };

    // Prefer nearest forward meaningful segment from bestIdx.
    for (size_t step = 1; step < n && !transport.hasServerVelocity; ++step) {
        size_t a = (bestIdx + step - 1) % n;
        size_t b = (bestIdx + step) % n;
        tryApplySegment(a, b);
    }
    // Fallback: nearest backward meaningful segment.
    for (size_t step = 1; step < n && !transport.hasServerVelocity; ++step) {
        size_t b = (bestIdx + n - step + 1) % n;
        size_t a = (bestIdx + n - step) % n;
        tryApplySegment(a, b);
    }

    if (transport.hasServerVelocity) {
        LOG_INFO("Transport 0x", std::hex, transport.guid, std::dec,
                 " bootstrapped velocity from DBC path ", transport.pathId,
                 " v=(", transport.serverLinearVelocity.x, ", ",
                 transport.serverLinearVelocity.y, ", ",
                 transport.serverLinearVelocity.z, ")");
    } else {
        LOG_INFO("Transport 0x", std::hex, transport.guid, std::dec,
                 " skipped DBC bootstrap velocity (segment too short/static)");
    }
}

} // namespace wowee::game
