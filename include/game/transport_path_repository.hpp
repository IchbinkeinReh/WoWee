// include/game/transport_path_repository.hpp
// Owns and manages transport path data - DBC, taxi, and custom paths.
// Uses CatmullRomSpline for spline evaluation (replaces duplicated evalTimedCatmullRom).
// Separated from TransportManager for SOLID-S (single responsibility).
#pragma once

#include "math/spline.hpp"
#include "game/transport_rotation.hpp"
#include <cstdint>
#include <optional>
#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>

namespace wowee::pipeline {
    class AssetManager;
}

namespace wowee::game {

/// Whether a displayId belongs to a vehicle that travels a route - a ship, a
/// zeppelin, an icebreaker, a gunship - as opposed to an elevator or a lift.
///
/// Both are transports and both animate, but they want opposite treatment: a
/// vehicle's route is long and mostly horizontal and often comes from the
/// server, while a lift travels a few metres straight up.
///
/// Named because the numbers were being written out at three call sites and
/// each list had picked up elevator displayIds along the way. 807 and 808 are
/// Gnomeregan's two lifts, 2454 the Searing Gorge scaffold cars, 1587 a
/// GameObject named "Elevator" - every one of them was being treated as a ship
/// or drawn as an airship. Values verified against gameobject_template: these
/// are the displayIds carried by type 15 (MO_TRANSPORT) rows.
constexpr bool isVehicleTransportDisplay(uint32_t displayId) {
    switch (displayId) {
        case 3015:  // ship
        case 3031:  // zeppelin
        case 6637:  // Naxxramas
        case 7087:  // night elf ship
        case 7446:  // icebreaker
        case 7546:  // horde zeppelin
        case 7570:  // Sister Mercy
        case 7636:  // turtle
        case 8253: case 8254:   // Orgrim's Hammer / The Skybreaker
        case 9001: case 9002:   // gunships
        case 9150: case 9151:   // Icecrown airships
            return true;
        default:
            return false;
    }
}

/// Vehicles whose route is a taxi path the server assigns, rather than a
/// TransportAnimation.dbc loop. Borrowing an animation path for one of these
/// sends it underwater, so they stay docked until the route arrives.
constexpr bool isOceanGoingTransportDisplay(uint32_t displayId) {
    switch (displayId) {
        case 3015:  // ship
        case 7087:  // night elf ship
        case 7446:  // icebreaker
        case 7636:  // turtle
            return true;
        default:
            return false;
    }
}

/// Metadata + CatmullRomSpline for a transport path.
struct PathEntry {
    math::CatmullRomSpline spline;
    uint32_t pathId = 0;
    bool zOnly = false;       // Elevator/bobbing - no meaningful XY travel
    bool fromDBC = false;     // Loaded from TransportAnimation.dbc
    bool worldCoords = false; // TaxiPathNode absolute world positions (not local offsets)
    /// TransportAnimation.dbc keys, which the client runs straight from one
    /// to the next (0x0070daa0 lerps the two nodes either side of the time),
    /// over a period that ends at the last key (0x00710640 takes the time
    /// modulo the last node's TimeIndex).
    bool linear = false;
    /// Each TransportAnimation key's SequenceID (its field 6), by the key's
    /// time - what the model plays from that key to the next (0x0070daa0).
    std::vector<std::pair<uint32_t, uint32_t>> keySequences;

    /// The sequence the model plays at `timeMs`: the one of the key the
    /// segment starts at, as 0x0070daa0 takes it. 0 without any.
    [[nodiscard]] uint32_t sequenceAt(uint32_t timeMs) const {
        uint32_t seq = 0;
        for (const auto& [t, s] : keySequences) {
            if (t > timeMs) break;
            seq = s;
        }
        return seq;
    }

    /// Where the path has the transport at a time in [0, duration].
    [[nodiscard]] glm::vec3 positionAt(uint32_t timeMs) const {
        if (!linear) return spline.evaluatePosition(timeMs);
        const auto& keys = spline.keys();
        if (keys.empty()) return glm::vec3(0.0f);
        if (keys.size() == 1 || timeMs <= keys.front().timeMs) return keys.front().position;
        if (timeMs >= keys.back().timeMs) return keys.back().position;
        size_t i = 1;
        while (i + 1 < keys.size() && keys[i].timeMs <= timeMs) ++i;
        const auto& a = keys[i - 1];
        const auto& b = keys[i];
        const uint32_t span = b.timeMs - a.timeMs;
        if (span == 0) return b.position;
        const float t = static_cast<float>(timeMs - a.timeMs) / static_cast<float>(span);
        return a.position * (1.0f - t) + b.position * t;
    }

