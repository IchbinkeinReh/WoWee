#pragma once

// When the loading screen kept over the world's first frames may go
// (Application::stepWorldSettle).
namespace wowee::core::world_settle {

/// A frame this long or shorter is an ordinary one.
inline constexpr float kCalmFrameMs = 45.0f;
/// This many of them in a row, with nothing left to spawn, is settled.
inline constexpr int kCalmFramesNeeded = 30;
/// Never shorter: the first frames are the ones that do the work, and a
/// lucky few early ones are not a settled world.
inline constexpr float kMinSeconds = 0.3f;
/// Never longer, whatever the frames do.
inline constexpr float kMaxSeconds = 8.0f;
/// But while what the world is drawn with is still compiled beside them - the
/// traced water's pipeline, once a driver's cache.
inline constexpr float kMaxCompileSeconds = 30.0f;

/// Why the screen may go now, or nullptr while it stays.
[[nodiscard]] constexpr const char* verdict(float elapsedSeconds, int calmFrames, bool compiling = false) {
    if (elapsedSeconds >= (compiling ? kMaxCompileSeconds : kMaxSeconds)) return "capped";
    if (compiling) return nullptr;
    if (elapsedSeconds >= kMinSeconds && calmFrames >= kCalmFramesNeeded) return "frames settled";
    return nullptr;
}

}  // namespace wowee::core::world_settle
