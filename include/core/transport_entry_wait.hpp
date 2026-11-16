#pragma once

namespace wowee {
namespace core {

/// What a passenger's transport says about the ground at the end of a world
/// load (WorldLoader's warmup).
///
/// On a transport the transport is the floor: no terrain or building answers
/// for a deck over the sea or in the air. Carried to another map on one, the
/// player is on it only once the transport is known on the new map
/// (completePlayerTransportWorldTransfer); until then the place the load was
/// asked for is the offset on the deck taken as a world position - the map's
/// origin - and the world is not entered there. The wait has a cap, after
/// which the ordinary floor checks decide.
struct TransportEntryGround {
    bool groundReady = false;     ///< the transport is the floor
    bool awaitTransport = false;  ///< keep the loading screen up for it
};

inline constexpr float kMaxTransportEntryWaitSeconds = 20.0f;

inline TransportEntryGround transportEntryGround(bool onTransport, bool transferPending, float elapsedSeconds) {
    TransportEntryGround g;
    g.groundReady = onTransport && !transferPending;
    g.awaitTransport = transferPending && elapsedSeconds < kMaxTransportEntryWaitSeconds;
    return g;
}

}  // namespace core
}  // namespace wowee