    /// Spans of the route's cycle during which the hull is on this slice's map.
    ///
    /// A taxi route crosses continents - the Undercity zeppelin spends half
    /// its cycle over Howling Fjord - and the slice held here covers the whole
    /// cycle so the server's published phase maps onto it directly. Outside
    /// these spans the hull is on another map and must not be drawn: the
    /// spline holds it still there, and the renderer hides it.
    ///
    /// Empty means always present, which is every path that never leaves one
    /// map and every TransportAnimation route.
    std::vector<std::pair<uint32_t, uint32_t>> presentMs;

    [[nodiscard]] bool presentAt(uint32_t timeMs) const {
        if (presentMs.empty()) return true;
        for (const auto& [from, to] : presentMs) {
            if (timeMs >= from && timeMs <= to) return true;
        }
        return false;
    }

    PathEntry(math::CatmullRomSpline s, uint32_t id, bool zo, bool dbc, bool wc)
        : spline(std::move(s)), pathId(id), zOnly(zo), fromDBC(dbc), worldCoords(wc) {}
};

/// One node of a taxi route, as TaxiPathNode.dbc authored it.
struct TaxiRouteNode {
    glm::vec3 position;      ///< canonical world coordinates
    uint32_t mapId = 0;
    uint32_t dwellMs = 0;    ///< authored stop at this node
};

/// A transport's whole route, across every map it touches.
///
/// Kept whole rather than sliced per map, because the route's timeline is what
/// the server publishes a phase against. Sliced first and timed separately,
/// each map's piece got a cycle of its own and the hull flew laps of one shore
/// while the server's schedule ran elsewhere.
struct TaxiRoute {
    std::vector<TaxiRouteNode> nodes;   ///< NodeIndex order
    /// The route closes back on itself: the last node leads straight to the
    /// first. False for an open A-to-B run, which the hull flies out and back.
    bool circuit = false;
    /// Distance covered in one cycle, and time stopped in one cycle. A map
    /// change covers no distance - the server teleports the hull across.
    /// Together with a published route period these give the hull's real
    /// speed, which is otherwise a guess.
    float cycleDistance = 0.0f;
    uint32_t cycleDwellMs = 0;
};

/// Owns and manages transport path data.
class TransportPathRepository {
public:
    TransportPathRepository() = default;

    // ── DBC loading ─────────────────────────────────────────
    bool loadTransportAnimationDBC(pipeline::AssetManager* assetMgr);
    /// TransportRotation.dbc: id, the game object's entry, the time, then
    /// x, y, z, w - each entry's rows in the order the file has them.
    bool loadTransportRotationDBC(pipeline::AssetManager* assetMgr);
    void storeRotationKeys(uint32_t entry, std::vector<transport_rotation::Key> keys) {
        rotations_[entry] = std::move(keys);
    }

    /// An entry's turn along its path, on its TransportAnimation timeline.
    struct RotationTrack {
        const std::vector<transport_rotation::Key>* keys = nullptr;
        /// The path's first and last node times as the DBC has them. Paths
        /// here start at 0 (startMs is taken off), the client's at the file's
        /// own times, and its last segment runs to the last node.
        uint32_t startMs = 0;
        uint32_t endMs = 0;
    };
    /// None for an entry with fewer than two rows: it does not turn.
    [[nodiscard]] std::optional<RotationTrack> findRotationTrack(uint32_t entry) const;
    bool loadTaxiPathNodeDBC(pipeline::AssetManager* assetMgr);

    // ── Path construction ───────────────────────────────────
    void loadPathFromNodes(uint32_t pathId, const std::vector<glm::vec3>& waypoints,
                           bool looping = true, float speed = 18.0f);

    // ── Lookup ──────────────────────────────────────────────
    [[nodiscard]] const PathEntry* findPath(uint32_t pathId) const;
    // Taxi paths are stored per-map: a continent-crossing boat path has nodes on
    // two maps, and only the segment on the transport's current map is valid world
    // geometry. mapId selects that segment.
    [[nodiscard]] const PathEntry* findTaxiPath(uint32_t taxiPathId, uint32_t mapId) const;
    [[nodiscard]] bool hasPathForEntry(uint32_t entry) const;
    [[nodiscard]] bool hasTaxiPath(uint32_t taxiPathId) const;              // exists on any map
    [[nodiscard]] bool hasTaxiPathForMap(uint32_t taxiPathId, uint32_t mapId) const;

