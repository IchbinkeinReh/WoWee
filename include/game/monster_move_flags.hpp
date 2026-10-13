#pragma once

/// What a monster move does to the unit's movement flags (CMovement +0x44),
/// as CGUnit_C's monster-move handler (0x0073c8e0) and the spline's end
/// (0x006eb0b0) leave them. A spline moves the unit forward - backward on an
/// inverted one - and arriving takes that off again; a stop, or a move that
/// starts no spline, stops every way the unit was moving. Arithmetic only.

#include <cstdint>

namespace wowee::game::monster_move {

/// MoveSplineFlag bits the flags read.
inline constexpr uint32_t kSplineFalling = 0x200;
inline constexpr uint32_t kSplineParabolic = 0x800;
inline constexpr uint32_t kSplineCyclic = 0x80000;
inline constexpr uint32_t kSplineTransportEnter = 0x800000;
inline constexpr uint32_t kSplineTransportExit = 0x1000000;
inline constexpr uint32_t kSplineOrientInversed = 0x8000000;

/// 0x0073c8e0: a path shorter than this starts no spline.
inline constexpr float kMinSplineLength = 0.16666667f;

/// 0x006e9980: every way of moving stopped - falling first, a pending root
/// taken (0x00988490), then the pending moves, ascent and descent (0xfb303fff),
/// turning (0x00989010), pitching (0x00989450), strafing (0x00988ba0),
/// forward and back (0x0098bd10), falling (0xffffcfff).
[[nodiscard]] constexpr uint32_t stopped(uint32_t flags) {
    if ((flags & 0x1000u) != 0) flags &= 0xffffcfffu;
    if ((flags & 0x100000u) != 0) flags = (flags & 0xff203f00u) | 0x800u;
    flags &= 0xfb303fffu;
    flags &= ~0x30u;
    flags &= ~0xc0u;
    flags &= 0xffff7ff3u;
    flags &= 0xffffbffcu;
    flags &= 0xffffcfffu;
    return flags;
}

/// 0x006eb680: a spline is started unless it enters or leaves a transport,
/// or the unit is rooted (or about to be) and it neither falls nor arcs.
[[nodiscard]] constexpr bool splineStarts(uint32_t flags, uint32_t splineFlags) {
    if ((splineFlags & (kSplineTransportEnter | kSplineTransportExit)) != 0) return false;
    return (flags & 0x100800u) == 0 || (splineFlags & (kSplineFalling | kSplineParabolic)) != 0;
}

/// 0x0098c770 -> 0x00988a20(!inverted, 1): the pending stop and pending
/// forward/back go, and the unit moves forward - backward on an inverted
/// spline; a rooted unit on a falling or arcing one has its root pending
/// instead (0x100000).
[[nodiscard]] constexpr uint32_t splineStarted(uint32_t flags, uint32_t splineFlags) {
    flags &= 0xfffcbfffu;
    if ((splineFlags & kSplineOrientInversed) != 0) flags = (flags & ~0x1u) | 0x2u;
    else flags = (flags & ~0x2u) | 0x1u;
    if ((flags & 0x800u) != 0 && (splineFlags & (kSplineFalling | kSplineParabolic)) != 0)
        flags = (flags & ~0x800u) | 0x100000u;
    return flags;
}

/// 0x006eb0b0 -> 0x0098bd10: arriving at the end of a spline that is not
/// cyclic, forward and back (and a pending stop) are taken off.
[[nodiscard]] constexpr uint32_t arrived(uint32_t flags) { return flags & 0xffffbffcu; }

/// 0x0073c8e0 as a whole. `onSpline` - the unit still on an unfinished
/// spline - keeps its flags as they are before the new one; otherwise
/// everything stops first (0x006ed7e0). `startsSpline` - a move that is not
/// a stop, long enough (kMinSplineLength) - starts it where splineStarts
/// lets it, else the unit stops where it is sent.
[[nodiscard]] constexpr uint32_t afterMonsterMove(uint32_t flags, bool onSpline, bool startsSpline,
                                                  uint32_t splineFlags) {
    if (!onSpline) flags = stopped(flags);
    if (startsSpline && splineStarts(flags, splineFlags)) return splineStarted(flags, splineFlags);
    return stopped(flags);
}

}  // namespace wowee::game::monster_move
