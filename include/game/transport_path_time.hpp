#pragma once

#include "game/transport_path_repository.hpp"

namespace wowee::game {

/// Whether a transport's path runs on the server's milliseconds: every one of
/// TransportAnimation.dbc's does (0x00714250 keeps the create block's less the
/// clock, 0x00710640 adds the clock to it, modulo the path), a lift's and the
/// Deeprun Tram's alike; a taxi route's phase is its route fields'. Before 3.x
/// the tram's block carries none.
[[nodiscard]] inline bool runsOnServerTime(const PathEntry& path, bool preWotlk) {
    return path.fromDBC && !path.worldCoords && (path.zOnly || !preWotlk);
}

}  // namespace wowee::game