    // ── Query ───────────────────────────────────────────────
    [[nodiscard]] bool hasUsableMovingPathForEntry(uint32_t entry, float minXYRange = 1.0f) const;
    [[nodiscard]] uint32_t inferDbcPathForSpawn(const glm::vec3& spawnWorldPos, float maxDistance,
                                  bool allowZOnly) const;
    [[nodiscard]] uint32_t inferMovingPathForSpawn(const glm::vec3& spawnWorldPos,
                                     float maxDistance = 1200.0f) const;
    [[nodiscard]] uint32_t pickFallbackMovingPath(uint32_t entry, uint32_t displayId) const;

    // ── Mutation ─────────────────────────────────────────────
    /// Store or overwrite a path entry (used by assignTaxiPathToTransport).
    void storePath(uint32_t pathId, PathEntry entry);

    // Build the cyclic CatmullRomSpline for one map's slice of a TaxiPathNode route.
    //
    // The transport clock samples the result modulo its duration, so the slice must be
    // position-closed (end where it began) or the hull teleport-snaps at the wrap. Two
    // topologies:
    //   - closed loop (endpoints coincide): append the first point to close the ring;
    //   - open route: append the outbound points in reverse so the slice is one continuous
    //     there-and-back ferry (offshore-in -> dock -> offshore-out -> U-turn -> back).
    // A slice is NEVER made to hold stationary offshore for the time the boat "spends" on
    // another continent: a rider aboard experiences that as the boat sitting dead at sea.
    // The cross-continent handoff is the server's SMSG_NEW_WORLD teleport at the offshore
    // node, independent of this client animation. pts are canonical world positions in
    // NodeIndex order; nodeDelaysMs is the authored per-node dock dwell in milliseconds
    // (0 where none), matched by index to pts. Static and dependency-free so the wrap
    // behaviour can be unit-tested without a DBC.
    // fullRouteCycleMs is the whole route's period across every map it touches,
    // in the same terms this function accounts in (legs twice, dwells once).
    // Anything left over after this slice's own cost is spent waiting at the
    // pier, so the boat makes one departure per server cycle rather than lapping
    // its shore until the transfer comes due. 0 disables the stretch.
    /// One map's view of a whole taxi route, over the route's own timeline.
    ///
    /// Every slice of a route shares one cycle length and one phase, so a
    /// phase the server publishes lands in the same place on every map. Where
    /// the route is on another map the hull holds still and `presentMs` says
    /// it is not here.
    ///
    /// speed is yards per second. Pass the hull's real speed when it is known
    /// - solved from the server's published period, see taxiRouteSpeedFor -
    /// because the authored dwells are absolute and the legs are not, so a
    /// wrong speed does not merely stretch the cycle, it moves the dock stop
    /// relative to it.
    ///
    /// Static and dependency-free so the timeline can be unit-tested without
    /// a DBC.
    [[nodiscard]] static PathEntry buildTaxiRouteSlice(const TaxiRoute& route,
                                                      uint32_t mapId,
                                                      float speed = kDefaultTransportSpeed);

    /// Whether the route closes back on itself rather than running A to B.
    ///
    /// A route that ends on a different map than it began always does: the
    /// closing leg is the server's teleport home. On one map it is measured
    /// against the route's own length - the Undercity zeppelin comes in from
    /// the north-east, docks, circles the tower and leaves the same way, its
    /// ends 118 units apart on a 787-unit circuit, while a harbour shuttle's
    /// ends are most of its length apart.
    [[nodiscard]] static bool taxiRouteIsCircuit(const TaxiRoute& route);

    /// The speed that makes this route take periodMs, or 0 if there is none.
    ///
    /// The dwells are fixed and the legs are not, so this is the only way to
    /// make the client's timeline proportional to the server's rather than
    /// merely the same length.
    [[nodiscard]] static float taxiRouteSpeedFor(const TaxiRoute& route, uint32_t periodMs);

    [[nodiscard]] const TaxiRoute* findTaxiRoute(uint32_t taxiPathId) const;

    static constexpr float kDefaultTransportSpeed = 28.0f;


private:
    std::unordered_map<uint32_t, PathEntry> paths_;
    // taxiPathId -> mapId -> world-coordinate path segment for that map.
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, PathEntry>> taxiPaths_;
    // taxiPathId -> the whole route, every map, as authored. Kept so a slice
    // can be rebuilt at the hull's real speed once the server publishes its
    // route period.
    std::unordered_map<uint32_t, TaxiRoute> taxiRoutes_;
    // TransportRotation.dbc rows by game object entry.
    std::unordered_map<uint32_t, std::vector<transport_rotation::Key>> rotations_;
    // TransportAnimation.dbc's first and last node times by entry.
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> animationSpans_;
};

} // namespace wowee::game
